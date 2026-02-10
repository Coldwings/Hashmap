#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

#include <concurrent_hashmap/detail/epoch.h>
#include <concurrent_hashmap/detail/hash_utils.h>
#include <concurrent_hashmap/detail/spinlock.h>

namespace concurrent_hashmap {
namespace detail {

// Concurrency contract: lock-free readers may observe slots mid-mutation.
// This is safe when Key and Value are trivially copyable (e.g. int, POD).
// For non-trivially-copyable types (e.g. std::string), concurrent reads
// during writes are data races.  The public ConcurrentHashMap layer must
// ensure readers and writers to the SAME shard are properly synchronized,
// or restrict Key/Value to trivially-copyable types for lock-free reads.
template <typename Key, typename Value,
          typename Hash     = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Mutex    = SpinLock>
class Shard {
public:
    // ------------------------------------------------------------------
    // Slot -- one bucket in the Robin Hood table.
    // dist == 0 means empty.  dist == 1 means home position.
    // dist == k means the element is displaced k-1 positions from home.
    //
    // hash is cached to avoid recomputing during resize and to enable
    // fast early-exit comparisons (compare hash before comparing key).
    // ------------------------------------------------------------------
    struct Slot {
        uint8_t dist;
        size_t  hash;
        Key     key;
        Value   value;

        Slot() : dist(0), hash(0), key(), value() {}
    };

    // ------------------------------------------------------------------
    // Table -- heap-allocated slot array, inherits from Retired so it
    // can be deferred-freed through epoch-based reclamation.
    // ------------------------------------------------------------------
    struct Table : EpochManager::Retired {
        size_t capacity;
        size_t mask;    // capacity - 1
        Slot*  slots;

        explicit Table(size_t cap)
            : capacity(cap), mask(cap - 1), slots(new Slot[cap]()) {}

        ~Table() override { delete[] slots; }
    };

    // ------------------------------------------------------------------
    // Construction / Destruction
    // ------------------------------------------------------------------
    Shard() : table_(new Table(kDefaultCapacity)), size_(0), shrink_counter_(0) {}

    explicit Shard(size_t initial_capacity)
        : table_(new Table(initial_capacity < kDefaultCapacity
                           ? kDefaultCapacity
                           : next_power_of_2(initial_capacity)))
        , size_(0)
        , shrink_counter_(0)
    {}

    ~Shard() {
        Table* t = table_.load(std::memory_order_relaxed);
        delete t;
    }

    // Non-copyable, non-movable.
    Shard(const Shard&) = delete;
    Shard& operator=(const Shard&) = delete;

    // ------------------------------------------------------------------
    // Lock-free reads (caller must hold an EpochGuard)
    // ------------------------------------------------------------------
    std::pair<Value, bool> find(size_t hash, const Key& key) const {
        const Table* t = table_.load(std::memory_order_acquire);
        size_t pos = hash & t->mask;
        uint8_t expected_dist = 1;

        for (;;) {
            const Slot& s = t->slots[pos];
            if (s.dist == 0) {
                return std::pair<Value, bool>(Value(), false);
            }
            if (s.dist < expected_dist) {
                return std::pair<Value, bool>(Value(), false);
            }
            // Prefetch next probe position to hide memory latency.
            __builtin_prefetch(&t->slots[(pos + 1) & t->mask], 0, 1);
            if (s.dist == expected_dist && s.hash == hash &&
                KeyEqual()(s.key, key)) {
                return std::pair<Value, bool>(s.value, true);
            }
            pos = (pos + 1) & t->mask;
            ++expected_dist;
            if (expected_dist == 0) {
                // wrapped around 255 -- impossible at sane load factors
                return std::pair<Value, bool>(Value(), false);
            }
        }
    }

    bool contains(size_t hash, const Key& key) const {
        const Table* t = table_.load(std::memory_order_acquire);
        return find_in_table(t, hash, key) != nullptr;
    }

