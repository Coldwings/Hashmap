#include <gtest/gtest.h>
#include <concurrent_hashmap/detail/epoch.h>
#include <thread>
#include <atomic>
#include <vector>

using namespace concurrent_hashmap::detail;

TEST(Epoch, GuardNesting) {
    EpochManager mgr;
    {
        EpochGuard g1(mgr);
        {
            EpochGuard g2(mgr);
            // Nested guard should work without deadlock
        }
    }
}

TEST(Epoch, RetireDefersDelete) {
    EpochManager mgr;
    std::atomic<int> deleted{0};
    struct Disposable : EpochManager::Retired {
        std::atomic<int>& counter;
        Disposable(std::atomic<int>& c) : counter(c) {}
        ~Disposable() { counter.fetch_add(1); }
    };
    {
        EpochGuard g(mgr);
        mgr.retire(new Disposable(deleted));
        // Not yet deleted — guard is still held
    }
    // After guard released, try_advance may have run.
    // Force multiple epochs to advance by entering/exiting guards.
    // The epoch manager amortises try_advance calls, so we need enough
    // iterations to guarantee at least 2 epoch advancements.
    for (int i = 0; i < 256; ++i) {
        EpochGuard g(mgr);
    }
    EXPECT_EQ(deleted.load(), 1);
}

TEST(Epoch, MultiThreadRetire) {
    EpochManager mgr;
    std::atomic<int> deleted{0};
    struct Disposable : EpochManager::Retired {
        std::atomic<int>& counter;
        Disposable(std::atomic<int>& c) : counter(c) {}
        ~Disposable() { counter.fetch_add(1); }
    };
    constexpr int N = 100;
    constexpr int T = 4;
    std::vector<std::thread> threads;
    for (int t = 0; t < T; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < N; ++i) {
                EpochGuard g(mgr);
                mgr.retire(new Disposable(deleted));
            }
        });
    }
    for (auto& th : threads) th.join();
    // Force epoch advancement.  The epoch manager amortises try_advance
    // calls, so we need enough iterations to guarantee full drain.
    for (int i = 0; i < 256; ++i) {
        EpochGuard g(mgr);
    }
    EXPECT_EQ(deleted.load(), N * T);
}
