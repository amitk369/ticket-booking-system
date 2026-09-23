#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <memory>
#include <stdexcept>

namespace ticket_engine {

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads);
    ~ThreadPool();

    // Submit any callable with arguments, returning a std::future
    template<class F, class... Args>
    auto submit(F&& f, Args&&... args) 
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        
        using return_type = typename std::invoke_result<F, Args...>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_) {
                throw std::runtime_error("Cannot submit task to stopped ThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return res;
    }

    size_t worker_count() const { return workers_.size(); }
    void shutdown();

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_{false};
};

} // namespace ticket_engine
