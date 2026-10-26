#pragma once
// What the game does not need to run: traces, dumps, statistics, the end-of-run
// reports, the stutter detector and the headless test harness.
//
// -DPES17_DIAGNOSTICS=0 (RELEASE=1 ./build.sh, which sets that) builds without it;
// the value is a number because kOn below compares it, not a word like OFF,
// which the preprocessor would leave as an identifier and fail on. The code is
// still compiled in that build, behind a constant false, so it cannot rot
// unnoticed; the optimiser removes it. Only what must not exist at all -- a hook
// on a guest function, a whole file of dumps -- is behind #if PES17_DIAGNOSTICS.
#include <cstdint>
#include <type_traits>
#include "counter.h"
#include "env.h"

#ifndef PES17_DIAGNOSTICS
#   define PES17_DIAGNOSTICS 1
#endif

namespace diag
{
    inline constexpr bool kOn = PES17_DIAGNOSTICS != 0;

    // The PES17_* debug switches, read as env:: reads them; unset in a release build.
    inline bool Flag(const char* name)
    {
        if constexpr (kOn) return env::Flag(name);
        else return false;
    }
    inline const char* Text(const char* name)
    {
        if constexpr (kOn) return env::Text(name);
        else return nullptr;
    }
    inline uint64_t Number(const char* name, uint64_t fallback = 0, int base = 10)
    {
        if constexpr (kOn) return env::Number(name, fallback, base);
        else return fallback;
    }
    inline double Real(const char* name, double fallback = 0.0)
    {
        if constexpr (kOn) return env::Real(name, fallback);
        else return fallback;
    }

    // A statistic for the reports: a Counter, or nothing in a release build.
    // Nothing the game does may read one.
    struct NoCounter
    {
        void operator++(int) {}
        void operator++() {}
        void operator+=(uint64_t) {}
        void Add(uint64_t) {}
        void Reset() {}
        uint64_t load() const { return 0; }
        operator uint64_t() const { return 0; }
    };
    using Stat = std::conditional_t<kOn, Counter, NoCounter>;
}
