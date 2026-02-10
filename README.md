# ConcurrentHashMap

A production-grade, header-only C++14 concurrent hash map with lock-free reads and per-shard locking for writes.

## Features

- **Lock-free reads** -- `find`, `contains`, and `count` never acquire a mutex
- **Per-shard write locking** -- writes to different shards proceed in parallel with no contention
- **Epoch-based memory reclamation** -- safe deferred freeing of old internal tables during resizes
- **Robin Hood open addressing** -- low variance probe distances, backward-shift deletion
- **Header-only** -- zero dependencies beyond the C++14 standard library
- **Configurable sharding** -- tunable shard count (default 64) to balance contention vs. memory
- **Pluggable mutex** -- swap in `std::mutex`, a coroutine-friendly mutex, or any `BasicLockable`
- **No iterators by design** -- concurrent iterators are either safety-hazardous or prohibitively expensive; this library avoids the footgun entirely
- **Value-copy semantics** -- `find()` returns by value, eliminating dangling-reference bugs

## Quick Start

ConcurrentHashMap is header-only. Copy or symlink the `include/concurrent_hashmap` directory into your project's include path, then:

```cpp
#include <concurrent_hashmap/concurrent_hashmap.h>

concurrent_hashmap::ConcurrentHashMap<std::string, int> map;

// Insert
map.insert("hello", 1);

// Lock-free lookup
auto result = map.find("hello");
if (result.second) {
    // use result.first (it is a copy)
}

// Insert-or-update
map.insert_or_assign("hello", 42);

// Lazy initialization
int v = map.get_or_set("counter", 0);

// Factory overload -- called only on first insertion
int v2 = map.get_or_set("expensive", []{ return compute_default(); });

// Erase
map.erase("hello");
```

If you use CMake, you can also add the project as a subdirectory:

```cmake
add_subdirectory(path/to/ConcurrentHashMap)
target_link_libraries(your_target PRIVATE concurrent_hashmap)
```

## API Reference

All public methods are on `concurrent_hashmap::ConcurrentHashMap<Key, Value, Hash, KeyEqual, ShardBits, Mutex>`.

### Lock-Free Reads

| Signature | Description |
|-----------|-------------|
| `std::pair<Value, bool> find(const Key& key) const` | Returns `{value, true}` if found, `{Value(), false}` otherwise. |
| `bool contains(const Key& key) const` | Returns `true` if the key exists. |
| `size_t count(const Key& key) const` | Returns `0` or `1`, matching `std::unordered_map::count` semantics. |

### Locked Writes

| Signature | Description |
|-----------|-------------|
| `bool insert(const Key& key, const Value& value)` | Inserts a key-value pair. Returns `true` if inserted, `false` if the key already exists. |
| `bool erase(const Key& key)` | Removes a key. Returns `true` if the key was found and removed. |
| `bool insert_or_assign(const Key& key, const Value& value)` | Inserts or overwrites. Returns `true` if newly inserted, `false` if an existing value was updated. |
| `template<class F> bool try_emplace(const Key& key, F&& factory)` | Inserts `factory()` as the value only if the key does not exist. Returns `true` if inserted. The factory is not called when the key is already present. |
| `Value get_or_set(const Key& key, const Value& default_value)` | Returns the existing value for the key, or inserts `default_value` and returns it. |
| `template<class F> Value get_or_set(const Key& key, F&& factory)` | Returns the existing value, or inserts `factory()` and returns it. SFINAE-guarded: enabled only when `F` is callable and not implicitly convertible to `Value`. |

### Utility

| Signature | Description |
|-----------|-------------|
| `size_t size() const` | Returns the approximate total number of elements (relaxed loads across shards). |
| `bool empty() const` | Returns `true` if `size() == 0`. |
| `void clear()` | Removes all elements from all shards. |
| `void reserve(size_t count)` | Pre-allocates capacity distributed evenly across shards. |

### Construction

The map is default-constructible. It is **non-copyable and non-movable**.

```cpp
static constexpr size_t kNumShards = 1 << ShardBits;  // e.g. 64
```

