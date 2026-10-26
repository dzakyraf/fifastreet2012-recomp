#include "debug_watch.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "../guest_memory.h"
#include "../log.h"
#include "../kernel/kernel.h"
#include "../platform.h"

extern "C" volatile int pes_trap_inf_armed = 0;
extern "C" void pes_trap_inf_store(uint32_t address, uint32_t value)
{
    static std::atomic<int> told{ 0 };
    const int n = told++;
    if (n >= 700) { pes_trap_inf_armed = 0; return; }
    char why[96];
    std::snprintf(why, sizeof why, "trap: %cinf stored to guest %08X", (value >> 31) ? '-' : '+', address);
    platform::PrintBacktrace(why);
}

namespace
{
    std::atomic<bool> g_armed{ false };
    std::atomic<uint32_t> g_reports{ 0 };
    std::atomic<bool> g_open{ false };          // the pages are writable, waiting to be re-armed
    uint64_t g_first = 0, g_pages = 0;
    bool g_raw = false;          // g_first is a guest address, not a physical offset
    // With watch_guest's third field: only writes into [lo, hi) are reported; the rest of the
    // page is let through quietly.
    uint64_t g_exactLo = 0, g_exactHi = 0;
    bool g_reads = false;        // watch_guest's fourth field: reads fault too (PROT_NONE)

    // A guest address and, inside the physical bank, the same bytes in the other windows.
    std::vector<uint64_t> Aliases(uint64_t address)
    {
        const uint64_t size = guest::kPhysicalEnd - guest::kPhysicalBase;
        for (uint64_t window : { uint64_t(guest::kCachedBase), uint64_t(guest::kPhysicalBase), uint64_t(guest::kApertureBase) })
            if (address >= window && address < window + size)
            {
                const uint64_t offset = address - window;
                return { guest::kCachedBase + offset, guest::kPhysicalBase + offset, guest::kApertureBase + offset };
            }
        return { address };
    }

    void Protect(bool writable)
    {
        if (g_raw)
        {
            // In host pages, which are 16 KB on Apple Silicon: g_first and g_pages are rounded
            // to them when the watch is armed.
            // The physical bank is mapped at three windows and the title writes through any of
            // them, so an address in one is watched in all.
            for (uint64_t at : Aliases(g_first))
                if (mprotect(guest::Base() + at, g_pages * 4096,
                             writable ? PROT_READ | PROT_WRITE : g_reads ? PROT_NONE : PROT_READ) != 0)
                {
                    static int told = 0;
                    if (told++ < 3) LOGW("debugwatch: mprotect of guest %08llX failed (errno %d)", (unsigned long long)at, errno);
                }
            return;
        }
        for (uint32_t window : { guest::kCachedBase, guest::kPhysicalBase, guest::kApertureBase })
            mprotect(guest::Base() + window + g_first, g_pages * 4096, writable ? PROT_READ | PROT_WRITE : PROT_READ);
    }
}

