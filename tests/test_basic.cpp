#include <gtest/gtest.h>
#include <concurrent_hashmap/concurrent_hashmap.h>
#include <string>

using concurrent_hashmap::ConcurrentHashMap;

// Use a small number of shards (ShardBits=2 => 4 shards) for testing
// to get reasonable coverage with small key sets.
using TestMap = ConcurrentHashMap<int, std::string, std::hash<int>,
                                  std::equal_to<int>, 2>;

// Default-parameter map for verifying default template instantiation.
using DefaultMap = ConcurrentHashMap<int, int>;

// Use a single ConcurrentHashMap for the entire test suite to avoid
// use-after-free in thread_local ThreadHandle across test instances.
// The EpochManager::get_thread_entry() caches a ThreadEntry in a
// thread_local; destroying one EpochManager and creating another
// leaves a dangling pointer in that thread_local.
//
// Both map types are held in the same fixture so they share a single
// destruction point, avoiding the dangling-pointer problem between
// different test suites.
class ConcurrentHashMapTest : public ::testing::Test {
protected:
    static TestMap*    map_;
    static DefaultMap* default_map_;

    static void SetUpTestSuite() {
        map_ = new TestMap();
        default_map_ = new DefaultMap();
    }
    static void TearDownTestSuite() {
        delete default_map_;
        default_map_ = nullptr;
        delete map_;
        map_ = nullptr;
    }

    void SetUp() override {
        map_->clear();
        default_map_->clear();
    }

    TestMap&    map()         { return *map_; }
    DefaultMap& default_map() { return *default_map_; }
};

TestMap*    ConcurrentHashMapTest::map_ = nullptr;
DefaultMap* ConcurrentHashMapTest::default_map_ = nullptr;

TEST_F(ConcurrentHashMapTest, InsertAndFind) {
    EXPECT_TRUE(map().insert(42, "hello"));
    auto result = map().find(42);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "hello");
}

TEST_F(ConcurrentHashMapTest, InsertDuplicate) {
    EXPECT_TRUE(map().insert(1, "first"));
    EXPECT_FALSE(map().insert(1, "second"));

    auto result = map().find(1);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "first");
}

TEST_F(ConcurrentHashMapTest, FindMissing) {
    auto result = map().find(999);
    EXPECT_FALSE(result.second);
    EXPECT_EQ(result.first, std::string());
}

TEST_F(ConcurrentHashMapTest, EraseExisting) {
    map().insert(10, "ten");
    EXPECT_TRUE(map().erase(10));
    EXPECT_FALSE(map().contains(10));
    EXPECT_EQ(map().size(), 0u);
}

TEST_F(ConcurrentHashMapTest, EraseMissing) {
    EXPECT_FALSE(map().erase(10));
}

TEST_F(ConcurrentHashMapTest, Contains) {
    EXPECT_FALSE(map().contains(5));
    map().insert(5, "five");
    EXPECT_TRUE(map().contains(5));
}

TEST_F(ConcurrentHashMapTest, Count) {
    EXPECT_EQ(map().count(5), 0u);
    map().insert(5, "five");
    EXPECT_EQ(map().count(5), 1u);
}

TEST_F(ConcurrentHashMapTest, SizeAndEmpty) {
    EXPECT_EQ(map().size(), 0u);
    EXPECT_TRUE(map().empty());

    map().insert(1, "a");
    EXPECT_EQ(map().size(), 1u);
    EXPECT_FALSE(map().empty());

    map().insert(2, "b");
    EXPECT_EQ(map().size(), 2u);

    map().erase(1);
    EXPECT_EQ(map().size(), 1u);
}

TEST_F(ConcurrentHashMapTest, Clear) {
    for (int i = 0; i < 10; ++i) {
        map().insert(i, std::to_string(i));
    }
    EXPECT_EQ(map().size(), 10u);

    map().clear();
    EXPECT_EQ(map().size(), 0u);

    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(map().contains(i));
    }
}

TEST_F(ConcurrentHashMapTest, InsertOrAssignNewKey) {
    // Insert a new key -- returns true.
    EXPECT_TRUE(map().insert_or_assign(1, "first"));
    auto result = map().find(1);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "first");
}

TEST_F(ConcurrentHashMapTest, InsertOrAssignExistingKey) {
    map().insert(1, "first");

    // Assign to existing key -- returns false.
    EXPECT_FALSE(map().insert_or_assign(1, "updated"));
    auto result = map().find(1);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "updated");
    EXPECT_EQ(map().size(), 1u);
}