    // ------------------------------------------------------------------
    // Locked writes (caller must hold an EpochGuard)
    // ------------------------------------------------------------------
    bool insert(size_t hash, const Key& key, const Value& value,
                EpochManager& epoch) {
        std::lock_guard<Mutex> lk(mutex_);
        Table* t = table_.load(std::memory_order_relaxed);

        // Check for existing key first.
        if (find_in_table(t, hash, key) != nullptr) {
            return false;  // key already exists
        }

        // Expand before insert to guarantee sufficient capacity.
        maybe_expand_for_insert(epoch);
        t = table_.load(std::memory_order_relaxed);

        while (!insert_into_table(t, hash, key, value)) {
            resize(t->capacity * 2, epoch);
            t = table_.load(std::memory_order_relaxed);
        }
        size_.fetch_add(1, std::memory_order_relaxed);
        shrink_counter_ = 0;
        return true;
    }

    bool erase(size_t hash, const Key& key, EpochManager& epoch) {
        std::lock_guard<Mutex> lk(mutex_);
        Table* t = table_.load(std::memory_order_relaxed);

        size_t pos = hash & t->mask;
        uint8_t expected_dist = 1;

        // Find the key.
        for (;;) {
            Slot& s = t->slots[pos];
            if (s.dist == 0) return false;
            if (s.dist < expected_dist) return false;
            if (s.dist == expected_dist && s.hash == hash &&
                KeyEqual()(s.key, key)) {
                break;  // found at pos
            }
            pos = (pos + 1) & t->mask;
            ++expected_dist;
            if (expected_dist == 0) return false;
        }

        // Backward-shift delete: shift subsequent elements backward.
        for (;;) {
            size_t next_pos = (pos + 1) & t->mask;
            Slot& next_slot = t->slots[next_pos];
            if (next_slot.dist <= 1) {
                // next is empty (dist==0) or at home (dist==1): stop.
                // Reset the entire slot to release held resources.
                t->slots[pos] = Slot();
                break;
            }
            // Move next_slot backward into pos, decrement its dist.
            t->slots[pos].key   = std::move(next_slot.key);
            t->slots[pos].value = std::move(next_slot.value);
            t->slots[pos].hash  = next_slot.hash;
            t->slots[pos].dist  = next_slot.dist - 1;
            pos = next_pos;
        }

        size_.fetch_sub(1, std::memory_order_relaxed);
        maybe_shrink(epoch);
        return true;
    }

    bool insert_or_assign(size_t hash, const Key& key, const Value& value,
                          EpochManager& epoch) {
        std::lock_guard<Mutex> lk(mutex_);
        Table* t = table_.load(std::memory_order_relaxed);

        Slot* existing = find_in_table_mut(t, hash, key);
        if (existing) {
            existing->value = value;
            return false;  // updated, not inserted
        }

        maybe_expand_for_insert(epoch);
        t = table_.load(std::memory_order_relaxed);

        while (!insert_into_table(t, hash, key, value)) {
            resize(t->capacity * 2, epoch);
            t = table_.load(std::memory_order_relaxed);
        }
        size_.fetch_add(1, std::memory_order_relaxed);
        shrink_counter_ = 0;
        return true;  // newly inserted
    }

    Value get_or_set(size_t hash, const Key& key, const Value& default_value,
                     EpochManager& epoch) {
        std::lock_guard<Mutex> lk(mutex_);
        Table* t = table_.load(std::memory_order_relaxed);

        const Slot* existing = find_in_table(t, hash, key);
        if (existing) {
            return existing->value;
        }

        maybe_expand_for_insert(epoch);
        t = table_.load(std::memory_order_relaxed);

        while (!insert_into_table(t, hash, key, default_value)) {
            resize(t->capacity * 2, epoch);
            t = table_.load(std::memory_order_relaxed);
        }
        size_.fetch_add(1, std::memory_order_relaxed);
        shrink_counter_ = 0;
        return default_value;
    }

    template <typename F>
    Value get_or_set_f(size_t hash, const Key& key, F&& factory,
                       EpochManager& epoch) {
        std::lock_guard<Mutex> lk(mutex_);
        Table* t = table_.load(std::memory_order_relaxed);

        const Slot* existing = find_in_table(t, hash, key);
        if (existing) {
            return existing->value;
        }

        maybe_expand_for_insert(epoch);
        t = table_.load(std::memory_order_relaxed);

        Value val = factory();
        while (!insert_into_table(t, hash, key, val)) {
            resize(t->capacity * 2, epoch);
            t = table_.load(std::memory_order_relaxed);
        }
        size_.fetch_add(1, std::memory_order_relaxed);
        shrink_counter_ = 0;
        return val;
    }