## Template Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `Key` | *(required)* | Key type. Must be hashable by `Hash` and comparable by `KeyEqual`. |
| `Value` | *(required)* | Mapped value type. Must be copyable, since `find()` returns by value. |
| `Hash` | `std::hash<Key>` | Hash function object type. |
| `KeyEqual` | `std::equal_to<Key>` | Key equality predicate. |
| `ShardBits` | `6` | `log2` of the number of shards. Default 6 gives 64 shards. Higher values reduce write contention at the cost of memory. |
| `Mutex` | `detail::SpinLock` | Per-shard mutex type. Must satisfy `BasicLockable` (`lock()` / `unlock()`). Replace with `std::mutex` for longer critical sections or a coroutine-friendly mutex for async workloads. |

## Thread Safety Guarantees

**Lock-free reads.** `find`, `contains`, and `count` execute without acquiring any mutex. Multiple readers can proceed concurrently and never block each other or writers.

**Per-shard locked writes.** `insert`, `erase`, `insert_or_assign`, `try_emplace`, and `get_or_set` acquire only the lock for the target shard. Writes to different shards proceed in parallel with zero contention.

**Approximate queries.** `size` and `empty` use relaxed atomic loads across all shards. The result is a consistent snapshot only if no concurrent modifications are in flight.

**Bulk operations.** `clear` and `reserve` iterate over all shards. They should be called when no other threads are performing concurrent operations, or with the understanding that they provide best-effort behavior under concurrency.

**Trivially copyable types.** For fully safe lock-free reads, `Key` and `Value` should be trivially copyable (e.g. `int`, `uint64_t`, POD structs). Using non-trivially-copyable types (e.g. `std::string`) with concurrent readers and writers to the same keys is **undefined behavior**. If you need non-trivially-copyable types, ensure external synchronization or accept that concurrent read/write access to the same shard is not safe.

## Performance Characteristics

The implementation includes several optimizations:

- **Hash caching** -- each slot stores the full hash value, avoiding recomputation during probing, resize, and comparison
- **Cache-line prefetching** -- `__builtin_prefetch` in probe loops to reduce cache misses on the hot path
- **Amortized epoch advancement** -- reduces mutex contention in the epoch-based reclamation system
- **Robin Hood probing** -- keeps probe distances short and uniform, improving both lookup and insertion throughput
- **Delayed shrink** -- avoids grow/shrink hysteresis by not immediately shrinking after deletions

### Benchmark Results

The project includes five benchmarks comparing `ConcurrentHashMap` against a `std::mutex`-protected `std::unordered_map` baseline:

| Benchmark | What It Measures |
|-----------|-----------------|
| `bench_read_heavy` | 95% reads / 5% writes |
| `bench_write_heavy` | 50% inserts / 50% erases |
| `bench_mixed` | Balanced read/write workloads |
| `bench_contention` | High contention on a small key set |
| `bench_scaling` | Throughput scaling from 1 to N threads |

The ConcurrentHashMap maintains throughput as thread count increases, while the `std::mutex + std::unordered_map` baseline degrades significantly under contention.

## Building Tests and Benchmarks

Requires CMake 3.14+ and a C++14-capable compiler. Tests use Google Test and benchmarks use Google Benchmark, both fetched automatically via CMake FetchContent.

```bash
# Build everything (Release mode recommended for benchmarks)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
ctest --test-dir build

# Run benchmarks (only built when sanitizers are off)
./build/benchmark/bench_read_heavy
./build/benchmark/bench_write_heavy
./build/benchmark/bench_mixed
./build/benchmark/bench_contention
./build/benchmark/bench_scaling
```

### Sanitizer Builds

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -B build-asan -DCHM_SANITIZER=asan
cmake --build build-asan
ctest --test-dir build-asan

# ThreadSanitizer
cmake -B build-tsan -DCHM_SANITIZER=tsan
cmake --build build-tsan
ctest --test-dir build-tsan
```

Note: benchmarks are automatically disabled when a sanitizer is enabled.

## Design Overview

The map partitions its key space into `2^ShardBits` independent shards. The high bits of each key's hash select the shard; the low bits index within that shard's Robin Hood open-addressing table. Each shard manages its own table, load factor, and resize logic independently.

Memory reclamation uses an epoch-based scheme: readers pin the current epoch on entry, and old table allocations are deferred until all pinned epochs have advanced past the retirement epoch. This allows resizes to proceed without blocking readers.

## License

MIT License

Copyright (c) 2026

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
