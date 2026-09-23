#include "booking_engine.hpp"
#include "rate_limiter.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <cassert>

using namespace ticket_engine;

int main() {
    std::cout << "\n=======================================================\n";
    std::cout << "   RUNNING TOKEN BUCKET RATE LIMITER TEST (Anti-Bot)   \n";
    std::cout << "=======================================================\n";

    // 1. Standalone Token Bucket Test
    {
        std::cout << "[Step 1] Testing TokenBucket mechanics (Capacity: 5, Refill: 2/sec)...\n";
        RateLimiter limiter(5.0, 2.0, 16);

        uint32_t bot_id = 999;

        // First 5 requests within burst limit should be allowed
        for (int i = 1; i <= 5; ++i) {
            bool allowed = limiter.allow_request(bot_id);
            assert(allowed && "Initial burst tokens should be granted");
        }
        std::cout << "  -> First 5 burst requests allowed.\n";

        // 6th immediate request should be rejected (HTTP 429)
        bool denied = !limiter.allow_request(bot_id);
        assert(denied && "6th immediate request must be rate limited!");
        std::cout << "  -> 6th immediate request blocked (Rate limited successfully).\n";

        // Wait 550ms: should refill approx 1 token (2 tokens/sec * 0.55s = 1.1 tokens)
        std::cout << "  -> Sleeping 600ms to allow token refill...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        bool allowed_after_refill = limiter.allow_request(bot_id);
        assert(allowed_after_refill && "Request after token refill should be allowed!");
        std::cout << "  -> Request after refill succeeded.\n";
    }

    // 2. Integrated BookingEngine Anti-Bot Test
    {
        std::cout << "\n[Step 2] Testing Bot Mitigation in BookingEngine...\n";
        BookingEngine engine(1000, "data/test_rate_limiter.wal");
        engine.reset_all();

        uint32_t bot_user_id = 888;
        uint32_t honest_user_id = 111;

        int bot_accepted = 0;
        int bot_blocked = 0;

        // Bot fires 20 rapid booking attempts
        for (uint32_t seat = 1; seat <= 20; ++seat) {
            auto res = engine.reserve_seat(bot_user_id, seat, 5000);
            if (res.success) {
                bot_accepted++;
            } else if (res.status == ReservationStatus::RATE_LIMITED) {
                bot_blocked++;
            }
        }

        std::cout << "  -> Bot fired 20 rapid requests: " << bot_accepted << " accepted, "
                  << bot_blocked << " blocked by Rate Limiter.\n";
        assert(bot_accepted == 5 && "Bot should only get 5 burst reservations");
        assert(bot_blocked == 15 && "Remaining 15 bot requests must be rate limited");

        // Honest user should not be affected by bot's rate limit
        auto honest_res = engine.reserve_seat(honest_user_id, 50, 5000);
        assert(honest_res.success && "Honest user's request must succeed independently!");
        std::cout << "  -> Honest User #111 booked Seat #50 with ZERO interference from the bot!\n";

        auto stats = engine.get_stats();
        assert(stats.total_rate_limited == 15 && "Telemetry counter for rate limited requests must match");
    }

    std::cout << "\n>>> ALL RATE LIMITER & BOT DEFENSE TESTS PASSED! <<<\n\n";
    return 0;
}
