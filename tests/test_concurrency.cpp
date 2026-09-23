#include "booking_engine.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#include <random>
#include <atomic>
#include <unordered_set>

using namespace ticket_engine;

int main() {
    std::cout << "\n=======================================================\n";
    std::cout << "  RUNNING HIGH-CONCURRENCY STRESS TEST (Zero Overselling)  \n";
    std::cout << "=======================================================\n";

    const uint32_t TOTAL_SEATS = 5000;
    const uint32_t NUM_THREADS = 16;
    const uint32_t REQUESTS_PER_THREAD = 3000; // 48,000 total requests
    BookingEngine engine(TOTAL_SEATS, "data/test_concurrency.wal");
    engine.reset_all();

    std::atomic<uint32_t> successful_reservations{0};
    std::atomic<uint32_t> successful_confirmations{0};
    std::atomic<uint32_t> rejected_requests{0};

    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);

    auto start_time = std::chrono::high_resolution_clock::now();

    for (uint32_t t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&engine, t, &successful_reservations, &successful_confirmations, &rejected_requests]() {
            std::mt19937 rng(1337 + t);
            std::uniform_int_distribution<uint32_t> seat_dist(1, TOTAL_SEATS);

            for (uint32_t r = 0; r < REQUESTS_PER_THREAD; ++r) {
                uint32_t user_id = (t * 100000) + r + 1;
                uint32_t chosen_seat = seat_dist(rng);

                auto res = engine.reserve_seat(user_id, chosen_seat, 10000); // 10s TTL
                if (res.success) {
                    successful_reservations.fetch_add(1, std::memory_order_relaxed);
                    // Confirm immediately
                    bool conf = engine.confirm_booking(user_id, res.reservation_id, res.seat_id);
                    if (conf) {
                        successful_confirmations.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    rejected_requests.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    auto stats = engine.get_stats();

    std::cout << "Total Requests Fired     : " << (NUM_THREADS * REQUESTS_PER_THREAD) << "\n";
    std::cout << "Successful Reservations  : " << successful_reservations.load() << "\n";
    std::cout << "Successful Confirmations : " << successful_confirmations.load() << "\n";
    std::cout << "Rejected / Sold Out      : " << rejected_requests.load() << "\n";
    std::cout << "Engine Confirmed Seats   : " << stats.confirmed_seats << "\n";
    std::cout << "Engine Available Seats   : " << stats.available_seats << "\n";
    std::cout << "Elapsed Time             : " << elapsed_ms << " ms\n";

    // INVARIANT ASSERTIONS
    assert(stats.confirmed_seats <= TOTAL_SEATS && "CRITICAL FAILURE: Overselling detected!");
    assert(stats.confirmed_seats == successful_confirmations.load() && "Confirmation count mismatch!");
    assert(stats.available_seats + stats.confirmed_seats + stats.held_seats == TOTAL_SEATS && 
           "Inventory conservation violated!");

    // Verify seat uniqueness
    std::unordered_set<uint32_t> confirmed_set;
    for (uint32_t s = 1; s <= TOTAL_SEATS; ++s) {
        if (engine.get_seat_state(s) == SeatState::CONFIRMED) {
            assert(confirmed_set.find(s) == confirmed_set.end() && "Duplicate seat allocation detected!");
            confirmed_set.insert(s);
        }
    }

    std::cout << "\n>>> ALL CONCURRENCY INVARIANTS PASSED! ZERO OVERSELLING DETECTED! <<<\n\n";
    return 0;
}
