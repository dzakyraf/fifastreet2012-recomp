// Guest threads. Each gets a host thread, its own guest stack, its own KPCR
// block for r13, and its own PPCContext.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "objects.h"
#include "../guest.h"
#include "../log.h"
#include "../crash.h"
#include "../platform.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <memory>
#include "trace.h"

using namespace kernel;

namespace
{
    // ExTerminateThread does not return on the console; the CRT's thread exit
    // puts a KeBugCheck after it as an unreachable guard. Thrown here and caught
    // in the thread's Body(), it unwinds the recompiled frames (built with unwind
    // tables) back to where the thread started.
    struct ThreadTerminated { uint32_t status; };
}

namespace
{
    // A thread that names no stack size gets 512 KB. One that does gets what it
    // asked for, but no less than 64 KB: PES asks for 4-64 KB per thread, and at
    // the old floor of 512 KB its 60-odd threads held 30 MB of the bank, part of
    // what the match load then ran out of.
    constexpr uint32_t kDefaultStackSize = 512u * 1024u;
    constexpr uint32_t kMinStackSize = 256u * 1024u;

    // The host stack a guest thread runs on. It carries the recompiled
    // functions' frames, which are the guest's, and the guest's are large --
    // this is not the guest stack (kDefaultStackSize, allocated in guest
    // memory), it is the host's own. 8 MB matches what a Linux desktop build
    // gets for free and is what an Android one has to ask for.
    constexpr size_t kHostStackSize = 8u * 1024u * 1024u;

    // The main thread is id 1 (main.cpp); the first thread the title creates is
    // XAudio2's mixer, and game code that runs from its voice callbacks must
    // not pass the title's "is this the main thread" checks.
    std::atomic<uint32_t> g_nextThreadId{ 2 };   // the main thread is 1

    // Signalled once it has exited, which is what a wait on a thread waits for.
    struct GuestThread : Dispatcher
    {
        uint32_t threadId = 0;
        uint32_t xapiStartup = 0;
        uint32_t startAddress = 0;
        uint32_t startContext = 0;
        uint32_t stackBase = 0;
        uint32_t stackSize = 0;
        uint32_t pcr = 0;
        uint8_t processor = 0;
        bool exited = false;      // under the dispatcher lock
        Event resumeGate;         // signalled when the thread may run

        const char* TypeName() const override { return "thread"; }
        bool Signalled(uintptr_t) const override { return exited; }

        // The host thread holds the object for as long as it runs.
        //
        // Not std::thread: the recompiled code puts the guest's frames on the
        // host stack, and the host's default is not always enough for them.
        // 8 MB is what glibc gives a thread anyway, so only bionic notices
        // the difference (platform::StartThread).
        static void Launch(const std::shared_ptr<GuestThread>& thread)
        {
            // The host thread owns this reference and drops it on the way out.
            auto* held = new std::shared_ptr<GuestThread>(thread);
            const bool started = platform::StartThread(
                [](void* raw)
                {
                    std::unique_ptr<std::shared_ptr<GuestThread>> owner(
                        static_cast<std::shared_ptr<GuestThread>*>(raw));
                    (*owner)->Body();
                },
                held, kHostStackSize, "guest thread");
            if (!started)
            {
                delete held;
                LOGE("kernel: a guest thread could not be started");
            }
        }

        // The KTHREAD this thread is to the title (its dispatcher header): the
        // CRT's GetExitCodeThread reads the signal state at +4 and, once it is
        // set, the exit status at +320 -- polling for STILL_ACTIVE (259) until
        // then. PES 2017's sound shutdown waits on exactly that.
        static constexpr uint32_t kExitStatusOffset = 320;
        uint32_t KThread() const { return pcr ? uint32_t(*GuestPtr<be32>(pcr + 256)) : 0; }

        void Exit(uint32_t status = 0)
        {
            std::lock_guard held(DispatcherLock());
            if (const uint32_t kthread = KThread())
            {
                *GuestPtr<be32>(kthread + kExitStatusOffset) = status;
                *GuestPtr<be32>(kthread + 4) = 1;   // signalled: the thread has ended
            }
            exited = true;
            Wake();
        }

