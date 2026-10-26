// Asynchronous procedure calls queued to another thread.
//
// The title keeps worker threads that sleep alertably for ever (SleepEx(INFINITE)) and are handed
// work by queueing an APC to them: NtQueueApcThread, or KeInitializeApc + KeInsertQueueApc. The
// calling thread's own I/O completions go through kernel::QueueUserApc (kernel.cpp); these are the
// ones other threads send. An APC runs on its target the next time that thread enters an alertable
// wait (KeDelayExecutionThread, NtWaitFor*Ex), as NT delivers user-mode APCs.
//
// A thread is named by its KTHREAD, the guest address its r13's KPCR points at (+256), which is
// also what ObReferenceObjectByHandle gives the title for a thread handle.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "objects.h"
#include "../guest.h"
#include "../log.h"
#include "../crash.h"

#include <deque>
#include <map>
#include <mutex>

using namespace kernel;

namespace
{
    constexpr uint32_t kCurrentThreadOffset = 256;   // KPCR.CurrentThread, as threads.cpp and kernel.cpp lay it out

    struct Pending
    {
        uint32_t routine = 0;                 // NtQueueApcThread: the routine, called with the three arguments
        uint32_t arguments[3]{};
        uint32_t apc = 0;                     // KeInsertQueueApc: the KAPC's address, which names the record below
    };

    // What KeInitializeApc was told, kept beside the title's KAPC (whose layout nothing here reads).
    struct ApcRecord
    {
        uint32_t thread = 0, kernelRoutine = 0, rundownRoutine = 0, normalRoutine = 0, normalContext = 0;
        uint32_t scratch = 0;                 // five guest words for the kernel routine's by-reference arguments
        bool inserted = false;
    };

    std::mutex g_lock;
    std::map<uint32_t, std::deque<Pending>> g_queues;     // KTHREAD -> what is waiting for it
    std::map<uint32_t, ApcRecord> g_records;              // KAPC -> its record

    uint32_t ThreadOf(const PPCContext& ctx) { return ctx.r13.u32 ? uint32_t(*GuestPtr<be32>(ctx.r13.u32 + kCurrentThreadOffset)) : 0; }

    // A thread handle (or the current-thread pseudo-handle) as a KTHREAD.
    uint32_t ThreadFromHandle(const PPCContext& ctx, uint32_t handle)
    {
        if (handle == kCurrentThreadHandle || handle == kCurrentProcessHandle) return ThreadOf(ctx);
        const auto object = LookupHandle(handle);
        return object ? object->guestHeader : 0;
    }
}

bool kernel::HasThreadApcs(const PPCContext& caller)
{
    const uint32_t thread = ThreadOf(caller);
    if (!thread) return false;
    std::lock_guard held(g_lock);
    const auto it = g_queues.find(thread);
    return it != g_queues.end() && !it->second.empty();
}

bool kernel::DeliverThreadApcs(const PPCContext& caller)
{
    const uint32_t thread = ThreadOf(caller);
    if (!thread) return false;
    bool ran = false;
    for (;;)
    {
        Pending next;
        {
            std::lock_guard held(g_lock);
            auto it = g_queues.find(thread);
            if (it == g_queues.end() || it->second.empty()) break;
            next = it->second.front();
            it->second.pop_front();
        }
        ran = true;
        if (!next.apc)
        {
            CallGuest(caller, next.routine, next.arguments[0], next.arguments[1], next.arguments[2]);
            continue;
        }
        // KeInsertQueueApc: the kernel routine first, with the normal routine and its three
        // arguments by reference so it can change them (or clear the normal routine); then the
        // normal routine, if one is left.
        ApcRecord record;
        {
            std::lock_guard held(g_lock);
            const auto found = g_records.find(next.apc);
            if (found == g_records.end()) continue;
            record = found->second;
            found->second.inserted = false;
        }
        const uint32_t slots = record.scratch;
        if (!slots) continue;
        *GuestPtr<be32>(slots + 0) = record.normalRoutine;
        *GuestPtr<be32>(slots + 4) = record.normalContext;
        *GuestPtr<be32>(slots + 8) = next.arguments[0];
        *GuestPtr<be32>(slots + 12) = next.arguments[1];
        if (record.kernelRoutine)
            CallGuest(caller, record.kernelRoutine, next.apc, slots + 0, slots + 4, slots + 8, slots + 12);
        const uint32_t normal = *GuestPtr<be32>(slots + 0);
        if (normal)
            CallGuest(caller, normal, *GuestPtr<be32>(slots + 4), *GuestPtr<be32>(slots + 8), *GuestPtr<be32>(slots + 12));
    }
    return ran;
}

// (thread, routine, argument1, argument2, argument3): the routine runs on that thread as
// routine(argument1, argument2, argument3).
PPC_FUNC(__imp__NtQueueApcThread)
{
    const uint32_t thread = ThreadFromHandle(ctx, ctx.r3.u32);
    if (!thread || !ctx.r4.u32) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    Pending pending;
    pending.routine = ctx.r4.u32;
    pending.arguments[0] = ctx.r5.u32;
    pending.arguments[1] = ctx.r6.u32;
    pending.arguments[2] = ctx.r7.u32;
    {
        std::lock_guard held(g_lock);
        g_queues[thread].push_back(pending);
    }
    static std::atomic<int> told{ 0 };
    if (told++ < 4) LOGI("apc: NtQueueApcThread -> thread %08X routine %08X (from '%s')", thread, pending.routine, crash::CurrentThreadName());
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// (apc, thread, environment, kernelRoutine, rundownRoutine, normalRoutine, mode, normalContext)
PPC_FUNC(__imp__KeInitializeApc)
{
    ApcRecord record;
    record.thread = ctx.r4.u32;
    record.kernelRoutine = ctx.r6.u32;
    record.rundownRoutine = ctx.r7.u32;
    record.normalRoutine = ctx.r8.u32;
    record.normalContext = ctx.r10.u32;
    std::lock_guard held(g_lock);
    ApcRecord& stored = g_records[ctx.r3.u32];
    const uint32_t scratch = stored.scratch;
    stored = record;
    stored.scratch = scratch ? scratch : AllocateGuest(32, 16);
}

// (apc, systemArgument1, systemArgument2, increment): returns whether it was queued.
PPC_FUNC(__imp__KeInsertQueueApc)
{
    Pending pending;
    pending.apc = ctx.r3.u32;
    pending.arguments[0] = ctx.r4.u32;
    pending.arguments[1] = ctx.r5.u32;
    uint32_t thread = 0;
    {
        std::lock_guard held(g_lock);
        const auto found = g_records.find(pending.apc);
        if (found == g_records.end() || found->second.inserted) { ctx.r3.u64 = 0; return; }
        found->second.inserted = true;
        thread = found->second.thread;
        g_queues[thread].push_back(pending);
    }
    static std::atomic<int> told{ 0 };
    if (told++ < 4) LOGI("apc: KeInsertQueueApc %08X -> thread %08X (from '%s')", pending.apc, thread, crash::CurrentThreadName());
    ctx.r3.u64 = 1;
}
