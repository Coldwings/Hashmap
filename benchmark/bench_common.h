#pragma once

#include <concurrent_hashmap/concurrent_hashmap.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>

// ===========================================================================
// MixHash
//
// Murmur3-style finalizer that distributes small integer keys across all
// shards.  std::hash<int> is often the identity function, which sends all
// small keys to shard 0.
// ===========================================================================
struct MixHash {
    size_t operator()(int key) const noexcept {
        auto x = static_cast<size_t>(static_cast<unsigned int>(key));
        x ^= x >> 16;
        x *= 0x45d9f3bUL;
        x ^= x >> 16;
        x *= 0x45d9f3bUL;
        x ^= x >> 16;
        return x | (x << 32);
    }
};

// ===========================================================================
// FastRng -- simple LCG for per-thread random key generation
//
// Each thread should hold its own instance to avoid contention.
// ===========================================================================
class FastRng {
public:
    explicit FastRng(uint64_t seed) : state_(seed) {}

    uint32_t operator()() {
        // LCG with Knuth's constants
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state_ >> 33);
    }

    uint32_t next_in_range(uint32_t range) {
        return (*this)() % range;
    }

private:
    uint64_t state_;
};

// ===========================================================================
// StdMutexMap<K, V>
//
// A baseline concurrent map: std::unordered_map guarded by a single
// std::mutex.  Provides the same API surface as ConcurrentHashMap for
// fair benchmark comparisons.
// ===========================================================================
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class StdMutexMap {
public:
    StdMutexMap() = default;

    bool insert(const Key& key, const Value& value) {
        std::lock_guard<std::mutex> lock(mu_);
        return map_.emplace(key, value).second;
    }

    std::pair<Value, bool> find(const Key& key) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            return {it->second, true};
        }
        return {Value(), false};
    }

    bool erase(const Key& key) {
        std::lock_guard<std::mutex> lock(mu_);
        return map_.erase(key) > 0;
    }

    bool contains(const Key& key) const {
        std::lock_guard<std::mutex> lock(mu_);
        return map_.count(key) > 0;
    }

    Value get_or_set(const Key& key, const Value& default_value) {
        std::lock_guard<std::mutex> lock(mu_);
        auto result = map_.emplace(key, default_value);
        return result.first->second;
    }

    bool insert_or_assign(const Key& key, const Value& value) {
        std::lock_guard<std::mutex> lock(mu_);
        auto result = map_.emplace(key, value);
        if (!result.second) {
            result.first->second = value;
            return false;  // updated
        }
        return true;  // inserted
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return map_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        map_.clear();
    }

    void reserve(size_t count) {
        std::lock_guard<std::mutex> lock(mu_);
        map_.reserve(count);
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<Key, Value, Hash> map_;
};

// Convenient type aliases for benchmarks.
using ConcurrentMap = concurrent_hashmap::ConcurrentHashMap<
    int, int, MixHash, std::equal_to<int>, 6>;

using BaselineMap = StdMutexMap<int, int, MixHash>;

// ===========================================================================
// MapHolder -- manages map lifetime safely across benchmark runs.
//
// The ConcurrentHashMap uses an EpochManager that registers thread-local
// state.  Deleting the map while thread-local entries still reference it
// leads to corrupted-list errors.  MapHolder heap-allocates the map once
// (per type) and reuses it across all benchmark invocations, clearing it
// between runs as needed.
// ===========================================================================
template <typename MapType>
struct MapHolder {
    static MapType& get() {
        static MapType* instance = new MapType();
        return *instance;
    }

    // Prepare the map for a new benchmark: clear and optionally pre-fill.
    // Must be called from thread 0 only.
    static void reset() {
        get().clear();
    }

    static void prefill(int count) {
        auto& m = get();
        m.reserve(count);
        for (int i = 0; i < count; ++i) {
            m.insert(i, i);
        }
    }
};

using ConcurrentHolder = MapHolder<ConcurrentMap>;
using BaselineHolder   = MapHolder<BaselineMap>;