        void Body()
        {
            Dispatcher* gate = &resumeGate;
            Wait(&gate, 1, false, nullptr);
            platform::SetThreadAffinity(platform::ThreadAffinityRole::PerformanceCore);

            // ExCreateThread's fourth argument is XAPI's thread-startup shim, and the
            // kernel enters that rather than the routine: it installs the per-thread state
            // -- notably the TLS array the KPCR points at, out of which the CRT and zlib
            // allocate -- and then calls the routine. Jumping straight to the routine
            // leaves every allocation from a created thread reading an empty pool.
            const bool viaShim = xapiStartup != 0;
            uint32_t entry = viaShim ? xapiStartup : startAddress;

            PPCFunc* fn = kernel::GuestFunction(entry);
            if (!fn)
            {
                LOGE("thread %u: no recompiled function at %08X", threadId, entry);
                Exit();
                return;
            }

            PPCContext ctx{};
            std::memset(&ctx, 0, sizeof(ctx));
            ctx.r1.u32 = (stackBase + stackSize - 0x100) & ~0xFu;
            ctx.r13.u32 = pcr;
            if (viaShim) { ctx.r3.u32 = startAddress; ctx.r4.u32 = startContext; }
            else         { ctx.r3.u32 = startContext; }
            ctx.fpscr.loadFromHost();

            char label[64];
            std::snprintf(label, sizeof label, "guest thread %u (routine %08X)", threadId, startAddress);
            crash::RegisterThread(label);

            uint32_t status = 0;
            try
            {
                fn(ctx, guest::Base());
                status = ctx.r3.u32;   // the routine's return value is the exit code
                LOGK("thread %u returned", threadId);
            }
            catch (const ThreadTerminated& t)
            {
                status = t.status;
                LOGK("thread %u terminated itself (%u)", threadId, t.status);
            }
            crash::UnregisterThread();
            // The stack goes back to the bank; the thread block stays, since a
            // waiter reads the exit status out of its KTHREAD afterwards.
            kernel::FreeGuest(stackBase);
            Exit(status);
        }
    };
}

uint32_t kernel::NewThreadId() { return g_nextThreadId++; }

