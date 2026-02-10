#include <gtest/gtest.h>
#include <concurrent_hashmap/concurrent_hashmap.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using concurrent_hashmap::ConcurrentHashMap;

// Use a small number of shards (ShardBits=2 => 4 shards) for testing.
using TestMap = ConcurrentHashMap<int, std::string, std::hash<int>,
                                  std::equal_to<int>, 2>;

// Use a single ConcurrentHashMap for the entire test suite to avoid
// use-after-free in thread_local ThreadHandle across test instances.
class GetOrSetTest : public ::testing::Test {
protected:
    static TestMap* map_;

    static void SetUpTestSuite() {
        map_ = new TestMap();
    }
    static void TearDownTestSuite() {
        delete map_;
        map_ = nullptr;
    }

    void SetUp() override {
        map_->clear();
    }

    TestMap& map() { return *map_; }
};

TestMap* GetOrSetTest::map_ = nullptr;

// ---------------------------------------------------------------
// Value overload
// ---------------------------------------------------------------

TEST_F(GetOrSetTest, ValueOverload_KeyAbsent) {
    // When the key does not exist, get_or_set inserts the default value
    // and returns it.
    auto val = map().get_or_set(1, std::string("default_val"));
    EXPECT_EQ(val, "default_val");
    EXPECT_EQ(map().size(), 1u);

    // Verify it was actually inserted.
    auto result = map().find(1);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "default_val");
}

TEST_F(GetOrSetTest, ValueOverload_KeyExists) {
    // When the key already exists, get_or_set returns the existing value
    // without overwriting it.
    map().insert(1, "existing");

    auto val = map().get_or_set(1, std::string("default_val"));
    EXPECT_EQ(val, "existing");
    EXPECT_EQ(map().size(), 1u);

    // Verify original value is unchanged.
    auto result = map().find(1);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "existing");
}

// ---------------------------------------------------------------
// Factory overload
// ---------------------------------------------------------------

TEST_F(GetOrSetTest, FactoryOverload_KeyAbsent) {
    // When the key does not exist, the factory is called exactly once,
    // its result is inserted, and that value is returned.
    int call_count = 0;
    auto val = map().get_or_set(1,
        [&call_count]() {
            ++call_count;
            return std::string("factory_val");
        });

    EXPECT_EQ(val, "factory_val");
    EXPECT_EQ(call_count, 1);
    EXPECT_EQ(map().size(), 1u);

    // Verify the value was inserted.
    auto result = map().find(1);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "factory_val");
}

TEST_F(GetOrSetTest, FactoryOverload_KeyExists) {
    // When the key already exists, the factory is NOT called and the
    // existing value is returned.
    map().insert(1, "existing");

    bool factory_called = false;
    auto val = map().get_or_set(1,
        [&factory_called]() {
            factory_called = true;
            return std::string("should_not_use");
        });

    EXPECT_EQ(val, "existing");
    EXPECT_FALSE(factory_called);
    EXPECT_EQ(map().size(), 1u);

    // Verify original value is unchanged.
    auto result = map().find(1);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "existing");
}

// ---------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------

TEST_F(GetOrSetTest, ConcurrentGetOrSet_SameKey) {
    // N threads all call get_or_set with the same key and a factory.
    // The factory should be called exactly once (shard mutex guarantees
    // this), and every thread should observe the same value.
    const int N = 16;
    std::atomic<int> factory_call_count(0);
    std::vector<std::string> results(N);
    std::vector<std::thread> threads;
    threads.reserve(N);

    for (int i = 0; i < N; ++i) {
        threads.emplace_back([this, i, &factory_call_count, &results]() {
            results[i] = map().get_or_set(42,
                [&factory_call_count]() {
                    factory_call_count.fetch_add(1, std::memory_order_relaxed);
                    return std::string("the_value");
                });
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // The factory must have been invoked exactly once.
    EXPECT_EQ(factory_call_count.load(), 1);

    // All threads must have received the same value.
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(results[i], "the_value")
            << "Thread " << i << " got unexpected value: " << results[i];
    }

    // The map should contain exactly one element with the expected value.
    EXPECT_EQ(map().size(), 1u);
    auto result = map().find(42);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "the_value");
}
