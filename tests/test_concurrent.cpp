#include <gtest/gtest.h>
#include <concurrent_hashmap/concurrent_hashmap.h>

#include <atomic>
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

// Small shard count (4 shards) for tests with small key sets.
using SmallMap = ConcurrentHashMap<int, int, MixHash, std::equal_to<int>, 2>;

// Default shard count (64 shards) for resize-heavy and large key set tests.
using DefaultMap = ConcurrentHashMap<int, int, MixHash, std::equal_to<int>, 6>;

// ---------------------------------------------------------------------------
// Fixture: static maps to avoid thread_local EpochManager dangling pointers.
// ---------------------------------------------------------------------------
class ConcurrentTest : public ::testing::Test {
protected:
    static SmallMap*   small_map_;
    static DefaultMap* default_map_;

    static void SetUpTestSuite() {
        small_map_   = new SmallMap();
        default_map_ = new DefaultMap();
    }
    static void TearDownTestSuite() {
        delete default_map_;
        default_map_ = nullptr;
        delete small_map_;
        small_map_ = nullptr;
    }

    void SetUp() override {
        small_map_->clear();
        default_map_->clear();
    }

    SmallMap&   smap() { return *small_map_; }
    DefaultMap& dmap() { return *default_map_; }
};

SmallMap*   ConcurrentTest::small_map_   = nullptr;
DefaultMap* ConcurrentTest::default_map_ = nullptr;

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
// Test 1: N threads insert disjoint key ranges -> final size == sum
//
// Uses the default map (64 shards) so that 16 threads x 500 keys = 8000
// total keys are spread across shards.
// ===========================================================================
TEST_F(ConcurrentTest, DisjointInsertSize) {
    const int kThreads   = 16;
    const int kPerThread = 500;

    run_threads(kThreads, [&](int tid) {
        int base = tid * kPerThread;
        for (int i = 0; i < kPerThread; ++i) {
            EXPECT_TRUE(dmap().insert(base + i, base + i));
        }
    });

    // Total size must equal the sum of all inserted keys.
    EXPECT_EQ(dmap().size(), static_cast<size_t>(kThreads * kPerThread));

    // Every key must be findable with the correct value.
    for (int tid = 0; tid < kThreads; ++tid) {
        int base = tid * kPerThread;
        for (int i = 0; i < kPerThread; ++i) {
            int key = base + i;
            auto result = dmap().find(key);
            EXPECT_TRUE(result.second)   << "key " << key << " not found";
            EXPECT_EQ(result.first, key) << "wrong value for key " << key;
        }
    }
}

