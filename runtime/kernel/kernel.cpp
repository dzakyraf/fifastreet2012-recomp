#include "kernel.h"
#include "title.h"
#include "../guest.h"
#include "../diagnostics.h"
#include "../log.h"
#include "../crash.h"
#include "../gpu/memory_watch.h"
#include <map>
#include <mutex>
#include <vector>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cstring>

namespace
{
    std::filesystem::path g_gameRoot;

    // A range of guest address space. Freed blocks are kept, merged with
    // their neighbours and handed out again first-fit, and zeroed when they
    // are, as the console's pages come; the rest is carved off the top, which
    // only rises, so what lies above it has never been used and is zero.
    struct Heap
    {
        const char* name;
        const uint32_t base, end;
        std::atomic<uint32_t> top;
        std::mutex lock;
        std::map<uint32_t, uint32_t> live, free;   // address -> size
        uint64_t allocations = 0, reused = 0, liveBytes = 0;

        Heap(const char* heapName, uint32_t base, uint32_t limit)
            : name(heapName), base(base), end(limit), top(base) {}

        // For the allocator's own profile line (see ReportAllocRate).
        std::atomic<uint64_t> scanNs{ 0 }, zeroNs{ 0 }, zeroBytes{ 0 }, waitNs{ 0 }, scanned{ 0 };

