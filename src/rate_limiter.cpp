#include "rate_limiter.hpp"
#include <algorithm>

namespace ticket_engine {

RateLimiter::RateLimiter(double capacity, double refill_rate_per_sec, size_t num_stripes)
    : capacity_(capacity),
      refill_rate_(refill_rate_per_sec),
      num_stripes_(num_stripes > 0 ? num_stripes : 1),
      stripes_(num_stripes > 0 ? num_stripes : 1) {
}

bool RateLimiter::allow_request(uint32_t user_id, double tokens) {
    if (tokens <= 0.0) return true;

    auto now = Clock::now();
    size_t stripe_idx = get_stripe(user_id);
    auto& stripe = stripes_[stripe_idx];

    std::lock_guard<std::mutex> lock(stripe.mtx);
    auto it = stripe.user_buckets.find(user_id);

    if (it == stripe.user_buckets.end()) {
        // First request from this user: give them full capacity minus requested tokens
        if (capacity_ >= tokens) {
            stripe.user_buckets[user_id] = Bucket{capacity_ - tokens, now};
            return true;
        }
        stripe.user_buckets[user_id] = Bucket{capacity_, now};
        return false;
    }

    Bucket& bucket = it->second;

    // Refill tokens based on time elapsed since last check
    std::chrono::duration<double> elapsed = now - bucket.last_refill;
    bucket.tokens = std::min(capacity_, bucket.tokens + elapsed.count() * refill_rate_);
    bucket.last_refill = now;

    // Check if user has sufficient tokens
    if (bucket.tokens >= tokens) {
        bucket.tokens -= tokens;
        return true;
    }

    // Rate limit triggered (HTTP 429 Too Many Requests)
    return false;
}

void RateLimiter::reset() {
    for (auto& stripe : stripes_) {
        std::lock_guard<std::mutex> lock(stripe.mtx);
        stripe.user_buckets.clear();
    }
}

} // namespace ticket_engine
