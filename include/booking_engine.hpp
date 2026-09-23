#pragma once

#include "wal.hpp"
#include "reaper.hpp"
#include "rate_limiter.hpp"
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <string>

namespace ticket_engine {

enum class SeatState : uint8_t {
    AVAILABLE = 0,
    HELD = 1,
    CONFIRMED = 2
};

enum class ReservationStatus {
    SUCCESS,
    SEAT_TAKEN,
    SOLD_OUT,
    RATE_LIMITED,
    INVALID_SEAT,
    INVALID_RESERVATION
};

struct Seat {
    uint32_t seat_id{0};
    SeatState state{SeatState::AVAILABLE};
    uint32_t user_id{0};
    uint64_t reservation_id{0};
    TimePoint hold_expiry{};
};

struct ReservationResult {
    bool success{false};
    ReservationStatus status{ReservationStatus::SOLD_OUT};
    uint64_t reservation_id{0};
    uint32_t seat_id{0};
};

struct EngineStats {
    uint32_t total_seats{0};
    int32_t available_seats{0};
    uint32_t held_seats{0};
    uint32_t confirmed_seats{0};
    uint64_t total_reservations_attempted{0};
    uint64_t total_confirmed{0};
    uint64_t total_expired{0};
    uint64_t total_rate_limited{0};
};

class BookingEngine {
public:
    static constexpr size_t NUM_STRIPES = 64; // Lock striping granularity

    BookingEngine(uint32_t total_seats, const std::string& wal_path = "data/ticket_engine.wal");
    ~BookingEngine();

    // 2-Phase Reservation APIs (with optional rate-limiting check)
    ReservationResult reserve_seat(uint32_t user_id, uint32_t seat_id, uint32_t ttl_ms = 5000, bool apply_rate_limit = true);
    ReservationResult reserve_any_seat(uint32_t user_id, uint32_t ttl_ms = 5000, bool apply_rate_limit = true);
    bool confirm_booking(uint32_t user_id, uint64_t reservation_id, uint32_t seat_id);
    bool cancel_reservation(uint32_t user_id, uint64_t reservation_id, uint32_t seat_id);

    // Expiry callback triggered by Reaper
    void handle_expiry(uint64_t reservation_id, uint32_t seat_id);

    // Crash recovery
    void recover_from_wal();

    // Inspection & Rate Limiter access
    SeatState get_seat_state(uint32_t seat_id);
    EngineStats get_stats();
    RateLimiter& rate_limiter() { return *rate_limiter_; }
    void reset_all();

private:
    uint32_t total_seats_;
    std::atomic<int32_t> available_count_;
    std::atomic<uint64_t> reservation_id_generator_{1};

    // Seat data storage
    std::vector<Seat> seats_;

    // Lock striping: array of mutexes to avoid global lock contention
    struct alignas(64) PaddedMutex {
        std::mutex mtx;
    };
    std::vector<PaddedMutex> stripe_mutexes_;

    // Subsystems
    std::unique_ptr<WriteAheadLog> wal_;
    std::unique_ptr<Reaper> reaper_;
    std::unique_ptr<RateLimiter> rate_limiter_;

    // Telemetry counters
    std::atomic<uint64_t> total_reservations_attempted_{0};
    std::atomic<uint64_t> total_confirmed_{0};
    std::atomic<uint64_t> total_expired_{0};
    std::atomic<uint64_t> total_rate_limited_{0};

    inline size_t get_stripe_index(uint32_t seat_id) const {
        return seat_id % NUM_STRIPES;
    }
};

} // namespace ticket_engine