        uint32_t Alloc(uint32_t size, uint32_t align)
        {
            if (!size) return 0;
            const auto asked = std::chrono::steady_clock::now();
            std::lock_guard held(lock);
            const auto got = std::chrono::steady_clock::now();
            waitNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(got - asked).count());
            allocations++;
            struct ScanTimer { Heap& h; std::chrono::steady_clock::time_point from; uint64_t n = 0;
                ~ScanTimer() { h.scanNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - from).count()); h.scanned += n; } } scan{ *this, got };
            for (auto it = free.begin(); it != free.end(); ++it)
            {
                scan.n++;
                const uint32_t start = it->first, length = it->second;
                const uint32_t at = (start + align - 1) & ~(align - 1);
                if (uint64_t(at) + size > uint64_t(start) + length) continue;
                free.erase(it);
                if (at > start) free[start] = at - start;
                if (at + size < start + length) free[at + size] = start + length - (at + size);
                // Zeroed as the console's pages come. Slow when the pages are write-watched (each
                // faults), so an allocation that takes long says so: a freeze was thread 11 here.
                const auto zeroFrom = std::chrono::steady_clock::now();
                std::memset(guest::Base() + at, 0, size);
                const auto zeroDone = std::chrono::steady_clock::now();
                zeroNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(zeroDone - zeroFrom).count());
                zeroBytes += size;
                const auto zeroMs = std::chrono::duration_cast<std::chrono::milliseconds>(zeroDone - zeroFrom).count();
                if (zeroMs >= 100)
                    LOGW("memory: zeroing %u KB of %s heap at %08X took %lld ms (on '%s')", size >> 10, name, at,
                         (long long)zeroMs, crash::CurrentThreadName());
                reused++;
                return Took(at, size);
            }
            const uint32_t from = top.load(std::memory_order_relaxed);
            const uint32_t at = (from + align - 1) & ~(align - 1);
            if (uint64_t(at) + size > end) return 0;
            if (at > from) free[from] = at - from;
            top.store(at + size, std::memory_order_release);
            return Took(at, size);
        }

        uint32_t Took(uint32_t at, uint32_t size)
        {
            live[at] = size;
            liveBytes += size;
            return at;
        }

        bool Free(uint32_t address)
        {
            std::lock_guard held(lock);
            auto it = live.find(address);
            if (it == live.end()) return false;
            uint32_t start = address, length = it->second;
            liveBytes -= length;
            live.erase(it);
            if (auto after = free.find(start + length); after != free.end())
            {
                length += after->second;
                free.erase(after);
            }
            if (auto before = free.lower_bound(start); before != free.begin())
                if (--before; before->first + before->second == start)
                {
                    start = before->first;
                    length += before->second;
                    free.erase(before);
                }
            free[start] = length;
            return true;
        }

        uint32_t HighWater() const { return top.load(std::memory_order_acquire); }

        // Whether something live in this arena covers `va`. FromPhysical needs
        // this to tell two arenas apart that answer to the same physical
        // address, and it is asked from the command processor for every packet
        // naming one -- so it does not wait: Alloc zeroes a reused block with
        // the lock held, and blocking the GPU thread behind that would trade one
        // stall for another. A lock it cannot take answers Unknown, and the
        // caller keeps the answer it would have given before.
        enum class Coverage { No, Yes, Unknown };

        Coverage Covers(uint32_t va)
        {
            std::unique_lock held(lock, std::try_to_lock);
            if (!held) return Coverage::Unknown;
            auto it = live.upper_bound(va);
            if (it == live.begin()) return Coverage::No;
            --it;
            return uint64_t(va) < uint64_t(it->first) + it->second ? Coverage::Yes
                                                                  : Coverage::No;
        }

        // The live block holding `address`, if any.
        bool Find(uint32_t address, uint32_t& at, uint32_t& size)
        {
            std::lock_guard held(lock);
            auto it = live.upper_bound(address);
            if (it == live.begin()) return false;
            --it;
            if (uint64_t(address) >= uint64_t(it->first) + it->second) return false;
            at = it->first; size = it->second;
            return true;
        }

        uint64_t FreeBytes()
        {
            std::lock_guard held(lock);
            return uint64_t(end - base) - liveBytes;
        }

        void Report()
        {
            std::lock_guard held(lock);
            uint64_t freeBytes = 0;
            for (const auto& [at, size] : free) freeBytes += size;
            LOGI("memory: %s heap %llu MB in use, %llu MB free below the high water of %08X;"
                 " %llu allocations, %llu of them into freed space",
                 name, (unsigned long long)(liveBytes >> 20), (unsigned long long)(freeBytes >> 20),
                 HighWater(), (unsigned long long)allocations, (unsigned long long)reused);
            // Live blocks by size: what the space went to.
            constexpr uint32_t kBounds[] = { 0x1000, 0x10000, 0x100000, 0x400000, 0x1000000, 0xFFFFFFFF };
            uint64_t count[6] = {}, bytes[6] = {};
            for (const auto& [at, size] : live)
                for (int b = 0; b < 6; b++)
                    if (size <= kBounds[b]) { count[b]++; bytes[b] += size; break; }
            LOGI("memory:   %s live blocks: <=4K %llu (%llu MB), <=64K %llu (%llu MB), <=1M %llu (%llu MB),"
                 " <=4M %llu (%llu MB), <=16M %llu (%llu MB), larger %llu (%llu MB)", name,
                 (unsigned long long)count[0], (unsigned long long)(bytes[0] >> 20),
                 (unsigned long long)count[1], (unsigned long long)(bytes[1] >> 20),
                 (unsigned long long)count[2], (unsigned long long)(bytes[2] >> 20),
                 (unsigned long long)count[3], (unsigned long long)(bytes[3] >> 20),
                 (unsigned long long)count[4], (unsigned long long)(bytes[4] >> 20),
                 (unsigned long long)count[5], (unsigned long long)(bytes[5] >> 20));
        }
    };

    // The bank, addressed through its physical window. Both the title's virtual
    // allocations (handed back in the cached window) and its physical ones come
    // from here, so no two of them ever share a physical offset -- the property
    // that lets the GPU and the CPU name the same bytes by either window.
    constexpr uint32_t kWindowDelta = guest::kPhysicalBase - guest::kCachedBase;
    Heap g_bankLow{ "bank below the image", guest::kPhysicalBase, guest::kPhysicalBase + guest::kImagePhysical };
    Heap g_bank{ "bank", guest::kHeapBase + kWindowDelta, guest::kPhysicalEnd };

    uint32_t BankAlloc(uint32_t size, uint32_t alignment)
    {
        if (const uint32_t at = g_bankLow.Alloc(size, alignment)) return at;
        return g_bank.Alloc(size, alignment);
    }
    bool BankFree(uint32_t physicalWindowAddress)
    {
        return g_bankLow.Free(physicalWindowAddress) || g_bank.Free(physicalWindowAddress);
    }
    Heap g_far{ "far", guest::kFarBase, guest::kFarEnd };

    // How often FromPhysical resolved an address to the general heap that the
    // high water mark alone would have sent to the physical arena. Every one of
    // these was a word the command processor and the title disagreed about.
    diag::Stat g_physicalCorrected;

    struct TlsTemplate { uint32_t address = 0, size = 0; };
    TlsTemplate g_tlsTemplate;

    std::mutex g_unimplLock;
    std::map<std::string, uint64_t> g_unimpl;
}

