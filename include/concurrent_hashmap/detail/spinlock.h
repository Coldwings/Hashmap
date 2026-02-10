#pragma once
#include <atomic>

namespace concurrent_hashmap {
namespace detail {

class SpinLock {
public:
    SpinLock() noexcept : flag_(false) {}

    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() noexcept {
        for (;;) {
            // Optimistic test first (read-only, no cache line bouncing)
            if (!flag_.exchange(true, std::memory_order_acquire)) {
                return;
            }
            // Spin on read until released
            while (flag_.load(std::memory_order_relaxed)) {
                #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
                #elif defined(__aarch64__)
                __asm__ volatile("yield" ::: "memory");
                #endif
            }
        }
    }

    void unlock() noexcept {
        flag_.store(false, std::memory_order_release);
    }

    bool try_lock() noexcept {
        return !flag_.exchange(true, std::memory_order_acquire);
    }

private:
    std::atomic<bool> flag_;
};

} // namespace detail
} // namespace concurrent_hashmap
