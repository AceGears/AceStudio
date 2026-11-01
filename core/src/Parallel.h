#pragma once
// Runs fn(i) for i in [0, n) on up to `maxThreads` threads (the calling thread
// included). Items are taken in order from a shared counter; callers write
// results into per-index slots, so the outcome does not depend on scheduling.
// The first exception thrown by any item is rethrown after all threads finish.
// If a thread cannot be started, the remaining work runs on fewer threads.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace acestudio::detail {

inline unsigned workerCount(std::size_t items, unsigned maxThreads = 16) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return static_cast<unsigned>(std::min<std::size_t>({items, hw, maxThreads}));
}

template <typename Fn> void parallelFor(std::size_t n, Fn&& fn, unsigned maxThreads = 16) {
    if (n == 0) return;
    const unsigned threads = workerCount(n, maxThreads);
    if (threads <= 1) {
        for (std::size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::atomic<bool> stop{false};
    std::exception_ptr error;
    std::mutex errorMutex;
    auto work = [&] {
        for (;;) {
            if (stop.load(std::memory_order_relaxed)) return;
            const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) return;
            try {
                fn(i);
            } catch (...) {
                std::lock_guard lock(errorMutex);
                if (!error) error = std::current_exception();
                stop = true;
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    try {
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    } catch (const std::system_error&) {
        // Fewer threads than hoped; the ones running (and this one) finish the work.
    }
    work();
    for (auto& t : pool) t.join();
    if (error) std::rethrow_exception(error);
}

} // namespace acestudio::detail
