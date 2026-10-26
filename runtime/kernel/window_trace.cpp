#include "window_trace.h"
#include "../log.h"
#ifdef PES17_ANDROID
#include "../android/android.h"
#endif
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

namespace
{
    const auto g_start = std::chrono::steady_clock::now();
    std::atomic<uint32_t> g_lines{ 0 };
    std::atomic<int64_t> g_opensUs{ -1 }, g_closesUs{ -1 }, g_checkedUs{ 0 };

    int64_t NowUs()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start).count();
    }
}

// The window opens when a file named wt_now appears in the app's folder
// (`adb shell touch .../files/wt_now`): the file is removed, and the next three
// seconds are traced. Polled at most five times a second.
bool kernel::WindowTraceOn()
{
    const int64_t now = NowUs();
    const int64_t opens = g_opensUs.load(std::memory_order_relaxed);
    if (opens >= 0)
        return now >= opens && now < g_closesUs.load(std::memory_order_relaxed) &&
               g_lines.load(std::memory_order_relaxed) < 30000;
#ifdef PES17_ANDROID
    int64_t checked = g_checkedUs.load(std::memory_order_relaxed);
    if (now - checked > 200'000 && g_checkedUs.compare_exchange_strong(checked, now))
    {
        const std::string trigger = android::GetPaths().external + "/wt_now";
        if (access(trigger.c_str(), F_OK) == 0)
        {
            unlink(trigger.c_str());
            int64_t none = -1;
            if (g_opensUs.compare_exchange_strong(none, now))
            {
                g_closesUs.store(now + 8'000'000);
                LOGW("WT window opens: the next 8 seconds are traced");
            }
        }
    }
#endif
    return false;
}

void kernel::WindowTrace(const char* kind, const char* thread, uint32_t a, uint32_t b, const char* note)
{
    // The busy pollers drown everything else out; they are known and uninteresting.
    if (thread)
        for (const char* quiet : { "main thread", "guest thread 47", "guest thread 50", "guest thread 34 ",
                                   "guest thread 35 ", "guest thread 36 ", "guest thread 30 ", "audio worker",
                                   "guest thread 45 ", "guest thread 33 ", "guest thread 48 ", "guest thread 32 " })
            if (std::strstr(thread, quiet)) return;
    g_lines++;
    LOGW("WT %lld %s %s %08X %08X %s", (long long)NowUs(), kind, thread ? thread : "?", a, b, note ? note : "");
}