PPC_FUNC(__imp__ExCreateThread)
{
    auto* handleOut  = GuestPtr<be32>(ctx.r3.u32);
    uint32_t stackSize = ctx.r4.u32 ? ctx.r4.u32 : kDefaultStackSize;
    auto* threadIdOut = GuestPtr<be32>(ctx.r5.u32);
    uint32_t xapiStartup  = ctx.r6.u32;
    uint32_t startAddress = ctx.r7.u32;
    uint32_t startContext = ctx.r8.u32;
    uint32_t creationFlags = ctx.r9.u32;

    if (startAddress < PPC_CODE_BASE || startAddress >= PPC_CODE_BASE + PPC_CODE_SIZE)
    {
        LOGW("ExCreateThread: start address %08X is not guest code", startAddress);
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    auto t = std::make_shared<GuestThread>();
    t->threadId = g_nextThreadId++;
    t->xapiStartup = xapiStartup;
    t->startAddress = startAddress;
    t->startContext = startContext;
    t->stackSize = (std::max(stackSize, kMinStackSize) + 0xFFFu) & ~0xFFFu;
    t->stackBase = kernel::AllocateGuest(t->stackSize, 0x1000);

    t->pcr = kernel::CreateThreadBlock(t->threadId);
    if (!t->stackBase || !t->pcr) { ctx.r3.u64 = X_STATUS_NO_MEMORY; return; }

    // The top byte of the flags is a mask naming the hardware thread to start
    // on (one bit; Xenia's GetFakeCpuNumber). XAudio2 creates one mixing thread
    // per processor it was given and each waits, by processor number, for the
    // others to check in -- with every thread reporting 0 that barrier never
    // completes and the mixer stalls after its first frame.
    if (const uint32_t mask = creationFlags >> 24)
    {
        t->processor = uint8_t(__builtin_ctz(mask));
        *GuestPtr<uint8_t>(t->pcr + 268) = t->processor;   // KPCR.CurrentProcessorNumber
    }

    uint32_t handle = InsertHandle(t, crash::CurrentThreadName(), uint32_t(ctx.lr));
    if (handleOut) *handleOut = handle;
    if (threadIdOut) *threadIdOut = t->threadId;
    // The KTHREAD is the thread's dispatcher header: ObReferenceObjectByHandle
    // hands it out and KeResumeThread and KeSetAffinityThread take it.
    constexpr uint32_t kCurrentThreadOffset = 256;   // KPCR.CurrentThread
    constexpr uint8_t kThreadObject = 6;
    AttachHeader(t, *GuestPtr<be32>(t->pcr + kCurrentThreadOffset), kThreadObject);

    GuestThread::Launch(t);
    constexpr uint32_t kCreateSuspended = 1;
    if (!(creationFlags & kCreateSuspended)) t->resumeGate.Set();

    LOGK("ExCreateThread -> id %u handle %08X at %08X, %u KB stack at %08X, processor %u"
         " (flags %08X), r13 %08X%s",
         t->threadId, handle, startAddress, stackSize / 1024, t->stackBase, t->processor,
         creationFlags, t->pcr,
         (creationFlags & kCreateSuspended) ? " (suspended)" : "");
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// (callback, message, optionalArguments, handleOut): XAM runs `callback(message)` on a thread of its own.
// FIFA Street's content enumeration across devices (XamContentAggregateCreateEnumerator, then an
// enumerate through a system message) is such a task: it marks its overlapped pending, schedules the
// task, and the task completes the overlapped and ends.
PPC_FUNC(__imp__XamTaskSchedule)
{
    const uint32_t callback = ctx.r3.u32, message = ctx.r4.u32;
    auto* handleOut = GuestPtr<be32>(ctx.r6.u32);
    if (callback < PPC_CODE_BASE || callback >= PPC_CODE_BASE + PPC_CODE_SIZE)
    {
        LOGW("XamTaskSchedule: callback %08X is not guest code", callback);
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }
    auto t = std::make_shared<GuestThread>();
    t->threadId = g_nextThreadId++;
    t->startAddress = callback;
    t->startContext = message;
    t->stackSize = kMinStackSize;
    t->stackBase = kernel::AllocateGuest(t->stackSize, 0x1000);
    t->pcr = kernel::CreateThreadBlock(t->threadId);
    if (!t->stackBase || !t->pcr) { ctx.r3.u64 = X_STATUS_NO_MEMORY; return; }
    const uint32_t handle = InsertHandle(t, crash::CurrentThreadName(), uint32_t(ctx.lr));
    if (handleOut) *handleOut = handle;
    constexpr uint32_t kCurrentThreadOffset = 256;   // KPCR.CurrentThread
    constexpr uint8_t kThreadObject = 6;
    AttachHeader(t, *GuestPtr<be32>(t->pcr + kCurrentThreadOffset), kThreadObject);
    GuestThread::Launch(t);
    t->resumeGate.Set();
    LOGI("XamTaskSchedule: task %08X(%08X) on thread %u, handle %08X", callback, message, t->threadId, handle);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__XamTaskCloseHandle) { ctx.r3.u64 = CloseHandle(ctx.r3.u32) ? 1 : 0; }
PPC_FUNC(__imp__XamTaskShouldExit) { ctx.r3.u64 = 0; }

PPC_FUNC(__imp__NtResumeThread)
{
    auto t = LookupHandleAs<GuestThread>(ctx.r3.u32);
    if (auto* previous = GuestPtr<be32>(ctx.r4.u32)) *previous = 1;
    if (t) t->resumeGate.Set();
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// Takes the thread object (its KTHREAD) rather than a handle. XAudio2 creates
// its mixing thread suspended and starts it this way.
PPC_FUNC(__imp__KeResumeThread)
{
    auto t = ObjectAtAs<GuestThread>(ctx.r3.u32);
    if (t) t->resumeGate.Set();
    else LOGW("KeResumeThread: %08X is not a guest thread", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NtSuspendThread)
{
    // Suspending a running host thread safely is not possible here; report
    // success so the title's bookkeeping stays consistent.
    if (auto* previous = GuestPtr<be32>(ctx.r4.u32)) *previous = 0;
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__ExTerminateThread)
{
    throw ThreadTerminated{ ctx.r3.u32 };
}

// XSetThreadProcessor reaches the kernel as a one-bit affinity mask over the
// console's six hardware threads. The number behind it is not bookkeeping:
// IW4 reads the processor out of its own KPCR (a byte at r13+268) and
// dispatches on it. The renderer's command-buffer jobs are only ever accepted
// on processors 1 and 2, so with every thread reporting 0 they are refused for
// ever and the first frame of a level never finishes.
PPC_FUNC(__imp__KeSetAffinityThread)
{
    constexpr uint32_t kProcessorOffset = 268;   // KPCR.CurrentProcessorNumber
    const uint32_t affinity = ctx.r4.u32;
    auto t = ObjectAtAs<GuestThread>(ctx.r3.u32);
    if (t && affinity)
    {
        t->processor = uint8_t(__builtin_ctz(affinity));
        *GuestPtr<uint8_t>(t->pcr + kProcessorOffset) = t->processor;
    }
    else if (affinity)
        LOGW("KeSetAffinityThread: %08X is not a guest thread", ctx.r3.u32);
    ctx.r3.u64 = affinity;
}
PPC_FUNC(__imp__KeSetBasePriorityThread)  { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__ExRegisterTitleTerminateNotification) { ctx.r3.u64 = 0; }