void debugwatch::ArmFromFile(const std::string& path)
{
    // Polled, so it can be armed once the title is where the question is.
    std::thread([path] {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (g_open.exchange(false) && g_reports.load() < 900) Protect(false);
            static int tick = 0;
            // `touch files/crowd_hunt`: looks (every 20 ms) for a 256 KB run of the black DXT1 pattern
            // 00000000 FFFFFFFF -- the crowd's colour atlas, freshly made -- and arms the watch on it, so the
            // next writes to it are reported with their call stacks. The atlas moves from run to run, so it
            // cannot be named beforehand.
            {
                static bool hunting = false, found = false;
                if (tick % 300 == 150 && !found && guest::Base())
                    if (FILE* h = std::fopen((path.substr(0, path.rfind('/') + 1) + "crowd_hunt").c_str(), "r")) { std::fclose(h); hunting = true; }
                if (hunting && !found && guest::Base() && tick % 20 == 0)
                {
                    static const uint8_t pattern[8] = { 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };
                    const uint8_t* physical = guest::Base() + guest::kPhysicalBase;
                    uint32_t run = 0;
                    for (uint32_t page = 0x10000; page < 0x14000; page++)   // 0x10000000 .. 0x14000000
                    {
                        const uint8_t* at = physical + (uint64_t(page) << 12);
                        const bool match = std::memcmp(at, pattern, 8) == 0 && std::memcmp(at + 8, pattern, 8) == 0 &&
                                           std::memcmp(at + 4088, pattern, 8) == 0;
                        run = match ? run + 1 : 0;
                        if (run == 64)
                        {
                            if (g_armed) Protect(true);
                            g_raw = false; g_first = uint64_t(page - 63) << 12; g_pages = 64; g_reports = 0; g_armed = true;
                            Protect(false);
                            found = true;
                            LOGW("crowd_hunt: the black DXT1 pattern, 64 pages at physical %08llX; watching it", (unsigned long long)g_first);
                            break;
                        }
                    }
                }
            }
            if (++tick % 300) continue;
            if (!guest::Base()) continue;   // a request left from the last run waits for the guest space
            {
                const std::string trap = path.substr(0, path.rfind('/') + 1) + "trap_inf";
                if (FILE* t = std::fopen(trap.c_str(), "r")) { std::fclose(t); std::remove(trap.c_str()); pes_trap_inf_armed = 1; LOGW("trap: armed"); }
            }
            {
                // `touch files/check_heap`: walk the process heap's free list once.
                // `touch files/heap_watch` (kept): walk it on every poll and log the
                // first break, so the moment of corruption lines up with the log.
                const std::string dir = path.substr(0, path.rfind('/') + 1);
                if (FILE* r = std::fopen((dir + "check_heap").c_str(), "r"))
                { std::fclose(r); std::remove((dir + "check_heap").c_str()); kernel::CheckProcessHeap(false); }
                if (FILE* r = std::fopen((dir + "alloc_watch").c_str(), "r")) { std::fclose(r); kernel::ReportAllocRate(); }
                static bool brokenReported = false;
                if (FILE* r = std::fopen((dir + "heap_watch").c_str(), "r"))
                {
                    std::fclose(r);
                    if (!kernel::CheckProcessHeap(true)) { if (brokenReported) {} brokenReported = true; }
                    else if (brokenReported) { brokenReported = false; LOGI("heapcheck: intact again"); }
                }
            }
            {
                // `touch files/report_memory`: the guest heaps' use, now.
                const std::string req = path.substr(0, path.rfind('/') + 1) + "report_memory";
                if (FILE* r = std::fopen(req.c_str(), "r")) { std::fclose(r); std::remove(req.c_str()); kernel::ReportMemory(); }
            }
            {
                // `echo "82235AE0 100" > files/dump_guest`: those guest bytes to files/guest_dump.bin.
                const std::string dir = path.substr(0, path.rfind('/') + 1), req = dir + "dump_guest";
                if (FILE* r = std::fopen(req.c_str(), "r"))
                {
                    unsigned long long a = 0, len = 0x1000;
                    std::fscanf(r, "%llx %llx", &a, &len);
                    std::fclose(r); std::remove(req.c_str());
                    a &= 0xFFFFFFFFull;                       // past 4 GB is unmapped: a bad request killed the game
                    len = std::min(len, 0x100000000ull - a);
                    if (FILE* o = std::fopen((dir + "guest_dump.bin").c_str(), "wb"))
                    {
                        std::fwrite(guest::Base() + (a & 0xFFFFFFFFull), 1, len, o);
                        std::fclose(o);
                        LOGW("dump_guest: %llX +%llX written", a, len);
                    }
                }
            }
            {
                // `echo "20026624 1" > files/watch_guest`: the same, for a guest virtual address.
                const std::string req = path.substr(0, path.rfind('/') + 1) + "watch_guest";
                if (FILE* f = std::fopen(req.c_str(), "r"))
                {
                    // `echo "93B9F400 1 10" > files/watch_guest`: and only report the 0x10 bytes there.
                    // A fourth field of 1 catches reads as well (the page has no access at all).
                    unsigned long long a = 0, n = 1, exact = 0, reads = 0;
                    std::fscanf(f, "%llx %llx %llx %llx", &a, &n, &exact, &reads);
                    g_reads = reads != 0;
                    std::fclose(f); std::remove(req.c_str());
                    if (g_armed) Protect(true);
                    const unsigned long long host = unsigned(getpagesize());
                    const unsigned long long lo = a & ~(host - 1), hi = (a + (n ? n : 1) * 4096 + host - 1) & ~(host - 1);
                    g_exactLo = exact ? a : 0; g_exactHi = exact ? a + exact : 0;
                    g_raw = true; g_first = lo; g_pages = (hi - lo) / 4096; g_reports = 0; g_armed = true;
                    Protect(false);
                    LOGW("debugwatch: guest %08llX, %llu pages read-only", g_first, g_pages);
                    // A store of our own, to show the fault reaches the handler: it should be reported.
                    if (std::getenv("PES17_WATCH_SELFTEST"))
                    {
                        volatile uint8_t* probe = guest::Base() + (g_exactLo ? g_exactLo : g_first);
                        probe[0] = probe[0];
                        LOGW("debugwatch: self-test store done");
                    }
                }
            }
            FILE* file = std::fopen(path.c_str(), "r");
            if (!file) continue;
            unsigned long long address = 0, pages = 1;
            const int n = std::fscanf(file, "%llx %llx", &address, &pages);
            std::fclose(file);
            std::remove(path.c_str());
            if (n < 1 || !pages) continue;
            if (g_armed) Protect(true);
            g_raw = false;
            g_first = address & ~0xFFFull;
            g_pages = pages;
            g_reports = 0;
            g_armed = true;
            Protect(false);
            LOGW("debugwatch: physical %08llX, %llu pages read-only", g_first, g_pages);
        }
    }).detach();
}