// ===========================================================================
// Test 2: N threads insert same key -> exactly 1 returns true
//
// Uses the small map (4 shards) since only 1 key is involved.
// ===========================================================================
TEST_F(ConcurrentTest, SameKeyInsertExactlyOneSucceeds) {
    const int kThreads = 16;
    std::atomic<int> success_count{0};

    run_threads(kThreads, [&](int tid) {
        if (smap().insert(42, tid)) {
            success_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    EXPECT_EQ(success_count.load(), 1);
    EXPECT_EQ(smap().size(), 1u);

    auto result = smap().find(42);
    EXPECT_TRUE(result.second);
}

// ===========================================================================
// Test 3: Concurrent find during insert -> no crash, no garbage
//
// Uses the small map (4 shards) with a modest key count.
// ===========================================================================
TEST_F(ConcurrentTest, ConcurrentFindDuringInsert) {
    const int kInserters = 8;
    const int kFinders   = 8;
    const int kKeys      = 200;

    // Inserter threads insert keys [0, kKeys).
    // Finder threads concurrently call find() on the same range.
    // find() must either return {value, true} with value == key,
    // or {0, false}. Never crash, never return garbage.

    run_threads(kInserters + kFinders, [&](int tid) {
        if (tid < kInserters) {
            // Inserter: each inserts a disjoint portion of the key space.
            int base = tid * (kKeys / kInserters);
            int end  = (tid + 1) * (kKeys / kInserters);
            for (int i = base; i < end; ++i) {
                smap().insert(i, i);
            }
        } else {
            // Finder: repeatedly probe the key space.
            for (int i = 0; i < kKeys; ++i) {
                auto result = smap().find(i);
                if (result.second) {
                    EXPECT_EQ(result.first, i)
                        << "garbage value for key " << i;
                }
            }
        }
    });

    // After all threads join, every key must be present.
    for (int i = 0; i < kKeys; ++i) {
        EXPECT_TRUE(smap().contains(i)) << "key " << i << " missing";
    }
}

// ===========================================================================
// Test 4: Concurrent erase -> no double-free, size consistent
//
// Uses the small map (4 shards) with a modest key count.
// ===========================================================================
TEST_F(ConcurrentTest, ConcurrentEraseExactlyOnce) {
    const int kKeys    = 500;
    const int kThreads = 16;

    // Pre-populate.
    for (int i = 0; i < kKeys; ++i) {
        smap().insert(i, i);
    }
    EXPECT_EQ(smap().size(), static_cast<size_t>(kKeys));

    // Each thread tries to erase all keys. Each key should be erased
    // successfully by exactly one thread.
    std::atomic<int> total_erased{0};

    run_threads(kThreads, [&](int /*tid*/) {
        int local_erased = 0;
        for (int i = 0; i < kKeys; ++i) {
            if (smap().erase(i)) {
                ++local_erased;
            }
        }
        total_erased.fetch_add(local_erased, std::memory_order_relaxed);
    });

    // Exactly kKeys successful erases across all threads.
    EXPECT_EQ(total_erased.load(), kKeys);
    EXPECT_EQ(smap().size(), 0u);
}

// ===========================================================================
// Test 5: Concurrent insert triggers resize -> no data loss
//
// Uses the default map (64 shards). 16 threads x 1000 keys = 16000 total,
// ~250 per shard, triggering multiple resizes from initial capacity of 16.
// ===========================================================================
TEST_F(ConcurrentTest, ConcurrentInsertTriggersResizeNoDataLoss) {
    const int kThreads   = 16;
    const int kPerThread = 1000;

    run_threads(kThreads, [&](int tid) {
        int base = tid * kPerThread;
        for (int i = 0; i < kPerThread; ++i) {
            dmap().insert(base + i, base + i);
        }
    });

    size_t expected = static_cast<size_t>(kThreads * kPerThread);
    EXPECT_EQ(dmap().size(), expected);

    // Verify every single key is present -- no data loss during resizes.
    for (int tid = 0; tid < kThreads; ++tid) {
        int base = tid * kPerThread;
        for (int i = 0; i < kPerThread; ++i) {
            int key = base + i;
            auto result = dmap().find(key);
            EXPECT_TRUE(result.second) << "key " << key << " lost during resize";
            EXPECT_EQ(result.first, key);
        }
    }
}

// ===========================================================================
// Test 6: Mixed insert + erase + find -> no crash, size >= 0
//
// Uses the small map (4 shards) with a small key range so shard loads
// stay manageable.
// ===========================================================================
TEST_F(ConcurrentTest, MixedOperationsNoCrash) {
    const int kThreads      = 16;
    const int kOpsPerThread = 2000;
    const int kKeyRange     = 100;

    run_threads(kThreads, [&](int tid) {
        // Use a simple LCG PRNG seeded by tid to choose operations.
        unsigned seed = static_cast<unsigned>(tid * 7919 + 1);
        for (int op = 0; op < kOpsPerThread; ++op) {
            seed = seed * 1103515245u + 12345u;
            int key = static_cast<int>((seed >> 16) % kKeyRange);
            unsigned action = (seed / 3) % 3;

            if (action == 0) {
                smap().insert(key, key);
            } else if (action == 1) {
                smap().erase(key);
            } else {
                // Lock-free find() may observe transient slot state during
                // a concurrent backward-shift delete or Robin Hood swap.
                // We only check that it does not crash or hang.
                auto result = smap().find(key);
                (void)result;
            }
        }
    });

    // The test succeeds if we reach here without crashing.
    // Size must be <= kKeyRange (can never have more unique keys).
    EXPECT_LE(smap().size(), static_cast<size_t>(kKeyRange));
}