    template <typename F>
    bool try_emplace(size_t hash, const Key& key, F&& factory,
                     EpochManager& epoch) {
        std::lock_guard<Mutex> lk(mutex_);
        Table* t = table_.load(std::memory_order_relaxed);

        if (find_in_table(t, hash, key) != nullptr) {
            return false;
        }

        maybe_expand_for_insert(epoch);
        t = table_.load(std::memory_order_relaxed);

        Value val = factory();
        while (!insert_into_table(t, hash, key, val)) {
            resize(t->capacity * 2, epoch);
            t = table_.load(std::memory_order_relaxed);
        }
        size_.fetch_add(1, std::memory_order_relaxed);
        shrink_counter_ = 0;
        return true;
    }

    // ------------------------------------------------------------------
    // Utility
    // ------------------------------------------------------------------
    size_t size() const {
        return size_.load(std::memory_order_relaxed);
    }

    void clear(EpochManager& epoch) {
        std::lock_guard<Mutex> lk(mutex_);
        Table* old_table = table_.load(std::memory_order_relaxed);
        Table* new_table = new Table(kDefaultCapacity);
        table_.store(new_table, std::memory_order_release);
        size_.store(0, std::memory_order_relaxed);
        shrink_counter_ = 0;
        epoch.retire(old_table);
    }

    void reserve(size_t count, EpochManager& epoch) {
        std::lock_guard<Mutex> lk(mutex_);
        // We need capacity such that count / capacity <= kMaxLoadFactor.
        // So capacity >= count / kMaxLoadFactor.
        size_t needed = static_cast<size_t>(
            static_cast<double>(count) / kMaxLoadFactor) + 1;
        needed = next_power_of_2(needed);
        if (needed < kDefaultCapacity) needed = kDefaultCapacity;

        Table* t = table_.load(std::memory_order_relaxed);
        if (needed <= t->capacity) return;

        resize(needed, epoch);
    }

private:
    std::atomic<Table*> table_;
    Mutex               mutex_;
    std::atomic<size_t> size_;
    size_t              shrink_counter_;

    static const size_t  kDefaultCapacity = 16;
    static const uint8_t kMaxDist = 128;

    static constexpr double kMaxLoadFactor    = 0.75;
    static constexpr double kShrinkLoadFactor = 0.15;

    // ------------------------------------------------------------------
    // find_in_table -- const version, returns const Slot* or nullptr.
    // ------------------------------------------------------------------
    const Slot* find_in_table(const Table* t, size_t hash,
                              const Key& key) const {
        size_t pos = hash & t->mask;
        uint8_t expected_dist = 1;

        for (;;) {
            const Slot& s = t->slots[pos];
            if (s.dist == 0) return nullptr;
            if (s.dist < expected_dist) return nullptr;
            __builtin_prefetch(&t->slots[(pos + 1) & t->mask], 0, 1);
            if (s.dist == expected_dist && s.hash == hash &&
                KeyEqual()(s.key, key)) {
                return &s;
            }
            pos = (pos + 1) & t->mask;
            ++expected_dist;
            if (expected_dist == 0) return nullptr;
        }
    }

    // ------------------------------------------------------------------
    // find_in_table_mut -- mutable version for write operations.
    // ------------------------------------------------------------------
    Slot* find_in_table_mut(Table* t, size_t hash, const Key& key) {
        size_t pos = hash & t->mask;
        uint8_t expected_dist = 1;

        for (;;) {
            Slot& s = t->slots[pos];
            if (s.dist == 0) return nullptr;
            if (s.dist < expected_dist) return nullptr;
            __builtin_prefetch(&t->slots[(pos + 1) & t->mask], 0, 1);
            if (s.dist == expected_dist && s.hash == hash &&
                KeyEqual()(s.key, key)) {
                return &s;
            }
            pos = (pos + 1) & t->mask;
            ++expected_dist;
            if (expected_dist == 0) return nullptr;
        }
    }

