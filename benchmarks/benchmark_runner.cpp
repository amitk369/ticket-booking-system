#include "booking_engine.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <iomanip>

using namespace ticket_engine;

int main() {
    std::cout << "\n=======================================================\n";
    std::cout << "   BENCHMARK: 100,000 CONCURRENT REQUESTS (QPS & P99)  \n";
    std::cout << "=======================================================\n";

    const uint32_t TOTAL_SEATS = 10000;
    const uint32_t NUM_THREADS = 16;
    const uint32_t REQS_PER_THREAD = 6250; // 100,000 total requests
    const uint32_t TOTAL_REQS = NUM_THREADS * REQS_PER_THREAD;

    BookingEngine engine(TOTAL_SEATS, "data/benchmark.wal");
    engine.reset_all();

    std::vector<std::vector<uint64_t>> thread_latencies(NUM_THREADS);
    for (auto& lat : thread_latencies) {
        lat.reserve(REQS_PER_THREAD);
    }

    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);

    auto global_start = std::chrono::high_resolution_clock::now();

    for (uint32_t t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&engine, t, &thread_latencies]() {
            auto& lat_vec = thread_latencies[t];

            for (uint32_t i = 0; i < REQS_PER_THREAD; ++i) {
                uint32_t user_id = (t * 100000) + i + 1;
                uint32_t seat_id = ((i * 37 + t) % TOTAL_SEATS) + 1;

                auto req_start = std::chrono::high_resolution_clock::now();
                auto res = engine.reserve_seat(user_id, seat_id, 5000);
                if (res.success) {
                    engine.confirm_booking(user_id, res.reservation_id, res.seat_id);
                }
                auto req_end = std::chrono::high_resolution_clock::now();

                uint64_t latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(req_end - req_start).count();
                lat_vec.push_back(latency_ns);
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    auto global_end = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(global_end - global_start).count();

    // Aggregate latencies
    std::vector<uint64_t> all_latencies;
    all_latencies.reserve(TOTAL_REQS);
    for (const auto& lats : thread_latencies) {
        all_latencies.insert(all_latencies.end(), lats.begin(), lats.end());
    }

    std::sort(all_latencies.begin(), all_latencies.end());

    double qps = static_cast<double>(TOTAL_REQS) / total_sec;
    uint64_t p50_ns = all_latencies[static_cast<size_t>(TOTAL_REQS * 0.50)];
    uint64_t p95_ns = all_latencies[static_cast<size_t>(TOTAL_REQS * 0.95)];
    uint64_t p99_ns = all_latencies[static_cast<size_t>(TOTAL_REQS * 0.99)];
    uint64_t p999_ns = all_latencies[static_cast<size_t>(TOTAL_REQS * 0.999)];

    double avg_ns = std::accumulate(all_latencies.begin(), all_latencies.end(), 0.0) / TOTAL_REQS;

    auto stats = engine.get_stats();

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n----------------- BENCHMARK RESULTS -----------------\n";
    std::cout << " Total Requests Processed : " << TOTAL_REQS << "\n";
    std::cout << " Total Elapsed Time       : " << total_sec << " s\n";
    std::cout << " Throughput (QPS)         : " << qps << " req/sec\n";
    std::cout << " Average Latency          : " << (avg_ns / 1000.0) << " µs\n";
    std::cout << " p50 (Median) Latency     : " << (p50_ns / 1000.0) << " µs\n";
    std::cout << " p95 Latency              : " << (p95_ns / 1000.0) << " µs\n";
    std::cout << " p99 Latency              : " << (p99_ns / 1000.0) << " µs\n";
    std::cout << " p99.9 Latency            : " << (p999_ns / 1000.0) << " µs\n";
    std::cout << " Confirmed Bookings       : " << stats.confirmed_seats << " / " << stats.total_seats << "\n";
    std::cout << "-----------------------------------------------------\n\n";

    return 0;
}
