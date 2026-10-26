// Nt*VirtualMemory / Mm* -- guest memory management over the heaps in kernel.cpp.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include <cstdlib>
#include "../guest.h"
#include "../log.h"
#include "../crash.h"
#include "trace.h"
#include <map>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <atomic>
#include <tuple>
#include <vector>
#include <algorithm>

namespace
{
    // Live bytes per API, for `files/report_memory`.
    std::mutex g_accountLock;
    std::map<uint32_t, uint32_t> g_virtualLive, g_physicalLive;
    // Who asked, for the report: the guest return addresses two and three frames
    // up (the first is the engine's allocator wrapper, sub_832D2010 & co).
    struct Caller { uint32_t a = 0, b = 0, c = 0; };
    std::map<uint32_t, Caller> g_callers;
    Caller CallerOf(const PPCContext& ctx)
    {
        // Each frame's back chain is at 0(r1); a function saves its LR at -8
        // from its caller's r1 before the stwu.
        Caller out;
        uint32_t sp = ctx.r1.u32;
        uint32_t* slot[3] = { &out.a, &out.b, &out.c };
        for (int i = 0; i < 3; i++)
        {
            if (sp < 0x10000 || sp >= 0xE0000000u) break;
            sp = *GuestPtr<be32>(sp);
            if (sp < 0x10000 || sp >= 0xE0000000u) break;
            *slot[i] = *GuestPtr<be32>(sp - 8);
        }
        return out;
    }
    void Account(std::map<uint32_t, uint32_t>& live, uint32_t at, uint32_t size, const PPCContext* ctx = nullptr)
    {
        std::lock_guard held(g_accountLock);
        if (size) { live[at] = size; if (ctx) g_callers[at] = CallerOf(*ctx); }
        else { live.erase(at); g_callers.erase(at); }
    }
    // Takes `at` out of `live` if this API handed it out. The bank serves both
    // APIs and reuses a freed start first-fit, so a stale free names whatever
    // took that start since -- a heap segment, once: PES freed a 3600 KB physical
    // block (sub_8386CF60) after the bank had given its start to the process
    // heap, the next physical block landed on the heap's free lists, and the
    // first malloc to walk them looped forever on the Exhibition loading screen.
    // The console's Mm frees only what its own allocator holds; so does this.
    bool Release(std::map<uint32_t, uint32_t>& live, uint32_t at)
    {
        std::lock_guard held(g_accountLock);
        if (!live.erase(at)) return false;
        g_callers.erase(at);
        return true;
    }
    std::atomic<int> g_traceCommits{ 0 };
    constexpr uint32_t X_MEM_COMMIT  = 0x00001000;
    constexpr uint32_t X_MEM_RESERVE = 0x00002000;
    constexpr uint32_t X_MEM_RELEASE = 0x00008000;
}

PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    auto* basePtr = GuestPtr<be32>(ctx.r3.u32);
    auto* sizePtr = GuestPtr<be32>(ctx.r4.u32);
    uint32_t type = ctx.r5.u32;

    if (!basePtr || !sizePtr) { ctx.r3.u64 = X_STATUS_INVALID_PARAMETER; return; }

    uint32_t requested = *sizePtr;
    uint32_t wanted    = *basePtr;
    uint32_t size = (requested + 0xFFF) & ~0xFFFu;
    if (!size) { ctx.r3.u64 = X_STATUS_INVALID_PARAMETER; return; }

    if (size >= (1u << 20))
        LOGI("NtAllocateVirtualMemory: %u KB at %08X, type %08X, protect %08X (from %08X)",
             size >> 10, wanted, type, ctx.r6.u32, uint32_t(ctx.lr));
    // A commit onto an existing reservation just succeeds in place.
    if (wanted && (type & X_MEM_COMMIT) && !(type & X_MEM_RESERVE))
    {
        // Does the range lie inside a block the bank handed out? A commit that
        // does not "succeeds" on memory the bank still counts as free, and a
        // later physical allocation lands on it (Master League setup).
        uint32_t at = 0, extent = 0;
        const bool inside = kernel::FindAllocation(wanted, at, extent) &&
                            uint64_t(wanted) + size <= uint64_t(at) + extent;
        if (!inside)
        {
            static std::atomic<int> told{ 0 };
            if (told++ < 40)
                LOGW("NtAllocateVirtualMemory: COMMIT of %08X +%X is outside any live block (nearest %08X +%X) (from %08X)",
                     wanted, size, at, extent, uint32_t(ctx.lr));
        }
        else if (g_traceCommits.fetch_add(1) < 12)
            LOGI("NtAllocateVirtualMemory: commit %08X +%X inside block %08X +%X (from %08X)",
                 wanted, size, at, extent, uint32_t(ctx.lr));
        *sizePtr = size;
        ctx.r3.u64 = X_STATUS_SUCCESS;
        return;
    }
    if (wanted && (type & X_MEM_RESERVE))
    {
        // We hand out our own address and ignore the one asked for.
        static std::atomic<int> told{ 0 };
        if (told++ < 40)
            LOGW("NtAllocateVirtualMemory: RESERVE asked for %08X +%X, type %08X -- address ignored (from %08X)",
                 wanted, size, type, uint32_t(ctx.lr));
    }

    uint32_t addr = kernel::AllocateGuest(size, 0x1000);
    if (!addr) { LOGW("NtAllocateVirtualMemory: out of guest heap (%u bytes)", size); ctx.r3.u64 = X_STATUS_NO_MEMORY; return; }

    *basePtr = addr;
    *sizePtr = size;
    Account(g_virtualLive, addr, size, &ctx);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    // (BaseAddress*, RegionSize*, FreeType, DebugMemory). A decommit keeps the
    // reservation, which here is the allocation itself.
    auto* basePtr = GuestPtr<be32>(ctx.r3.u32);
    if (!basePtr) { ctx.r3.u64 = X_STATUS_INVALID_PARAMETER; return; }
    auto* sizePtr = GuestPtr<be32>(ctx.r4.u32);
    if (!(ctx.r5.u32 & X_MEM_RELEASE)) { ctx.r3.u64 = X_STATUS_SUCCESS; return; }
    const uint32_t at = *basePtr;
    uint32_t start = 0, size = 0;
    const bool known = kernel::FindAllocation(at, start, size) && start == at;
    if (known && !Release(g_virtualLive, at))
    {
        static std::atomic<int> told{ 0 };
        if (told++ < 20) LOGW("NtFreeVirtualMemory: %08X was not allocated by it -- left alone (from %08X)", at, uint32_t(ctx.lr));
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }
    if (known && kernel::FreeGuest(at))
    {
        if (sizePtr) *sizePtr = size;
        ctx.r3.u64 = X_STATUS_SUCCESS;
        return;
    }
    static std::atomic<int> told{ 0 };
    if (told++ < 10) LOGW("NtFreeVirtualMemory: %08X is not the start of a block (from %08X)", at, uint32_t(ctx.lr));
    ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
}

// X_MEMORY_BASIC_INFORMATION for the block holding the address. The CRT's
// munmap (sub_82595C00) frees a block only when BaseAddress and AllocationBase
// are the block, State is MEM_COMMIT and RegionSize fits; the old stub answered
// "the whole heap" for every address, so every large free failed silently and
// the match load leaked 340 MB of malloc's big chunks out of the bank.
PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    auto* info = GuestPtr<be32>(ctx.r4.u32);
    if (!info) { ctx.r3.u64 = X_STATUS_INVALID_PARAMETER; return; }
    const uint32_t address = ctx.r3.u32, page = address & ~0xFFFu;
    uint32_t at = 0, size = 0;
    if (kernel::FindAllocation(address, at, size))
    {
        info[0] = page;                                   // BaseAddress
        info[1] = at;                                     // AllocationBase
        info[2] = 0x04;                                   // AllocationProtect = PAGE_READWRITE
        info[3] = at + size - page;                       // RegionSize, to the block's end
        info[4] = 0x1000;                                 // State = MEM_COMMIT
        info[5] = 0x04;                                   // Protect
        info[6] = 0x20000;                                // Type = MEM_PRIVATE
    }
    else
    {
        info[0] = page; info[1] = 0; info[2] = 0;
        info[3] = 0x1000;                                 // one free page
        info[4] = 0x10000;                                // State = MEM_FREE
        info[5] = 0x01;                                   // PAGE_NOACCESS
        info[6] = 0;
    }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    uint32_t size      = ctx.r4.u32;
    uint32_t protect   = ctx.r5.u32;
    uint32_t minAddr   = ctx.r6.u32;
    uint32_t maxAddr   = ctx.r7.u32;
    uint32_t alignment = ctx.r8.u32 ? ctx.r8.u32 : 0x1000;
    // The page size is the caller's to choose, as Xenia has it: 64 KB with
    // MEM_LARGE_PAGES, 16 MB with MEM_16MB_PAGES, else 4 KB. MW2's runtime gave
    // every block 64 KB pages (its one 444 MB block needed them, docs/runtime.md),
    // but PES makes thousands of small vertex buffers -- a text mesh is 320 bytes
    // -- and at 64 KB each the 512 MB bank ran out at the team select: the meshes
    // were never made, and every team name drew through a null vertex buffer.
    constexpr uint32_t kLargePages = 0x20000000, k16MbPages = 0x80000000;
    const uint32_t page = (protect & k16MbPages) ? 0x1000000u : (protect & kLargePages) ? 0x10000u : 0x1000u;
    if (alignment < page) alignment = page;
    uint32_t addr = kernel::AllocatePhysical((size + page - 1) & ~(page - 1), alignment);
    if (size >= (1u << 20))
        LOGI("MmAllocatePhysicalMemoryEx: %u KB align %X range %08X..%08X -> %08X (from %08X)",
             size >> 10, alignment, minAddr, maxAddr, addr, uint32_t(ctx.lr));
    if (!addr)
        LOGW("MmAllocatePhysicalMemoryEx: out of physical heap (%u bytes, align %u, range %08X..%08X; %u MB already handed out)",
             size, alignment, minAddr, maxAddr,
             (kernel::PhysicalHighWater() - guest::kPhysicalBase) / (1024 * 1024));
    if (addr) Account(g_physicalLive, addr & 0x1FFFFFFFu, (size + page - 1) & ~(page - 1), &ctx);
    ctx.r3.u64 = addr;
}

PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    // (Type, BaseAddress)
    ctx.r3.u64 = 0;
    if (!Release(g_physicalLive, ctx.r4.u32 & 0x1FFFFFFFu))
    {
        static std::atomic<int> told{ 0 };
        if (told++ < 20)
            LOGW("MmFreePhysicalMemory: %08X is not a live physical block -- left alone (from %08X on '%s')",
                 ctx.r4.u32, uint32_t(ctx.lr), crash::CurrentThreadName());
        return;
    }
    kernel::FreePhysical(ctx.r4.u32);
}

// MmGetPhysicalAddress is in video.cpp, next to the Vd* calls that consume it.
PPC_FUNC(__imp__MmQueryAddressProtect) { ctx.r3.u64 = 0x04; }
PPC_FUNC(__imp__MmSetAddressProtect)   { ctx.r3.u64 = 0; }

// X_MM_STATISTICS (104 bytes, the caller sets Length): page counts, as Xenia
// fills it. XAPI's GlobalMemoryStatus reads TotalPhysicalPages, the title's
// AvailablePages and its virtual-memory totals, and PES sizes from them -- the
// old stub wrote bytes where pages go and left those fields as stack garbage.
PPC_FUNC(__imp__MmQueryStatistics)
{
    auto* s = GuestPtr<be32>(ctx.r3.u32);
    if (!s) { ctx.r3.u64 = X_STATUS_INVALID_PARAMETER; return; }
    if (uint32_t(s[0]) != 104) { ctx.r3.u64 = 0xC0000004; return; }   // STATUS_INFO_LENGTH_MISMATCH
    for (int i = 1; i < 26; i++) s[i] = 0;
    s[1]  = 0x00020000;                                  // TotalPhysicalPages: 512 MB
    s[2]  = 0x00000300;                                  // KernelPages
    s[3]  = uint32_t(kernel::BankFreeBytes() / 4096);    // Title.AvailablePages
    s[4]  = 0x2FFE0000;                                  // Title.TotalVirtualMemoryBytes
    s[5]  = 0x00160000;                                  // Title.ReservedVirtualMemoryBytes
    s[6]  = 0x00001000;                                  // Title.PhysicalPages
    s[7]  = 0x00000010;                                  // Title.PoolPages
    for (int i = 8; i <= 13; i++) s[i] = 0x00000100;     // stack, image, heap, virtual, page table, cache
    s[25] = 0x0001FFFF;                                  // HighestPhysicalPage
    static std::atomic<int> told{ 0 };
    if (told++ < 3) LOGI("MmQueryStatistics: %u MB available to the title (from %08X)", uint32_t(s[3]) >> 8, uint32_t(ctx.lr));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

void kernel::ReportApiMemory()
{
    std::lock_guard held(g_accountLock);
    uint64_t v = 0, p = 0, vFar = 0;
    for (const auto& [at, size] : g_virtualLive) { v += size; if (at < guest::kCachedBase) vFar += size; }
    for (const auto& [at, size] : g_physicalLive) p += size;
    LOGI("memory: NtAllocateVirtualMemory %zu blocks %llu MB (%llu MB of it far), MmAllocatePhysicalMemoryEx %zu blocks %llu MB",
         g_virtualLive.size(), (unsigned long long)(v >> 20), (unsigned long long)(vFar >> 20),
         g_physicalLive.size(), (unsigned long long)(p >> 20));
    // The biggest holders by call chain (physical keys are offsets < 0x20000000,
    // virtual ones are addresses; they cannot collide with a live block of the other).
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, bool>, std::pair<uint64_t, uint32_t>> by;
    for (const auto* live : { &g_virtualLive, &g_physicalLive })
        for (const auto& [at, size] : *live)
        {
            const Caller c = g_callers.count(at) ? g_callers[at] : Caller{};
            auto& e = by[{ c.a, c.b, c.c, live == &g_physicalLive }];
            e.first += size; e.second++;
        }
    std::vector<std::pair<uint64_t, decltype(by)::key_type>> top;
    for (const auto& [k, v] : by) top.push_back({ v.first, k });
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 30; i++)
    {
        const auto& [a, b, c, phys] = top[i].second;
        LOGI("memory:   %6.1f MB in %5u %s blocks from %08X <- %08X <- %08X", top[i].first / 1048576.0,
             by[top[i].second].second, phys ? "physical" : "virtual", a, b, c);
    }
}

