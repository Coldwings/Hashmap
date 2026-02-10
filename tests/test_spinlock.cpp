#include <gtest/gtest.h>
#include <concurrent_hashmap/detail/spinlock.h>
#include <thread>
#include <vector>

using namespace concurrent_hashmap::detail;

TEST(SpinLock, LockUnlock) {
    SpinLock lock;
    lock.lock();
    lock.unlock();
}

TEST(SpinLock, MutualExclusion) {
    SpinLock lock;
    int counter = 0;
    constexpr int N = 10000;
    constexpr int T = 8;
    std::vector<std::thread> threads;
    for (int t = 0; t < T; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < N; ++i) {
                lock.lock();
                ++counter;
                lock.unlock();
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(counter, N * T);
}

TEST(SpinLock, LockGuardCompatible) {
    SpinLock lock;
    {
        std::lock_guard<SpinLock> guard(lock);
    }
}