namespace
{
    // IMAGE_TLS_DIRECTORY32, big-endian, located through the PE data directory.
    void ReadTlsDirectory()
    {
        const uint8_t* image = guest::Base() + PPC_IMAGE_BASE;
        uint32_t peOffset;
        std::memcpy(&peOffset, image + 0x3C, 4);          // e_lfanew, little-endian
        if (peOffset + 0x100 > PPC_IMAGE_SIZE) return;

        uint16_t optionalMagic;
        std::memcpy(&optionalMagic, image + peOffset + 24, 2);
        const uint32_t directories = peOffset + 24 + (optionalMagic == 0x10B ? 96 : 112);

        uint32_t tlsRva, tlsSize;
        std::memcpy(&tlsRva, image + directories + 9 * 8, 4);
        std::memcpy(&tlsSize, image + directories + 9 * 8 + 4, 4);
        if (!tlsRva || tlsSize < 24) return;

        // Guest-endian: raw data start/end, then index.
        const be32* entry = reinterpret_cast<const be32*>(image + tlsRva);
        uint32_t rawStart = entry[0], rawEnd = entry[1], zeroFill = entry[4];
        if (rawEnd <= rawStart) return;

        g_tlsTemplate.address = rawStart;
        g_tlsTemplate.size = (rawEnd - rawStart) + zeroFill;
    }
}

const TlsTemplate& ImageTlsTemplate() { return g_tlsTemplate; }

void kernel::Initialise(const std::filesystem::path& gameRoot)
{
    // The modules resolve their own imports as they are mapped (modules.cpp).
    ReadTlsDirectory();
    g_gameRoot = gameRoot;
    LOGI("game root: %s%s", gameRoot.string().c_str(),
         std::filesystem::exists(gameRoot) ? "" : "  (does not exist)");
}

const std::filesystem::path& kernel::GameRoot() { return g_gameRoot; }

void kernel::ReportAllocRate()
{
    static uint64_t lastAllocs = 0, lastScan = 0, lastZero = 0, lastBytes = 0, lastWait = 0, lastScanned = 0, lastFaults = 0;
    static auto lastAt = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - lastAt).count();
    if (seconds < 2.0) return;
    lastAt = now;
    Heap* heaps[2] = { &g_bankLow, &g_bank };
    uint64_t allocs = 0, scan = 0, zero = 0, bytes = 0, wait = 0, scanned = 0, freeCount = 0;
    for (Heap* h : heaps)
    {
        allocs += h->allocations; scan += h->scanNs; zero += h->zeroNs; bytes += h->zeroBytes;
        wait += h->waitNs; scanned += h->scanned;
        std::lock_guard held(h->lock);
        freeCount += h->free.size();
    }
    const uint64_t faults = gpu::watch::FaultCount();
    LOGI("alloc: %.0f allocs/s, scan %.0f ms (%.0f entries each), zeroing %.0f ms for %.1f MB, lock waits %.0f ms, "
         "%llu free blocks, %llu write faults/s",
         (allocs - lastAllocs) / seconds, (scan - lastScan) / 1e6,
         allocs > lastAllocs ? double(scanned - lastScanned) / double(allocs - lastAllocs) : 0.0,
         (zero - lastZero) / 1e6, (bytes - lastBytes) / 1048576.0, (wait - lastWait) / 1e6,
         (unsigned long long)freeCount, (unsigned long long)((faults - lastFaults) / seconds));
    lastAllocs = allocs; lastScan = scan; lastZero = zero; lastBytes = bytes; lastWait = wait;
    lastScanned = scanned; lastFaults = faults;
}