    // ------------------------------------------------------------------
    // insert_into_table -- Robin Hood insertion.  Does NOT check for
    // duplicates.  The caller must do that.
    //
    // Returns true on success, false if max probe distance exceeded
    // (caller must resize and retry).
    // ------------------------------------------------------------------
    bool insert_into_table(Table* t, size_t hash,
                           const Key& key, const Value& value) {
        size_t pos = hash & t->mask;
        uint8_t cur_dist = 1;
        size_t cur_hash  = hash;
        Key   cur_key   = key;
        Value cur_value = value;

        for (;;) {
            Slot& s = t->slots[pos];

            if (s.dist == 0) {
                s.dist  = cur_dist;
                s.hash  = cur_hash;
                s.key   = std::move(cur_key);
                s.value = std::move(cur_value);
                return true;
            }

            if (s.dist < cur_dist) {
                // Robin Hood: steal from the rich.
                std::swap(cur_dist,  s.dist);
                std::swap(cur_hash,  s.hash);
                std::swap(cur_key,   s.key);
                std::swap(cur_value, s.value);
            }

            pos = (pos + 1) & t->mask;
            ++cur_dist;

            if (cur_dist >= kMaxDist) {
                // Undo the partial Robin Hood swaps by forcing a resize.
                // The caller will resize (which rehashes everything
                // including the element we were trying to insert).
                return false;
            }
        }
    }

    // ------------------------------------------------------------------
    // Robin Hood insertion during resize (directly into the given table).
    // Identical logic but operates on an explicit table pointer.
    // ------------------------------------------------------------------
    void rehash_insert(Table* t, Key key, Value value, size_t hash) {
        size_t pos = hash & t->mask;
        uint8_t cur_dist = 1;
        size_t cur_hash  = hash;
        Key   cur_key   = std::move(key);
        Value cur_value = std::move(value);

        for (;;) {
            Slot& s = t->slots[pos];

            if (s.dist == 0) {
                s.dist  = cur_dist;
                s.hash  = cur_hash;
                s.key   = std::move(cur_key);
                s.value = std::move(cur_value);
                return;
            }

            if (s.dist < cur_dist) {
                std::swap(cur_dist,  s.dist);
                std::swap(cur_hash,  s.hash);
                std::swap(cur_key,   s.key);
                std::swap(cur_value, s.value);
            }

            pos = (pos + 1) & t->mask;
            ++cur_dist;
            assert(cur_dist != 0 && "rehash_insert: probe distance overflow");
        }
    }

    // ------------------------------------------------------------------
    // resize -- allocate new table, rehash, atomic swap, retire old.
    // Must be called under mutex_.
    // ------------------------------------------------------------------
    void resize(size_t new_capacity, EpochManager& epoch) {
        Table* old_table = table_.load(std::memory_order_relaxed);
        Table* new_table = new Table(new_capacity);

        for (size_t i = 0; i < old_table->capacity; ++i) {
            Slot& s = old_table->slots[i];
            if (s.dist != 0) {
                // Use cached hash -- no need to recompute.
                rehash_insert(new_table, std::move(s.key),
                              std::move(s.value), s.hash);
            }
        }

        table_.store(new_table, std::memory_order_release);
        epoch.retire(old_table);
    }

    // ------------------------------------------------------------------
    // maybe_expand_for_insert -- expand BEFORE inserting a new element.
    // Checks whether (current_size + 1) would exceed the load factor.
    // Must be called under mutex_.
    // ------------------------------------------------------------------
    void maybe_expand_for_insert(EpochManager& epoch) {
        Table* t = table_.load(std::memory_order_relaxed);
        size_t sz = size_.load(std::memory_order_relaxed);
        if (static_cast<double>(sz + 1) >
            static_cast<double>(t->capacity) * kMaxLoadFactor) {
            resize(t->capacity * 2, epoch);
        }
    }

    // ------------------------------------------------------------------
    // maybe_shrink -- delayed shrink after erase.
    // Must be called under mutex_.
    // ------------------------------------------------------------------
    void maybe_shrink(EpochManager& epoch) {
        Table* t = table_.load(std::memory_order_relaxed);
        size_t sz = size_.load(std::memory_order_relaxed);
        double load = static_cast<double>(sz) /
                      static_cast<double>(t->capacity);

        if (load < kShrinkLoadFactor && t->capacity > kDefaultCapacity) {
            ++shrink_counter_;
            if (shrink_counter_ > t->capacity) {
                size_t new_cap = t->capacity / 2;
                if (new_cap < kDefaultCapacity) new_cap = kDefaultCapacity;
                resize(new_cap, epoch);
                shrink_counter_ = 0;
            }
        } else {
            shrink_counter_ = 0;
        }
    }
};

} // namespace detail
} // namespace concurrent_hashmap
