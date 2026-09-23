#include "reaper.hpp"

namespace ticket_engine {

Reaper::Reaper(ExpireCallback on_expire)
    : on_expire_(std::move(on_expire)) {
}

Reaper::~Reaper() {
    stop();
}

void Reaper::start() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (running_) return;
        running_ = true;
    }
    worker_ = std::thread(&Reaper::run, this);
}

void Reaper::stop() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Reaper::schedule_expiry(uint64_t reservation_id, uint32_t seat_id, uint32_t duration_ms) {
    auto expiry_point = Clock::now() + std::chrono::milliseconds(duration_ms);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        expiry_queue_.push({expiry_point, reservation_id, seat_id});
    }
    cv_.notify_one();
}

void Reaper::run() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    while (running_) {
        if (expiry_queue_.empty()) {
            cv_.wait(lock, [this]() { return !running_ || !expiry_queue_.empty(); });
        } else {
            auto next_expiry = expiry_queue_.top().expiry_time;
            auto status = cv_.wait_until(lock, next_expiry);
            (void)status;
        }

        if (!running_) break;

        auto now = Clock::now();
        std::vector<ExpiryEvent> expired_events;

        while (!expiry_queue_.empty() && expiry_queue_.top().expiry_time <= now) {
            expired_events.push_back(expiry_queue_.top());
            expiry_queue_.pop();
        }

        // Release lock before invoking callbacks to avoid deadlock with engine
        if (!expired_events.empty()) {
            lock.unlock();
            for (const auto& ev : expired_events) {
                if (on_expire_) {
                    on_expire_(ev.reservation_id, ev.seat_id);
                }
            }
            lock.lock();
        }
    }
}

} // namespace ticket_engine
