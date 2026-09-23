#pragma once

#include <chrono>
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <cstdint>

namespace ticket_engine {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct ExpiryEvent {
    TimePoint expiry_time;
    uint64_t reservation_id;
    uint32_t seat_id;

    bool operator>(const ExpiryEvent& other) const {
        return expiry_time > other.expiry_time; // Min-heap: earliest time on top
    }
};

class Reaper {
public:
    using ExpireCallback = std::function<void(uint64_t reservation_id, uint32_t seat_id)>;

    explicit Reaper(ExpireCallback on_expire);
    ~Reaper();

    // Schedule a reservation to expire after duration_ms
    void schedule_expiry(uint64_t reservation_id, uint32_t seat_id, uint32_t duration_ms);

    void start();
    void stop();

private:
    void run();

    ExpireCallback on_expire_;
    std::priority_queue<ExpiryEvent, std::vector<ExpiryEvent>, std::greater<ExpiryEvent>> expiry_queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool running_{false};
};

} // namespace ticket_engine