uint32_t kernel::AllocateGuest(uint32_t size, uint32_t alignment)
{
    if (const uint32_t at = BankAlloc(size, alignment)) return at - kWindowDelta;
    return g_far.Alloc(size, alignment);   // too big for what is left of the bank
}
uint32_t kernel::AllocatePhysical(uint32_t size, uint32_t alignment) { return BankAlloc(size, alignment); }
bool kernel::FreeGuest(uint32_t address)
{
    if (address >= guest::kCachedBase && address < guest::kPhysicalBase) return BankFree(address + kWindowDelta);
    return g_far.Free(address);
}
bool kernel::FreePhysical(uint32_t address)
{
    // Through whichever window it was named.
    return BankFree(guest::kPhysicalBase + (address & 0x1FFFFFFFu));
}

bool kernel::FindAllocation(uint32_t address, uint32_t& at, uint32_t& size)
{
    if (address >= guest::kCachedBase && address < guest::kPhysicalBase)
    {
        if (!g_bankLow.Find(address + kWindowDelta, at, size) && !g_bank.Find(address + kWindowDelta, at, size))
            return false;
        at -= kWindowDelta;
        return true;
    }
    if (address >= guest::kPhysicalBase && address < guest::kPhysicalEnd)
        return g_bankLow.Find(address, at, size) || g_bank.Find(address, at, size);
    return g_far.Find(address, at, size);
}

uint64_t kernel::BankFreeBytes() { return g_bankLow.FreeBytes() + g_bank.FreeBytes(); }

void kernel::ReportMemory()
{
    ReportApiMemory();
    g_bankLow.Report();
    g_bank.Report();
    g_far.Report();
    if (g_physicalCorrected)
        LOGI("memory: %llu physical addresses resolved to the virtual heap that the high"
             " water mark alone would have sent to the physical arena",
             (unsigned long long)g_physicalCorrected.load());
}

namespace
{
    // X_TIME_STAMP_BUNDLE: interrupt time, system time, millisecond tick count.
    // GetTickCount loads the pointer and reads the third field, so an unresolved
    // pointer freezes the title's clock and every timeout built on it.
    struct TimeStampBundleBlock
    {
        be64 interruptTime;
        be64 systemTime;
        be32 tickCount;
        be32 padding;
    };

    uint32_t g_timeStampBundle = 0;
    std::chrono::steady_clock::time_point g_bootTime;

}

uint32_t kernel::TimeStampBundle()
{
    static std::once_flag once;
    std::call_once(once, [] {
        g_timeStampBundle = AllocateGuest(sizeof(TimeStampBundleBlock), 16);
        if (!g_timeStampBundle) { LOGE("could not allocate the time stamp bundle"); return; }
        std::memset(guest::Base() + g_timeStampBundle, 0, sizeof(TimeStampBundleBlock));
        g_bootTime = std::chrono::steady_clock::now();
        UpdateTimeStampBundle();
    });
    return g_timeStampBundle;
}

// KeDebugMonitorData: a pointer to the debug monitor's dispatch table, null on
// a retail console. XAudio2's mixer reads it on every pass and, when the word
// it points at is not zero, calls a method through it -- so left as an import
// record it points into the low guest space, and whatever the title happens
// to have written there (handles handed back as object pointers live there)
// becomes a call target.
uint32_t kernel::DebugMonitorData()
{
    static uint32_t word = 0;
    static std::once_flag once;
    std::call_once(once, [] {
        word = AllocateGuest(16, 16);
        if (word) *GuestPtr<be32>(word) = 0;
    });
    return word;
}

void kernel::UpdateTimeStampBundle()
{
    if (!g_timeStampBundle) return;
    auto* bundle = GuestPtr<TimeStampBundleBlock>(g_timeStampBundle);
    const auto elapsed = std::chrono::steady_clock::now() - g_bootTime;
    const uint64_t hundredNanoseconds =
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100);
    bundle->interruptTime = hundredNanoseconds;
    bundle->systemTime = SystemTime100ns();
    bundle->tickCount = uint32_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

