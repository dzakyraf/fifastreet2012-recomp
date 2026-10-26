// See profiler.h.
#include "profiler.h"

#if defined(__linux__)
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <unordered_map>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <vector>
#include "log.h"
#ifdef PES17_ANDROID
#include "android/android.h"
#endif

namespace
{
    constexpr int kSlots = 8;
    constexpr uint32_t kRing = 1u << 15;   // samples a slot keeps: a minute at 500 Hz, then it laps
    constexpr int kFrames = 5;

    struct Sample { uint64_t pc[kFrames]; };

    struct Slot
    {
        std::atomic<int> tid{ 0 };
        char name[64]{};
        std::atomic<uint32_t> written{ 0 };
        Sample* ring = nullptr;
    } g_slots[kSlots];

    std::atomic<int> g_used{ 0 };
    std::atomic<bool> g_started{ false };
    thread_local int t_slot = -1;

    // A word of the thread's own stack, read without the possibility of a fault: a system call
    // that fails with EFAULT where a load would take a signal inside a signal handler.
    bool ReadWord(uint64_t address, uint64_t& value)
    {
        iovec local{ &value, sizeof value }, remote{ reinterpret_cast<void*>(address), sizeof value };
        return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == sizeof value;
    }

    void Handler(int, siginfo_t*, void* context)
    {
        const int saved = errno;
        if (t_slot >= 0)
        {
            Slot& slot = g_slots[t_slot];
            const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__aarch64__)
            Sample sample{};
            sample.pc[0] = uc->uc_mcontext.pc;
            sample.pc[1] = uc->uc_mcontext.regs[30];
            // The frame-pointer chain, where the code keeps one: [fp] = previous fp, [fp + 8] = return.
            uint64_t fp = uc->uc_mcontext.regs[29];
            const uint64_t sp = uc->uc_mcontext.sp;
            for (int i = 2; i < kFrames; i++)
            {
                uint64_t next = 0, ret = 0;
                if (fp < sp || fp - sp > (1u << 20) || (fp & 7) || !ReadWord(fp, next) || !ReadWord(fp + 8, ret)) break;
                sample.pc[i] = ret;
                if (next <= fp) break;
                fp = next;
            }
            const uint32_t at = slot.written.fetch_add(1, std::memory_order_relaxed);
            slot.ring[at % kRing] = sample;
#endif
        }
        errno = saved;
    }

    bool Wanted(const char* name)
    {
        // Read each time: the switch can arrive (files/env_override) after the first thread registers.
        const std::vector<std::string> filters = [] {
            std::vector<std::string> out;
            const char* text = std::getenv("PES17_PROFILE");
            if (!text || !*text) return out;
            std::string all = text;
            size_t from = 0;
            while (from <= all.size())
            {
                const size_t to = all.find(',', from);
                const std::string one = all.substr(from, to == std::string::npos ? std::string::npos : to - from);
                if (!one.empty()) out.push_back(one);
                if (to == std::string::npos) break;
                from = to + 1;
            }
            return out;
        }();
        for (const auto& f : filters)
            if (std::strstr(name, f.c_str())) return true;
        return false;
    }

    // What a dump writes is raw: absolute addresses and the executable part of the process's
    // memory map, which tools/profile_report.py turns into module+offset and then into names.
    // Nothing here asks the dynamic loader anything: it does not have to be safe to call from
    // the sampler's thread at the moment some other thread is inside it.
    void Dump()
    {
        if (std::FILE* maps = std::fopen("/proc/self/maps", "r"))
        {
            char line[512];
            while (std::fgets(line, sizeof line, maps))
                if (std::strstr(line, " r-xp ") || std::strstr(line, " r--p ")) // code, and the headers a base is taken from
                {
                    size_t n = std::strlen(line);
                    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
                    if (std::strchr(line, '/')) LOGI("profile-map %s", line);
                }
            std::fclose(maps);
        }
        const int used = g_used.load();
        for (int s = 0; s < used; s++)
        {
            Slot& slot = g_slots[s];
            const uint32_t n = slot.written.exchange(0);
            const uint32_t have = n < kRing ? n : kRing;
            std::unordered_map<uint64_t, uint32_t> leaf, inclusive;
            for (uint32_t i = 0; i < have; i++)
            {
                const Sample& sample = slot.ring[i];
                leaf[sample.pc[0]]++;
                for (int f = 0; f < kFrames; f++)
                {
                    if (!sample.pc[f]) continue;
                    bool again = false;
                    for (int g = 0; g < f; g++) again |= sample.pc[g] == sample.pc[f];
                    if (!again) inclusive[sample.pc[f]]++;
                }
            }
            LOGI("profile: %s: %u samples", slot.name, have);
            auto top = [&](const char* kind, std::unordered_map<uint64_t, uint32_t>& m) {
                std::vector<std::pair<uint32_t, uint64_t>> rows;
                rows.reserve(m.size());
                for (auto& [pc, c] : m) rows.push_back({ c, pc });
                std::sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
                for (size_t i = 0; i < rows.size() && i < 600; i++)
                    LOGI("profile-%s %s %u %llx", kind, slot.name, rows[i].first, (unsigned long long)rows[i].second);
            };
            top("leaf", leaf);
            top("incl", inclusive);
        }
        LOGI("profile: dumped, counting again");
    }

    void SamplerThread()
    {
        for (int tick = 0;; tick++)
        {
            timespec pause{ 0, 2'000'000 };   // 500 Hz
            nanosleep(&pause, nullptr);
            const int used = g_used.load();
            for (int s = 0; s < used; s++)
                if (const int tid = g_slots[s].tid.load(std::memory_order_relaxed))
                    syscall(SYS_tgkill, getpid(), tid, SIGPROF);
            if (tick % 500 == 0)
            {
#ifdef PES17_ANDROID
                const std::string trigger = android::GetPaths().external + "/profile_dump";
                if (access(trigger.c_str(), F_OK) == 0) { unlink(trigger.c_str()); Dump(); }
#endif
            }
        }
    }
}

void profiler::Note(const char* name)
{
    if (!name || !Wanted(name)) return;
    const int slot = g_used.fetch_add(1);
    if (slot >= kSlots) { g_used.store(kSlots); return; }
    Slot& s = g_slots[slot];
    std::snprintf(s.name, sizeof s.name, "%s", name);
    for (char* c = s.name; *c; c++) if (*c == ' ') *c = '_';
    s.ring = new Sample[kRing]();
    t_slot = slot;
    if (!g_started.exchange(true))
    {
        struct sigaction action{};
        action.sa_sigaction = Handler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        sigaction(SIGPROF, &action, nullptr);
        std::thread(SamplerThread).detach();
        LOGI("profile: sampling started (PES17_PROFILE)");
    }
    s.tid.store(int(syscall(SYS_gettid)), std::memory_order_release);
    LOGI("profile: sampling thread %d (%s)", s.tid.load(), s.name);
}
#else
void profiler::Note(const char*) {}
#endif
