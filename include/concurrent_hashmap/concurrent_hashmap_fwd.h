#pragma once

#include <cstdint>
#include <functional>

#include <concurrent_hashmap/detail/spinlock.h>

namespace concurrent_hashmap {

template <typename Key,
          typename Value,
          typename Hash     = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          uint8_t  ShardBits = 6,
          typename Mutex    = detail::SpinLock>
class ConcurrentHashMap;

}  // namespace concurrent_hashmap
