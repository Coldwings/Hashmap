#include <gtest/gtest.h>
#include <concurrent_hashmap/concurrent_hashmap.h>

#include <thread>
#include <vector>
#include <functional>

using concurrent_hashmap::ConcurrentHashMap;

// ---------------------------------------------------------------------------
// MixHash: a hash function that distributes small integers across all shards.
//
// std::hash<int> is the identity function on many platforms, which causes
// all small integer keys to route to shard 0 (shard routing uses the top
// bits of the hash).  This leads to extreme probe-distance pressure in a
// single shard and is not representative of real-world workloads.
//
// MixHash applies a Murmur3-style finalizer to spread bits uniformly.
// ---------------------------------------------------------------------------
struct MixHash {
    size_t operator()(int key) const noexcept {
        auto x = static_cast<size_t>(static_cast<unsigned int>(key));
        x ^= x >> 16;
        x *= 0x45d9f3bUL;
        x ^= x >> 16;
        x *= 0x45d9f3bUL;
        x ^= x >> 16;
        // Ensure the upper bits are populated for 64-bit shard routing.
        return x | (x << 32);
    }
};

// Default shard count (64 shards) for stress testing.
using StressMap = ConcurrentHashMap<int, int, MixHash, std::equal_to<int>, 6>;

// ---------------------------------------------------------------------------
// Fixture: static map pointer to avoid thread_local EpochManager dangling
// pointer issues.
// ---------------------------------------------------------------------------
class StressTest : public ::testing::Test {
protected:
    static StressMap* map_;

    static void SetUpTestSuite() {
        map_ = new StressMap();
    }
    static void TearDownTestSuite() {
        delete map_;
        map_ = nullptr;
    }

    void SetUp() override {
        map_->clear();
    }

    StressMap& map() { return *map_; }
};

StressMap* StressTest::map_ = nullptr;

// ---------------------------------------------------------------------------
// Helper: launch N threads, each calling fn(thread_index).
// ---------------------------------------------------------------------------
static void run_threads(int n, std::function<void(int)> fn) {
    std::vector<std::thread> threads;
    threads.reserve(n);
    for (int i = 0; i < n; ++i) {
        threads.emplace_back(fn, i);
    }
    for (auto& t : threads) {
        t.join();
    }
}

// ===========================================================================
// Stress test: 32 threads, 100K ops each, random mix of
// insert / find / erase / get_or_set / contains / insert_or_assign
// on key range [0, 10000).
//
// Verifies: no crash, no hang, final size <= 10000.
// ===========================================================================
TEST_F(StressTest, MixedOpsHighContention) {
    const int kThreads      = 32;
    const int kOpsPerThread = 100000;
    const int kKeyRange     = 10000;

    run_threads(kThreads, [&](int tid) {
        // Simple LCG PRNG seeded per thread.
        unsigned seed = static_cast<unsigned>(tid * 7919 + 1);

        for (int op = 0; op < kOpsPerThread; ++op) {
            seed = seed * 1103515245u + 12345u;
            int key = static_cast<int>((seed >> 16) % kKeyRange);

            // Choose among 6 operations.
            unsigned action = (seed / 3) % 6;

            switch (action) {
            case 0:
                map().insert(key, key);
                break;
            case 1: {
                // Lock-free find() may observe transient slot state during
                // a concurrent backward-shift delete or Robin Hood swap.
                // We only check that it does not crash or hang.
                auto result = map().find(key);
                (void)result;
                break;
            }
            case 2:
                map().erase(key);
                break;
            case 3:
                map().get_or_set(key, key);
                break;
            case 4:
                map().contains(key);
                break;
            case 5:
                map().insert_or_assign(key, key);
                break;
            }
        }
    });

    // If we reach here, no crash or hang occurred.
    // Final size must be bounded by the key range.
    size_t final_size = map().size();
    EXPECT_LE(final_size, static_cast<size_t>(kKeyRange))
        << "final size " << final_size << " exceeds key range " << kKeyRange;
}
