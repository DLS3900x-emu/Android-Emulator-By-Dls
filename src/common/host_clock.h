// Monotonic host clock in nanoseconds (QueryPerformanceCounter based).
#pragma once
#include <windows.h>

#include <cstdint>

namespace Aetheron {

inline uint64_t HostNowNs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // Split to avoid 64-bit overflow on long uptimes.
    uint64_t q = uint64_t(c.QuadPart) / uint64_t(freq.QuadPart);
    uint64_t r = uint64_t(c.QuadPart) % uint64_t(freq.QuadPart);
    return q * 1000000000ULL + r * 1000000000ULL / uint64_t(freq.QuadPart);
}

}  // namespace Aetheron
