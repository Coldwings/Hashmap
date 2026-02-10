#include <gtest/gtest.h>
#include <concurrent_hashmap/detail/shard.h>
#include <concurrent_hashmap/detail/epoch.h>
#include <string>

using namespace concurrent_hashmap::detail;
using TestShard = Shard<int, std::string, std::hash<int>, std::equal_to<int>, SpinLock>;

static size_t h(int key) {
    return std::hash<int>{}(key);
}

// Use a single EpochManager for the entire test suite to avoid
// use-after-free in thread_local ThreadHandle across test instances.
class ResizeTest : public ::testing::Test {
protected:
    static EpochManager* epoch_;

    static void SetUpTestSuite() {
        epoch_ = new EpochManager();
    }
    static void TearDownTestSuite() {
        delete epoch_;
        epoch_ = nullptr;
    }

    EpochManager& epoch() { return *epoch_; }
};

EpochManager* ResizeTest::epoch_ = nullptr;

TEST_F(ResizeTest, InsertTriggersExpansion) {
    // Start with capacity=16.  Load factor threshold is 0.75 => 12 elements.
    // Inserting 13+ should trigger a resize (expansion).
    TestShard shard(16);
    EpochGuard g(epoch());

    const int N = 100;
    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(shard.insert(h(i), i, std::to_string(i), epoch()));
    }
    EXPECT_EQ(shard.size(), static_cast<size_t>(N));

    // Verify all elements are still findable after resize(s).
    for (int i = 0; i < N; ++i) {
        auto result = shard.find(h(i), i);
        EXPECT_TRUE(result.second) << "key " << i << " not found after expansion";
        EXPECT_EQ(result.first, std::to_string(i));
    }
}

TEST_F(ResizeTest, EraseTriggersDelayedShrink) {
    // Insert enough to cause multiple expansions, then erase most elements.
    // The delayed shrink counter should eventually trigger a shrink.
    TestShard shard(16);
    EpochGuard g(epoch());

    // Insert 200 elements -- causes several expansions.
    const int N = 200;
    for (int i = 0; i < N; ++i) {
        shard.insert(h(i), i, std::to_string(i), epoch());
    }
    EXPECT_EQ(shard.size(), static_cast<size_t>(N));

    // Erase all but 2 elements.  After enough erases the shrink counter
    // should exceed capacity and trigger a shrink.
    for (int i = 2; i < N; ++i) {
        EXPECT_TRUE(shard.erase(h(i), i, epoch()));
    }
    EXPECT_EQ(shard.size(), 2u);

    // The surviving elements must still be findable.
    for (int i = 0; i < 2; ++i) {
        auto result = shard.find(h(i), i);
        EXPECT_TRUE(result.second) << "key " << i << " not found after shrink";
        EXPECT_EQ(result.first, std::to_string(i));
    }

    // Erased elements must not be found.
    for (int i = 2; i < N; ++i) {
        EXPECT_FALSE(shard.contains(h(i), i))
            << "key " << i << " should have been erased";
    }
}

TEST_F(ResizeTest, ReservePreAllocates) {
    TestShard shard;
    EpochGuard g(epoch());

    // Reserve space for 1000 elements.
    shard.reserve(1000, epoch());

    // Insert 500 elements -- should not trigger any resize.
    for (int i = 0; i < 500; ++i) {
        EXPECT_TRUE(shard.insert(h(i), i, std::to_string(i), epoch()));
    }
    EXPECT_EQ(shard.size(), 500u);

    // Verify all findable.
    for (int i = 0; i < 500; ++i) {
        auto result = shard.find(h(i), i);
        EXPECT_TRUE(result.second) << "key " << i << " not found";
        EXPECT_EQ(result.first, std::to_string(i));
    }
}

TEST_F(ResizeTest, ExpandAndShrinkCycle) {
    // Exercise multiple expand/shrink cycles.
    TestShard shard(16);
    EpochGuard g(epoch());

    for (int cycle = 0; cycle < 3; ++cycle) {
        // Insert many.
        for (int i = 0; i < 100; ++i) {
            shard.insert(h(i + cycle * 1000), i + cycle * 1000,
                         std::to_string(i), epoch());
        }

        // Erase all.  After enough erases, shrink may occur.
        for (int i = 0; i < 100; ++i) {
            shard.erase(h(i + cycle * 1000), i + cycle * 1000, epoch());
        }
        EXPECT_EQ(shard.size(), 0u);
    }

    // Final state: empty shard, still functional.
    EXPECT_TRUE(shard.insert(h(42), 42, "final", epoch()));
    auto result = shard.find(h(42), 42);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "final");
}
