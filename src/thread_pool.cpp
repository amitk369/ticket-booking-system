#include "thread_pool.hpp"

namespace ticket_engine {

ThreadPool::ThreadPool(size_t num_threads) {
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;
    }

    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this]() {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex_);
                    this->condition_.wait(lock, [this]() {
                        return this->stop_ || !this->tasks_.empty();
                    });

                    if (this->stop_ && this->tasks_.empty()) {
                        return;
                    }

                    task = std::move(this->tasks_.front());
                    this->tasks_.pop();
                }
                task();
            }
        });
    }
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_) return;
        stop_ = true;
    }
    condition_.notify_all();
    for (std::thread &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

} // namespace ticket_engine
