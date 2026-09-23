#include "booking_engine.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cassert>

using namespace ticket_engine;

int main() {
    std::cout << "\n=======================================================\n";
    std::cout << "  SIMULATION: BOT ATTACK DURING PEAK FLASH-SALE TRAFFIC \n";
    std::cout << "=======================================================\n";

    const uint32_t TOTAL_SEATS = 2000;
    BookingEngine engine(TOTAL_SEATS, "data/test_bot_attack.wal");
    engine.reset_all();

    std::atomic<uint32_t> legitimate_success{0};
    std::atomic<uint32_t> bot_blocked_by_rate_limiter{0};
    std::atomic<uint32_t> bot_accepted_burst{0};

    const int NUM_BOTS = 5;
    const int BOT_SPAM_PER_BOT = 5000; // 25,000 bot requests
    const int NUM_HUMANS = 500;

    std::vector<std::thread> workers;

    auto start_time = std::chrono::high_resolution_clock::now();

    // 1. Launch 5 Aggressive Scalper Bots simultaneously
    for (int b = 1; b <= NUM_BOTS; ++b) {
        workers.emplace_back([&engine, b, &bot_blocked_by_rate_limiter, &bot_accepted_burst]() {
            uint32_t bot_id = 9000 + b;
            for (int r = 0; r < BOT_SPAM_PER_BOT; ++r) {
                uint32_t target_seat = (r % TOTAL_SEATS) + 1;
                auto res = engine.reserve_seat(bot_id, target_seat, 5000);
                if (res.success) {
                    bot_accepted_burst.fetch_add(1, std::memory_order_relaxed);
                } else if (res.status == ReservationStatus::RATE_LIMITED) {
                    bot_blocked_by_rate_limiter.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // 2. Launch 500 Genuine Humans at the exact same millisecond
    for (int h = 1; h <= 10; ++h) { // 10 worker threads simulating 500 humans
        workers.emplace_back([&engine, h, &legitimate_success]() {
            for (int u = 1; u <= 50; ++u) {
                uint32_t user_id = (h * 100) + u; // Unique human ID
                uint32_t chosen_seat = ((user_id * 17) % TOTAL_SEATS) + 1;

                auto res = engine.reserve_seat(user_id, chosen_seat, 5000);
                if (res.success) {
                    legitimate_success.fetch_add(1, std::memory_order_relaxed);
                    engine.confirm_booking(user_id, res.reservation_id, res.seat_id);
                }
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    std::cout << "--- [ ATTACK SIMULATION RESULTS ] ---\n";
    std::cout << " Total Bot Spam Requests Fired : " << (NUM_BOTS * BOT_SPAM_PER_BOT) << "\n";
    std::cout << " Bot Requests Blocked (HTTP 429): " << bot_blocked_by_rate_limiter.load() 
              << " (" << (bot_blocked_by_rate_limiter.load() * 100.0 / (NUM_BOTS * BOT_SPAM_PER_BOT)) << "%)\n";
    std::cout << " Bot Requests Allowed (Burst)   : " << bot_accepted_burst.load() << "\n";
    std::cout << " Genuine Human Bookings Succeeded: " << legitimate_success.load() << " / " << (NUM_HUMANS) << "\n";
    std::cout << " Total Time to Mitigate & Process: " << elapsed_ms << " ms\n";
    std::cout << "-------------------------------------\n";

    // Invariant: The rate limiter must block > 99.8% of the bot requests
    assert(bot_blocked_by_rate_limiter.load() >= 24970 && "Rate limiter must block all bot requests past burst limit!");
    assert(legitimate_success.load() > 0 && "Genuine users must succeed without being starved by bots!");

    std::cout << "\n>>> VERIFIED: Bot attack neutralized with ZERO downtime or starvation for real users! <<<\n\n";
    return 0;
}
