#pragma once

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace ticket_engine {

/**
 * Thread-safe Token Bucket Rate Limiter.
 * 
 * Uses lazy refills: tokens are calculated dynamically on each request using 
 * the time delta rather than keeping active background timers.
 * 
 * To prevent a global lock bottleneck across all users, the bucket map 
 * is partitioned into multiple stripes based on user_id.
 */
class RateLimiter {
public:
    RateLimiter(double capacity = 5.0, double refill_rate_per_sec = 2.0, size_t num_stripes = 32);
    ~RateLimiter() = default;

    // Checks if the user has enough tokens; consumes them if allowed.
    bool allow_request(uint32_t user_id, double tokens = 1.0);

    // Clear state (useful for test resets)
    void reset();

    double capacity() const { return capacity_; }
    double refill_rate() const { return refill_rate_; }

private:
    using Clock = std::chrono::steady_clock;

    struct Bucket {
        double tokens;
        Clock::time_point last_refill;
    };

    struct Stripe {
        std::mutex mtx;
        std::unordered_map<uint32_t, Bucket> user_buckets;
    };

    double capacity_;
    double refill_rate_;
    size_t num_stripes_;
    std::vector<Stripe> stripes_;

    inline size_t get_stripe(uint32_t user_id) const {
        return user_id % num_stripes_;
    }
};

} // namespace ticket_engine
