#include "booking_engine.hpp"
#include <iostream>

namespace ticket_engine {

BookingEngine::BookingEngine(uint32_t total_seats, const std::string& wal_path)
    : total_seats_(total_seats),
      available_count_(static_cast<int32_t>(total_seats)),
      stripe_mutexes_(NUM_STRIPES) {
    
    seats_.resize(total_seats_);
    for (uint32_t i = 0; i < total_seats_; ++i) {
        seats_[i].seat_id = i + 1;
        seats_[i].state = SeatState::AVAILABLE;
    }

    wal_ = std::make_unique<WriteAheadLog>(wal_path);

    reaper_ = std::make_unique<Reaper>([this](uint64_t reservation_id, uint32_t seat_id) {
        this->handle_expiry(reservation_id, seat_id);
    });
    reaper_->start();

    // Default policy: 5 burst tokens, 2 tokens/sec refill, sharded across 32 buckets
    rate_limiter_ = std::make_unique<RateLimiter>(5.0, 2.0, 32);
}

BookingEngine::~BookingEngine() {
    if (reaper_) {
        reaper_->stop();
    }
}

ReservationResult BookingEngine::reserve_seat(uint32_t user_id, uint32_t seat_id, uint32_t ttl_ms, bool apply_rate_limit) {
    total_reservations_attempted_.fetch_add(1, std::memory_order_relaxed);

    // Tier 0: Token Bucket Rate Limiter (Anti-Bot / Fair Allocation Guard)
    if (apply_rate_limit && rate_limiter_ && !rate_limiter_->allow_request(user_id)) {
        total_rate_limited_.fetch_add(1, std::memory_order_relaxed);
        return {false, ReservationStatus::RATE_LIMITED, 0, seat_id};
    }

    if (seat_id == 0 || seat_id > total_seats_) {
        return {false, ReservationStatus::INVALID_SEAT, 0, seat_id};
    }

    // Tier 1: Fast-Path Atomic Filter (rejects if sold out in nanoseconds)
    if (available_count_.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        available_count_.fetch_add(1, std::memory_order_relaxed);
        return {false, ReservationStatus::SOLD_OUT, 0, seat_id};
    }

    // Tier 2: Striped Lock for specific seat
    size_t stripe = get_stripe_index(seat_id);
    {
        std::lock_guard<std::mutex> lock(stripe_mutexes_[stripe].mtx);
        Seat& seat = seats_[seat_id - 1];

        if (seat.state != SeatState::AVAILABLE) {
            // Revert fast-path counter since this specific seat was already taken
            available_count_.fetch_add(1, std::memory_order_relaxed);
            return {false, ReservationStatus::SEAT_TAKEN, 0, seat_id};
        }

        uint64_t res_id = reservation_id_generator_.fetch_add(1, std::memory_order_relaxed);
        seat.state = SeatState::HELD;
        seat.user_id = user_id;
        seat.reservation_id = res_id;
        seat.hold_expiry = Clock::now() + std::chrono::milliseconds(ttl_ms);

        // Write to WAL first before acknowledging
        wal_->append_record(WalAction::HOLD, user_id, seat_id, res_id);

        // Register with reaper for TTL expiry
        reaper_->schedule_expiry(res_id, seat_id, ttl_ms);

        return {true, ReservationStatus::SUCCESS, res_id, seat_id};
    }
}

ReservationResult BookingEngine::reserve_any_seat(uint32_t user_id, uint32_t ttl_ms, bool apply_rate_limit) {
    total_reservations_attempted_.fetch_add(1, std::memory_order_relaxed);

    // Tier 0: Rate Limiter
    if (apply_rate_limit && rate_limiter_ && !rate_limiter_->allow_request(user_id)) {
        total_rate_limited_.fetch_add(1, std::memory_order_relaxed);
        return {false, ReservationStatus::RATE_LIMITED, 0, 0};
    }

    // Fast-path atomic check
    if (available_count_.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        available_count_.fetch_add(1, std::memory_order_relaxed);
        return {false, ReservationStatus::SOLD_OUT, 0, 0};
    }

    // Fast random offset based on user_id to minimize collision on seat #1
    uint32_t start_offset = (user_id * 179424673u) % total_seats_;

    for (uint32_t i = 0; i < total_seats_; ++i) {
        uint32_t candidate_seat_id = ((start_offset + i) % total_seats_) + 1;
        size_t stripe = get_stripe_index(candidate_seat_id);

        std::unique_lock<std::mutex> lock(stripe_mutexes_[stripe].mtx, std::try_to_lock);
        if (!lock.owns_lock()) {
            continue; // Don't block, try next candidate
        }

        Seat& seat = seats_[candidate_seat_id - 1];
        if (seat.state == SeatState::AVAILABLE) {
            uint64_t res_id = reservation_id_generator_.fetch_add(1, std::memory_order_relaxed);
            seat.state = SeatState::HELD;
            seat.user_id = user_id;
            seat.reservation_id = res_id;
            seat.hold_expiry = Clock::now() + std::chrono::milliseconds(ttl_ms);

            wal_->append_record(WalAction::HOLD, user_id, candidate_seat_id, res_id);
            reaper_->schedule_expiry(res_id, candidate_seat_id, ttl_ms);

            return {true, ReservationStatus::SUCCESS, res_id, candidate_seat_id};
        }
    }

    // If all seats were busy or locked (rare race)
    available_count_.fetch_add(1, std::memory_order_relaxed);
    return {false, ReservationStatus::SOLD_OUT, 0, 0};
}

bool BookingEngine::confirm_booking(uint32_t user_id, uint64_t reservation_id, uint32_t seat_id) {
    if (seat_id == 0 || seat_id > total_seats_) return false;

    size_t stripe = get_stripe_index(seat_id);
    std::lock_guard<std::mutex> lock(stripe_mutexes_[stripe].mtx);

    Seat& seat = seats_[seat_id - 1];
    if (seat.state == SeatState::HELD &&
        seat.reservation_id == reservation_id &&
        seat.user_id == user_id) {
        
        seat.state = SeatState::CONFIRMED;
        wal_->append_record(WalAction::CONFIRM, user_id, seat_id, reservation_id);
        total_confirmed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    return false;
}

bool BookingEngine::cancel_reservation(uint32_t user_id, uint64_t reservation_id, uint32_t seat_id) {
    if (seat_id == 0 || seat_id > total_seats_) return false;

    size_t stripe = get_stripe_index(seat_id);
    std::lock_guard<std::mutex> lock(stripe_mutexes_[stripe].mtx);

    Seat& seat = seats_[seat_id - 1];
    if (seat.state == SeatState::HELD &&
        seat.reservation_id == reservation_id &&
        seat.user_id == user_id) {
        
        seat.state = SeatState::AVAILABLE;
        seat.user_id = 0;
        seat.reservation_id = 0;
        available_count_.fetch_add(1, std::memory_order_relaxed);

        wal_->append_record(WalAction::RELEASE, user_id, seat_id, reservation_id);
        return true;
    }

    return false;
}

void BookingEngine::handle_expiry(uint64_t reservation_id, uint32_t seat_id) {
    if (seat_id == 0 || seat_id > total_seats_) return;

    size_t stripe = get_stripe_index(seat_id);
    std::lock_guard<std::mutex> lock(stripe_mutexes_[stripe].mtx);

    Seat& seat = seats_[seat_id - 1];
    // Only expire if it is still in HELD state with the matching reservation ID
    if (seat.state == SeatState::HELD && seat.reservation_id == reservation_id) {
        seat.state = SeatState::AVAILABLE;
        seat.user_id = 0;
        seat.reservation_id = 0;
        available_count_.fetch_add(1, std::memory_order_relaxed);
        total_expired_.fetch_add(1, std::memory_order_relaxed);

        wal_->append_record(WalAction::EXPIRE, 0, seat_id, reservation_id);
    }
}

SeatState BookingEngine::get_seat_state(uint32_t seat_id) {
    if (seat_id == 0 || seat_id > total_seats_) return SeatState::AVAILABLE;
    size_t stripe = get_stripe_index(seat_id);
    std::lock_guard<std::mutex> lock(stripe_mutexes_[stripe].mtx);
    return seats_[seat_id - 1].state;
}

EngineStats BookingEngine::get_stats() {
    EngineStats stats;
    stats.total_seats = total_seats_;
    stats.available_seats = available_count_.load(std::memory_order_relaxed);
    stats.total_reservations_attempted = total_reservations_attempted_.load(std::memory_order_relaxed);
    stats.total_confirmed = total_confirmed_.load(std::memory_order_relaxed);
    stats.total_expired = total_expired_.load(std::memory_order_relaxed);
    stats.total_rate_limited = total_rate_limited_.load(std::memory_order_relaxed);

    uint32_t held = 0;
    uint32_t confirmed = 0;
    for (uint32_t i = 0; i < total_seats_; ++i) {
        size_t stripe = get_stripe_index(i + 1);
        std::lock_guard<std::mutex> lock(stripe_mutexes_[stripe].mtx);
        if (seats_[i].state == SeatState::HELD) held++;
        else if (seats_[i].state == SeatState::CONFIRMED) confirmed++;
    }
    stats.held_seats = held;
    stats.confirmed_seats = confirmed;

    return stats;
}

void BookingEngine::recover_from_wal() {
    std::cout << "[Recovery] Replaying Write-Ahead Log to restore state...\n";
    uint64_t records_replayed = 0;

    wal_->replay([this, &records_replayed](const WalRecord& rec) {
        records_replayed++;
        if (rec.seat_id == 0 || rec.seat_id > total_seats_) return;

        Seat& seat = seats_[rec.seat_id - 1];
        WalAction action = static_cast<WalAction>(rec.action);

        switch (action) {
            case WalAction::HOLD:
                seat.state = SeatState::HELD;
                seat.user_id = rec.user_id;
                seat.reservation_id = rec.reservation_id;
                break;
            case WalAction::CONFIRM:
                seat.state = SeatState::CONFIRMED;
                break;
            case WalAction::RELEASE:
            case WalAction::EXPIRE:
                seat.state = SeatState::AVAILABLE;
                seat.user_id = 0;
                seat.reservation_id = 0;
                break;
        }
    });

    // Recompute available count
    int32_t available = 0;
    for (const auto& seat : seats_) {
        if (seat.state == SeatState::AVAILABLE) {
            available++;
        }
    }
    available_count_.store(available, std::memory_order_relaxed);
    std::cout << "[Recovery Complete] " << records_replayed << " records replayed. Available seats: " 
              << available << "/" << total_seats_ << "\n";
}

void BookingEngine::reset_all() {
    for (uint32_t i = 0; i < total_seats_; ++i) {
        size_t stripe = get_stripe_index(i + 1);
        std::lock_guard<std::mutex> lock(stripe_mutexes_[stripe].mtx);
        seats_[i].state = SeatState::AVAILABLE;
        seats_[i].user_id = 0;
        seats_[i].reservation_id = 0;
    }
    available_count_.store(total_seats_, std::memory_order_relaxed);
    total_reservations_attempted_.store(0, std::memory_order_relaxed);
    total_confirmed_.store(0, std::memory_order_relaxed);
    total_expired_.store(0, std::memory_order_relaxed);
    total_rate_limited_.store(0, std::memory_order_relaxed);
    if (wal_) {
        wal_->reset();
    }
    if (rate_limiter_) {
        rate_limiter_->reset();
    }
}

} // namespace ticket_engine