bool debugwatch::HandleFault(const void* address)
{
    if (!g_armed.load(std::memory_order_relaxed)) return false;
    const uint8_t* p = static_cast<const uint8_t*>(address);
    bool ours = false;
    uint64_t aliasDelta = 0;    // from the faulting window to the one the watch was given
    if (g_raw)
    {
        for (uint64_t at : Aliases(g_first))
        {
            const uint8_t* lo = guest::Base() + at;
            if (p >= lo && p < lo + g_pages * 4096) { ours = true; aliasDelta = g_first - at; }
        }
    }
    else
    for (uint32_t window : { guest::kCachedBase, guest::kPhysicalBase, guest::kApertureBase })
    {
        const uint8_t* lo = guest::Base() + window + g_first;
        if (p >= lo && p < lo + g_pages * 4096) ours = true;
    }
    if (!ours) return false;
    Protect(true);
    g_open = true;
    if (g_raw && g_exactHi)
    {
        const uint64_t at = uint64_t(p - guest::Base()) + aliasDelta;
        // A write is a store of up to 16 bytes; one that touches the range counts.
        if (at + 16 <= g_exactLo || at >= g_exactHi) return true;
        // At most three reports per address, so one field written every frame cannot use them all up.
        static uint16_t perAddress[4096];
        uint16_t& seen = perAddress[(at - g_exactLo) / 4 % 4096];
        if (seen >= 3) return true;
        seen++;
    }
    const uint32_t n = g_reports++;
    if (n < 900)
    {
        char why[96];
        std::snprintf(why, sizeof why, "debugwatch: write #%u to %p (physical %08llX)", n, address,
                      (unsigned long long)(p - guest::Base()) & 0x1FFFFFFFull);
        platform::PrintBacktrace(why);
    }
    return true;
}
