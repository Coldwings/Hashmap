#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

#include <concurrent_hashmap/detail/epoch.h>
#include <concurrent_hashmap/detail/hash_utils.h>
#include <concurrent_hashmap/detail/shard.h>
#include <concurrent_hashmap/detail/spinlock.h>

namespace concurrent_hashmap {

// =========================================================================
// ConcurrentHashMap
//
// A thread-safe hash map built on top of sharded Robin Hood hash tables
// with epoch-based memory reclamation.
//
// Template parameters:
//   Key       -- key type
//   Value     -- mapped value type
//   Hash      -- hash function (default: std::hash<Key>)
//   KeyEqual  -- key equality predicate (default: std::equal_to<Key>)
//   ShardBits -- log2 of the number of shards (default: 6 => 64 shards)
//   Mutex     -- per-shard mutex type (default: detail::SpinLock)
// =========================================================================
template <typename Key,
          typename Value,
          typename Hash     = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          uint8_t  ShardBits = 6,
          typename Mutex    = detail::SpinLock>
class ConcurrentHashMap {
public:
    static constexpr size_t kNumShards = size_t{1} << ShardBits;

    // ------------------------------------------------------------------
    // Construction / Destruction
    // ------------------------------------------------------------------
    ConcurrentHashMap() = default;
    ~ConcurrentHashMap() = default;

    // Non-copyable, non-movable.
    ConcurrentHashMap(const ConcurrentHashMap&) = delete;
    ConcurrentHashMap& operator=(const ConcurrentHashMap&) = delete;
    ConcurrentHashMap(ConcurrentHashMap&&) = delete;
    ConcurrentHashMap& operator=(ConcurrentHashMap&&) = delete;

    // ------------------------------------------------------------------
    // Lock-free reads
    // ------------------------------------------------------------------

    /// Look up a key.  Returns {value, true} if found, {Value(), false} if not.
    std::pair<Value, bool> find(const Key& key) const {
        detail::EpochGuard guard(epoch_);
        size_t h = hash_(key);
        return shard_for(h).find(h, key);
    }

    /// Returns true if the key exists.
    bool contains(const Key& key) const {
        detail::EpochGuard guard(epoch_);
        size_t h = hash_(key);
        return shard_for(h).contains(h, key);
    }

    /// Returns 0 or 1 (like std::unordered_map::count for unique keys).
    size_t count(const Key& key) const {
        return contains(key) ? 1 : 0;
    }

    // ------------------------------------------------------------------
    // Locked writes
    // ------------------------------------------------------------------

    /// Insert a key-value pair.  Returns true if inserted, false if key exists.
    bool insert(const Key& key, const Value& value) {
        detail::EpochGuard guard(epoch_);
        size_t h = hash_(key);
        return shard_for(h).insert(h, key, value, epoch_);
    }

    /// Erase a key.  Returns true if the key was found and erased.
    bool erase(const Key& key) {
        detail::EpochGuard guard(epoch_);
        size_t h = hash_(key);
        return shard_for(h).erase(h, key, epoch_);
    }

    /// Insert or update.  Returns true if newly inserted, false if updated.
    bool insert_or_assign(const Key& key, const Value& value) {
        detail::EpochGuard guard(epoch_);
        size_t h = hash_(key);
        return shard_for(h).insert_or_assign(h, key, value, epoch_);
    }

    /// Try to emplace using a factory.  Returns true if inserted; factory is
    /// only called when the key does not already exist.
    template <typename F>
    bool try_emplace(const Key& key, F&& factory) {
        detail::EpochGuard guard(epoch_);
        size_t h = hash_(key);
        return shard_for(h).try_emplace(h, key, std::forward<F>(factory), epoch_);
    }

    /// Return existing value, or insert default_value and return it.
    Value get_or_set(const Key& key, const Value& default_value) {
        detail::EpochGuard guard(epoch_);
        size_t h = hash_(key);
        return shard_for(h).get_or_set(h, key, default_value, epoch_);
    }

    /// Return existing value, or call factory(), insert result, and return it.
    /// SFINAE: enabled only when F is callable (not implicitly convertible to Value).
    template <typename F,
              typename = typename std::enable_if<
                  !std::is_convertible<typename std::decay<F>::type, Value>::value
              >::type>
    Value get_or_set(const Key& key, F&& factory) {
        detail::EpochGuard guard(epoch_);
        size_t h = hash_(key);
        return shard_for(h).get_or_set_f(h, key, std::forward<F>(factory), epoch_);
    }

    // ------------------------------------------------------------------
    // Utility
    // ------------------------------------------------------------------

    /// Approximate total size (sum of all shard sizes with relaxed loads).
    size_t size() const {
        size_t total = 0;
        for (size_t i = 0; i < kNumShards; ++i) {
            total += shards_[i].size();
        }
        return total;
    }

    /// Returns true if the map appears empty.
    bool empty() const {
        return size() == 0;
    }

    /// Clear all shards.
    void clear() {
        detail::EpochGuard guard(epoch_);
        for (size_t i = 0; i < kNumShards; ++i) {
            shards_[i].clear(epoch_);
        }
    }

    /// Reserve capacity distributed evenly across shards.
    void reserve(size_t count) {
        detail::EpochGuard guard(epoch_);
        size_t per_shard = count / kNumShards + (count % kNumShards != 0 ? 1 : 0);
        for (size_t i = 0; i < kNumShards; ++i) {
            shards_[i].reserve(per_shard, epoch_);
        }
    }

private:
    using ShardType = detail::Shard<Key, Value, Hash, KeyEqual, Mutex>;

    // The epoch manager must be declared before shards so that it is
    // destroyed after the shards (shards may reference it during cleanup).
    // Mutable because const read operations still need to pin the epoch.
    mutable detail::EpochManager epoch_;
    mutable std::array<ShardType, kNumShards> shards_;

    Hash hash_;

    // Route to the correct shard using the high bits of the hash.
    ShardType& shard_for(size_t hash) {
        return shards_[detail::shard_index<ShardBits>(hash)];
    }

    const ShardType& shard_for(size_t hash) const {
        return shards_[detail::shard_index<ShardBits>(hash)];
    }
};

}  // namespace concurrent_hashmap