uint32_t kernel::PhysicalHighWater() { return g_bank.HighWater(); }

uint32_t kernel::ToPhysical(uint32_t va)
{
    // Exactly the sequence the title inlines around its Vd* calls:
    //   phys = (va & 0x1FFFFFFF) + (((va >> 20) + 0x200) & 0x1000)
    return (va & 0x1FFFFFFFu) + ((((va >> 20) + 0x200u) & 0x1000u));
}

uint32_t kernel::FromPhysical(uint32_t phys)
{
    // Every window maps the same bank, so the physical window names these bytes
    // as well as any other. (MW2's runtime kept the heap as separate memory and
    // had to guess the window from what each arena had live -- wrong as soon as
    // both held something at the same offset, which PES's layout made common.)
    return guest::kPhysicalBase + (phys & 0x1FFFFFFFu);
}

uint32_t kernel::CreateThreadBlock(uint32_t threadId)
{
    // [block .. block+0x8000) is the KPCR, with r13 pointing 0x4000 in so the
    // negative r13-relative offsets the game uses stay inside it. The KTHREAD
    // follows, then this thread's copy of the image's TLS block.
    constexpr uint32_t kPcrSize     = 0x8000;
    constexpr uint32_t kPcrBias     = 0x4000;
    constexpr uint32_t kThreadSize  = 0x1000;
    constexpr uint32_t kTlsSize     = 0x200;
    constexpr uint32_t kTlsPointerOffset    = 0;     // KPCR.TlsData
    constexpr uint32_t kCurrentThreadOffset = 256;   // KPCR.CurrentThread
    constexpr uint32_t kThreadIdOffset      = 332;   // KTHREAD.ThreadId

    const uint32_t total = kPcrSize + kThreadSize + kTlsSize;
    uint32_t block = AllocateGuest(total, 0x1000);
    if (!block) return 0;
    std::memset(guest::Base() + block, 0, total);

    uint32_t pcr     = block + kPcrBias;
    uint32_t kthread = block + kPcrSize;
    uint32_t tls     = block + kPcrSize + kThreadSize;

    // The CRT hangs its per-thread scratch allocator off a slot in the TLS
    // template, reached as [[r13] + slot], so a null TlsData sends every thread to
    // the same imaginary pool at guest address 0 -- where they corrupt each other.
    const auto& tlsTemplate = ImageTlsTemplate();
    if (tlsTemplate.address && tlsTemplate.size && tlsTemplate.size <= kTlsSize)
        std::memcpy(guest::Base() + tls, guest::Base() + tlsTemplate.address, tlsTemplate.size);

    *GuestPtr<be32>(pcr + kTlsPointerOffset)    = tls;
    *GuestPtr<be32>(pcr + kCurrentThreadOffset) = kthread;
    *GuestPtr<be32>(kthread + kThreadIdOffset)  = threadId;
    return pcr;
}

void kernel::Unimplemented(const char* name)
{
    std::lock_guard g(g_unimplLock);
    auto [it, inserted] = g_unimpl.try_emplace(name, 0);
    it->second++;
    if (inserted) LOGW("unimplemented: %s", name);
    // And at every power of ten after that: a stub called thousands of times is
    // a title polling for something it will never be given.
    else if (it->second == 10 || it->second == 100 || it->second == 1000 || it->second % 10000 == 0)
        LOGW("unimplemented: %s called %llu times", name, (unsigned long long)it->second);
}

void kernel::ReportUnimplemented()
{
    if constexpr (!diag::kOn) return;
    std::lock_guard g(g_unimplLock);
    if (g_unimpl.empty()) { LOGI("no unimplemented imports were reached"); return; }
    std::vector<std::pair<std::string, uint64_t>> v(g_unimpl.begin(), g_unimpl.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.second > b.second; });
    LOGI("unimplemented imports actually reached (%zu distinct):", v.size());
    for (auto& [name, count] : v) LOGI("  %8llu  %s", (unsigned long long)count, name.c_str());
}

PPCFunc* kernel::GuestFunction(uint32_t address)
{
    if (address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE) return nullptr;
    return *(PPCFunc**)(guest::Base() + guest::FunctionSlotOffset(address, uint32_t(PPC_CODE_BASE)));
}