std::string kernel::OwnersOf(uint32_t guestAddress)
{
    const uint32_t phys = guestAddress & 0x1FFFFFFFu;
    std::string out;
    char line[160];
    std::lock_guard held(g_accountLock);
    // Virtual blocks are keyed by their cached-window address, physical ones by offset.
    for (const auto& [at, size] : g_virtualLive)
    {
        if (at < guest::kCachedBase || at >= guest::kPhysicalBase) continue;
        const uint32_t start = at & 0x1FFFFFFFu;
        if (phys < start || phys >= start + size) continue;
        const Caller c = g_callers.count(at) ? g_callers[at] : Caller{};
        std::snprintf(line, sizeof line, " [virtual %08X +%X from %08X<-%08X<-%08X]", at, size, c.a, c.b, c.c);
        out += line;
    }
    for (const auto& [at, size] : g_physicalLive)
    {
        if (phys < at || phys >= at + size) continue;
        const Caller c = g_callers.count(at) ? g_callers[at] : Caller{};
        std::snprintf(line, sizeof line, " [physical %08X +%X from %08X<-%08X<-%08X]", at, size, c.a, c.b, c.c);
        out += line;
    }
    return out.empty() ? " [no live block]" : out;
}

bool kernel::CheckProcessHeap(bool quiet)
{
    const auto word = [](uint32_t at) -> uint32_t { return uint32_t(*GuestPtr<be32>(at)); };
    const auto inBank = [](uint32_t p) { return p >= 0x80000000u && p < 0xA0000000u && !(p & 3); };
    const uint32_t heap = word(0x84065CE4);
    if (!inBank(heap)) { LOGW("heapcheck: no process heap pointer yet (%08X)", heap); return true; }
    const uint32_t head = heap + 384;
    uint32_t previous = head, node = word(head), count = 0;
    while (node != head)
    {
        const bool linked = inBank(node) && word(node + 4) == previous;
        if (!linked || ++count > 200000)
        {
            // Each distinct break once.
            static uint32_t lastNode = 0, lastPrevious = 0;
            if (node == lastNode && previous == lastPrevious) return false;
            lastNode = node; lastPrevious = previous;
            LOGW("heapcheck: BROKEN after %u nodes: node %08X (previous %08X, its next %08X) -- %s", count, node, previous,
                 word(previous), count > 200000 ? "loops" : inBank(node) ? "prev link wrong" : "pointer out of range");
            if (inBank(node))
            {
                char dump[160]; int at = 0;
                for (int i = -2; i < 6; i++)
                    at += std::snprintf(dump + at, sizeof dump - at, " %08X", word(node + i * 4));
                LOGW("heapcheck:   words at node-8..node+24:%s", dump);
                LOGW("heapcheck:   owners of node:%s", OwnersOf(node).c_str());
            }
            LOGW("heapcheck:   owners of previous node %08X:%s", previous, OwnersOf(previous).c_str());
            return false;
        }
        previous = node;
        node = word(node);
    }
    if (!quiet) LOGI("heapcheck: process heap %08X free list intact, %u large nodes", heap, count);
    return true;
}
