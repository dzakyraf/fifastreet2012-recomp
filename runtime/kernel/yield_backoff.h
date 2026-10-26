#pragma once
// A guest that waits by polling -- NtYieldExecution in a loop, or a zero
// KeDelayExecutionThread -- turns sched_yield into a busy loop. On the phone that
// was measured (simpleperf, in a match): the main thread and three worker threads
// spent a quarter of all CPU cycles in the kernel yielding, a core each, which
// is heat the thermal governor answers by clocking the whole chip down.
//
// The first few yields of a burst stay plain yields, so a wait that ends at once
// costs nothing extra. A thread that keeps coming back within a fraction of a
// millisecond is spinning, and gets a sleep of a few microseconds that grows to
// PES17_YIELD_SLEEP_US (default 0 = off: measured, 100 us cut CPU cycles by a fifth but cost about 6 fps, and 30 us after 20000 yields changed nothing).
#include "../env.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <sched.h>

namespace kernel
{
    inline void YieldBackoff()
    {
        using Clock = std::chrono::steady_clock;
        thread_local uint32_t spins = 0;
        thread_local Clock::time_point last{};
        struct Tuning { uint32_t maxSleepUs; uint32_t freeYields; };
        static const Tuning tuning = [] {
            Tuning t{ uint32_t(env::Number("PES17_YIELD_SLEEP_US", 0)), uint32_t(env::Number("PES17_YIELD_SPINS", 32)) };
#ifdef PES17_ANDROID
            // files/yield_tuning "<sleep us> <free yields>" overrides, for measuring on the phone.
            if (std::FILE* f = std::fopen("/sdcard/Android/data/com.fifastreet.recomp/files/yield_tuning", "r"))
            {
                unsigned a = 0, b = 0;
                if (std::fscanf(f, "%u %u", &a, &b) == 2) { t.maxSleepUs = a; t.freeYields = b; }
                std::fclose(f);
            }
#endif
            return t;
        }();
        const uint32_t maxSleepUs = tuning.maxSleepUs;
        const uint32_t kFreeYields = tuning.freeYields;

        if (maxSleepUs == 0) { sched_yield(); return; }
        const auto now = Clock::now();
        // Anything but a tight loop in between: it was not a spin.
        if (now - last > std::chrono::microseconds(500)) spins = 0;
        else if (spins < 0xFFFFFFF0u) ++spins;

        if (spins < kFreeYields)
        {
            sched_yield();
        }
        else
        {
            const uint32_t us = std::min(maxSleepUs, 10u * (spins - kFreeYields + 1));
            timespec wait{ 0, long(us) * 1000L };
            nanosleep(&wait, nullptr);
        }
        last = Clock::now();
    }
}
