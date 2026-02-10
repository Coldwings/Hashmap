#pragma once
#include <cstddef>
#include <cstdint>

namespace concurrent_hashmap {
namespace detail {

// Extract high bits of hash for shard routing
// ShardBits determines how many bits to use (e.g., 6 for 64 shards)
template <uint8_t ShardBits>
inline size_t shard_index(size_t hash) {
    return hash >> (sizeof(size_t) * 8 - ShardBits);
}

// Extract remaining bits for in-shard table indexing
// This returns the full hash (used with mask inside shard)
// We keep the full hash so Robin Hood probing uses all bits
inline size_t in_shard_hash(size_t hash) {
    return hash;
}

// Round up to next power of 2 (or return n if already power of 2)
// Minimum return value is 1
inline size_t next_power_of_2(size_t n) {
    if (n == 0) return 1;
    if ((n & (n - 1)) == 0) return n;  // already power of 2
    size_t result = 1;
    while (result < n) {
        result <<= 1;
    }
    return result;
}

// Check if n is a power of 2
inline bool is_power_of_2(size_t n) {
    return n > 0 && (n & (n - 1)) == 0;
}

} // namespace detail
} // namespace concurrent_hashmap
