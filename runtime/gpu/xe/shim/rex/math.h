#pragma once
// Shim for the helpers of rex/math.h the vendored shader interpreter uses.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace rex
{
    template <typename T, size_t N> constexpr size_t countof(const T (&)[N]) { return N; }
    inline float clamp_float(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
    inline float saturate(float v) { return clamp_float(v, 0.0f, 1.0f); }
    inline uint16_t byte_swap(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }
    inline uint32_t byte_swap(uint32_t v) { return __builtin_bswap32(v); }
    inline int32_t byte_swap(int32_t v) { return int32_t(__builtin_bswap32(uint32_t(v))); }
    inline uint64_t byte_swap(uint64_t v) { return __builtin_bswap64(v); }
    // Half to float with the Xenos' extended range: exponent 31 is an ordinary
    // exponent, not infinity or NaN.
    inline float xenos_half_to_float(uint16_t half)
    {
        const int sign = half >> 15, exponent = (half >> 10) & 31, mantissa = half & 1023;
        float value = exponent ? std::ldexp(1.0f + mantissa / 1024.0f, exponent - 15)
                               : std::ldexp(float(mantissa), -24);
        return sign ? -value : value;
    }
    inline bool bit_scan_forward(uint32_t v, uint32_t* out)
    {
        if (!v) return false;
        *out = uint32_t(__builtin_ctz(v));
        return true;
    }
}
namespace rex { template <typename T> using le = T; }