TEST_F(ConcurrentHashMapTest, TryEmplaceNewKey) {
    bool inserted = map().try_emplace(1,
        []() { return std::string("created"); });
    EXPECT_TRUE(inserted);

    auto result = map().find(1);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "created");
}

TEST_F(ConcurrentHashMapTest, TryEmplaceExistingKey) {
    map().insert(1, "original");

    bool factory_called = false;
    bool inserted = map().try_emplace(1,
        [&factory_called]() {
            factory_called = true;
            return std::string("should_not_insert");
        });

    EXPECT_FALSE(inserted);
    EXPECT_FALSE(factory_called);

    auto result = map().find(1);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "original");
}

TEST_F(ConcurrentHashMapTest, GetOrSetValueAbsent) {
    auto val = map().get_or_set(1, std::string("default_val"));
    EXPECT_EQ(val, "default_val");
    EXPECT_EQ(map().size(), 1u);

    // Verify it was actually inserted.
    auto result = map().find(1);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "default_val");
}

TEST_F(ConcurrentHashMapTest, GetOrSetValuePresent) {
    map().insert(1, "existing");
    auto val = map().get_or_set(1, std::string("default_val"));
    EXPECT_EQ(val, "existing");
    EXPECT_EQ(map().size(), 1u);
}

TEST_F(ConcurrentHashMapTest, GetOrSetFactoryAbsent) {
    auto val = map().get_or_set(1,
        []() { return std::string("factory_val"); });
    EXPECT_EQ(val, "factory_val");
    EXPECT_EQ(map().size(), 1u);
}

TEST_F(ConcurrentHashMapTest, GetOrSetFactoryPresent) {
    map().insert(1, "existing");

    bool factory_called = false;
    auto val = map().get_or_set(1,
        [&factory_called]() {
            factory_called = true;
            return std::string("should_not_use");
        });

    EXPECT_EQ(val, "existing");
    EXPECT_FALSE(factory_called);
}

TEST_F(ConcurrentHashMapTest, ManyInserts) {
    const int N = 2000;
    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(map().insert(i, std::to_string(i)));
    }
    EXPECT_EQ(map().size(), static_cast<size_t>(N));

    for (int i = 0; i < N; ++i) {
        auto result = map().find(i);
        EXPECT_TRUE(result.second) << "key " << i << " not found";
        EXPECT_EQ(result.first, std::to_string(i));
    }
}

TEST_F(ConcurrentHashMapTest, EraseAndReinsert) {
    map().insert(1, "v1");
    EXPECT_TRUE(map().erase(1));
    EXPECT_FALSE(map().contains(1));

    // Re-insert with a different value.
    EXPECT_TRUE(map().insert(1, "v2"));
    auto result = map().find(1);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first, "v2");
}

TEST_F(ConcurrentHashMapTest, BackwardShiftDeleteChain) {
    // Insert several elements.
    for (int i = 0; i < 10; ++i) {
        map().insert(i, std::to_string(i));
    }

    // Erase some from the beginning.
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(map().erase(i));
    }
    EXPECT_EQ(map().size(), 5u);

    // All remaining must be findable.
    for (int i = 5; i < 10; ++i) {
        auto result = map().find(i);
        EXPECT_TRUE(result.second) << "key " << i << " not found after erase";
        EXPECT_EQ(result.first, std::to_string(i));
    }

    // All erased must not be found.
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(map().contains(i));
    }
}

TEST_F(ConcurrentHashMapTest, Reserve) {
    // Reserve space for 1000 elements.
    map().reserve(1000);

    // Insert 500 elements.
    for (int i = 0; i < 500; ++i) {
        EXPECT_TRUE(map().insert(i, std::to_string(i)));
    }
    EXPECT_EQ(map().size(), 500u);

    // Verify all findable.
    for (int i = 0; i < 500; ++i) {
        auto result = map().find(i);
        EXPECT_TRUE(result.second) << "key " << i << " not found";
        EXPECT_EQ(result.first, std::to_string(i));
    }
}

TEST_F(ConcurrentHashMapTest, DefaultShardBits) {
    // Verify the default template instantiation (64 shards) compiles and works.
    default_map().insert(1, 100);
    default_map().insert(2, 200);

    auto r1 = default_map().find(1);
    EXPECT_TRUE(r1.second);
    EXPECT_EQ(r1.first, 100);

    auto r2 = default_map().find(2);
    EXPECT_TRUE(r2.second);
    EXPECT_EQ(r2.first, 200);

    EXPECT_EQ(default_map().size(), 2u);
}
