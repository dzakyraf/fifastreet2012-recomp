#include "stream_trace.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <mutex>
#include <sched.h>
#include <string>
#include <thread>
#include <unistd.h>
#include "crash.h"
#include "env.h"
#include "log.h"
#ifdef PES17_ANDROID
#include "android/android.h"
#endif

namespace
{
    std::once_flag g_once;
    std::atomic<bool> g_on{ false };
    std::chrono::steady_clock::time_point g_start;

    double NowMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
    }

    int CurrentCpu()
    {
#ifdef __linux__
        return sched_getcpu();
#else
        return -1;
#endif
    }

    bool ReadSmall(const std::string& path, char* out, size_t size)
    {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return false;
        const size_t got = std::fread(out, 1, size - 1, f);
        std::fclose(f);
        out[got] = 0;
        return got > 0;
    }

    // This kernel has no schedstat, so every 10 ms each thread's state is read
    // from stat: a sample of R is a thread running or waiting for a core. Its
    // cpu ticks (10 ms each) say how much of that was running; the rest of the
    // R samples is time spent runnable and not scheduled. Reported per 500 ms
    // for threads with at least 5 ms of either.
    struct Acc { int rSamples = 0, sSamples = 0, dSamples = 0; int samples = 0; std::string wchan; long ticks0 = -1, ticks1 = 0; int cpu = -1; std::string name; };

    bool ParseStat(const char* buf, char& state, long& ticks, int& cpu)
    {
        const char* p = std::strrchr(buf, ')');
        if (!p || !p[1] || !p[2]) return false;
        state = p[2];
        int field = 3;
        const char* q = p + 2;
        long utime = 0, stime = 0;
        for (; *q && field < 39; q++)
        {
            if (*q != ' ') continue;
            field++;
            if (field == 14) utime = std::atol(q + 1);
            else if (field == 15) stime = std::atol(q + 1);
        }
        cpu = std::atoi(q);
        ticks = utime + stime;
        return true;
    }

    void Sampler()
    {
        std::map<int, Acc> acc;
        double windowStart = NowMs();
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            DIR* dir = opendir("/proc/self/task");
            if (!dir) return;
            while (dirent* entry = readdir(dir))
            {
                if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
                const int tid = std::atoi(entry->d_name);
                const std::string task = std::string("/proc/self/task/") + entry->d_name;
                char buf[768];
                if (!ReadSmall(task + "/stat", buf, sizeof buf)) continue;
                char state; long ticks; int cpu;
                if (!ParseStat(buf, state, ticks, cpu)) continue;
                Acc& a = acc[tid];
                if (a.ticks0 < 0) a.ticks0 = ticks;
                a.ticks1 = ticks;
                a.samples++;
                if (state == 'R') { a.rSamples++; a.cpu = cpu; }
                else if (state == 'D') a.dSamples++;
                else a.sSamples++;
                if (state != 'R' && a.samples % 10 == 0)
                {
                    char w[64] = "";
                    if (ReadSmall(task + "/wchan", w, sizeof w)) a.wchan = w;
                }
                if (a.name.empty())
                {
                    char name[64] = "?";
                    if (ReadSmall(task + "/comm", name, sizeof name))
                        if (char* nl = std::strchr(name, '\n')) *nl = 0;
                    a.name = name;
                }
            }
            closedir(dir);
            const double now = NowMs();
            if (now - windowStart < 500) continue;
            for (auto& [tid, a] : acc)
            {
                const double runMs = (a.ticks1 - a.ticks0) * 10.0;
                const double rMs = a.rSamples * (now - windowStart) / std::max(1, a.samples);
                const double waitMs = std::max(0.0, rMs - runMs);
                if (runMs >= 5 || waitMs >= 5)
                    LOGI("stream: sched t=%.0f tid=%d thr=%s cpu=%d run=%.0f wait=%.0f R=%d S=%d D=%d wchan=%s", now, tid, a.name.c_str(), a.cpu, runMs, waitMs, a.rSamples, a.sSamples, a.dSamples, a.wchan.c_str());
            }
            acc.clear();
            windowStart = now;
        }
    }

    void Init()
    {
        bool on = env::Flag("PES17_STREAM_TRACE");
#ifdef PES17_ANDROID
        if (!on) on = access((android::GetPaths().external + "/stream_trace").c_str(), F_OK) == 0;
#endif
        if (!on) return;
        g_start = std::chrono::steady_clock::now();
        g_on = true;
        LOGW("stream: tracing file reads, texture uploads and thread scheduling");
        std::thread(Sampler).detach();
    }
}

bool stream_trace::On()
{
    std::call_once(g_once, Init);
    return g_on.load(std::memory_order_relaxed);
}

void stream_trace::Read(const char* file, uint32_t bytes, uint32_t got, double freadMs, uint32_t buffer)
{
    if (!On()) return;
    const char* thread = crash::CurrentThreadName();
    LOGI("stream: read t=%.1f thr=%s cpu=%d file=%s bytes=%u got=%u fread=%.2f buf=%08X",
         NowMs(), thread ? thread : "?", CurrentCpu(), file, bytes, got, freadMs, buffer);
}

void stream_trace::Texture(const char* kind, uint32_t address, uint32_t width, uint32_t height, uint32_t format)
{
    if (width < 64 || height < 64 || !On()) return;
    LOGI("stream: tex t=%.1f %s addr=%08X %ux%u fmt=%u", NowMs(), kind, address, width, height, format);
}
