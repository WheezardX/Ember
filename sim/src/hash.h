#pragma once
// Bit-exact port of terrain/veg/hashing.py (Epic 2 E1 determinism spec) plus the state
// hash. Every random draw in the sim is hash64(run_seed, SYSTEM, entity, tick, k); there is
// no RNG state anywhere. Golden vectors vs the Python implementation live in tests/.
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace embersim {

inline constexpr uint64_t HASH_GOLDEN = 0x9E3779B97F4A7C15ull;

inline constexpr uint64_t splitmix64(uint64_t x) noexcept {
    uint64_t z = x + HASH_GOLDEN;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// hash64(*values): fold each input into the accumulator via splitmix64(h ^ v).
inline constexpr uint64_t hash64(std::initializer_list<uint64_t> values) noexcept {
    uint64_t h = 0;
    for (uint64_t v : values) h = splitmix64(h ^ v);
    return h;
}
template <class... Ts>
inline constexpr uint64_t hash64(Ts... vs) noexcept {
    return hash64({static_cast<uint64_t>(vs)...});
}

// Stream ids (ADR 0008 §5; spec §0). Never renumber.
enum Sys : uint64_t {
    SYS_RUNNER = 1,
    SYS_SUPPRESSION = 2,
    SYS_SPOT_LAUNCH = 3,
    SYS_SPOT_TRANSPORT = 4,
    SYS_SPOT_IGNITE = 5,
};

// FNV-1a 64 over raw bytes — the state-hash primitive (ADR 0008 §3).
inline constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ull;
inline constexpr uint64_t FNV_PRIME = 0x100000001b3ull;
inline uint64_t fnv1a(const void* data, size_t n, uint64_t h = FNV_OFFSET) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}
template <class T>
inline uint64_t fnv1a_array(const T* data, size_t count, uint64_t h = FNV_OFFSET) noexcept {
    // Element-wise little-endian serialisation so the hash is endian-independent.
    for (size_t i = 0; i < count; ++i) {
        uint64_t v = static_cast<uint64_t>(data[i]);
        for (size_t b = 0; b < sizeof(T); ++b) {
            h ^= static_cast<unsigned char>((v >> (8 * b)) & 0xFF);
            h *= FNV_PRIME;
        }
    }
    return h;
}

}  // namespace embersim