uint32_t kernel::CallGuest(const PPCContext& caller, uint32_t address,
                           uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    PPCFunc* fn = GuestFunction(address);
    if (!fn) { LOGW("CallGuest: no recompiled function at %08X", address); return 0; }

    PPCContext ctx{};
    std::memset(&ctx, 0, sizeof(ctx));
    // Leave the caller's frame and its 64-byte red zone untouched.
    ctx.r1.u32  = (caller.r1.u32 - 0x400) & ~0xFu;
    ctx.r13.u32 = caller.r13.u32;
    ctx.r3.u32  = a0;
    ctx.r4.u32  = a1;
    ctx.r5.u32  = a2;
    ctx.r6.u32  = a3;
    ctx.r7.u32  = a4;
    // The caller's code keeps its own idea of the float mode (whether
    // denormals flush: on for vector code, off for scalar) and only switches
    // when it thinks the mode differs. The console keeps the two in separate
    // registers, FPSCR and VSCR; x86 has one, MXCSR, and the recompiled code
    // tracks which it last set. A routine run here in a context of its own can
    // leave MXCSR switched -- vector code turns flushing on -- and the caller,
    // believing it off, would flush every tiny value its scalar code makes from
    // then on. So the caller gets the register back as it left it.
    const uint32_t before = ctx.fpscr.getcsr();
    ctx.fpscr.loadFromHost();
    fn(ctx, guest::Base());
    if (ctx.fpscr.getcsr() != before) ctx.fpscr.setcsr(before);
    return ctx.r3.u32;
}

namespace
{
    struct PendingApc
    {
        uint32_t routine, context, ioStatusBlock;
        std::function<void()> work;
    };
    thread_local std::vector<PendingApc> t_apcs;
}

void kernel::QueueUserApc(uint32_t routine, uint32_t context, uint32_t ioStatusBlock,
                          std::function<void()> work)
{
    if (!routine) { if (work) work(); return; }
    t_apcs.push_back({ routine, context, ioStatusBlock, std::move(work) });
}

bool kernel::DeliverUserApcs(const PPCContext& caller)
{
    const bool queued = DeliverThreadApcs(caller);
    if (t_apcs.empty()) return queued;
    // An APC may queue more work, so drain rather than iterate.
    std::vector<PendingApc> batch;
    batch.swap(t_apcs);
    for (const auto& a : batch)
    {
        if (a.work) a.work();
        CallGuest(caller, a.routine, a.context, a.ioStatusBlock, 0);
    }
    return true;
}

#include "../crash.h"
#include "../gpu/memory_watch.h"
#include "trace.h"
std::atomic<bool> kernel::g_traceAll{ false };
void kernel::TraceReturn(const char* name, const PPCContext& ctx)
{
    // Only the calls that decide something; the polling ones would bury them.
    static const char* const kQuiet[] = { "NtWaitFor", "XNotifyGetNext", "KeGetCurrentProcessType", "KeSetEvent", "NtReleaseSemaphore",
                                          "KeWaitFor", "XamInput", "XNetGetTitleXnAddr", "XAudio", "XamUserGetSigninState", "Vd",
                                          "KeQuery", "NtClearEvent", "NtSetEvent", "KeDelay", "Xe", "Rtl", "Nt", "Mm", "Ke", "Ex", "Ob" };
    const char* n = name + (std::strncmp(name, "__imp__", 7) == 0 ? 7 : 0);
    for (const char* quiet : kQuiet) if (std::strncmp(n, quiet, std::strlen(quiet)) == 0) return;
    LOGK("  -> %s returns %08X on '%s'", n, ctx.r3.u32, crash::CurrentThreadName());
}
void kernel::TraceImport(const char* name, uint32_t count, const PPCContext& ctx)
{
    LOGK("import: %s #%u (r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X) lr %08X on '%s'",
         name + (std::strncmp(name, "__imp__", 7) == 0 ? 7 : 0), count, ctx.r3.u32, ctx.r4.u32,
         ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, uint32_t(ctx.lr), crash::CurrentThreadName());
}
