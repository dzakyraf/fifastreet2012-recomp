#pragma once
#if __has_include(<ppc_recomp_shared.h>)
#include <ppc_recomp_shared.h>
#else
#include "../../ppc/ppc_recomp_shared.h"
#endif
#include "physical.h"
#include "durable.h"
#include "../guest.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <filesystem>
#include <functional>

namespace kernel
{
    // What this thread has asked KeDelayExecutionThread for: the sleeps (count, nanoseconds
    // asked) and the zero-length ones (yields). Read around a guest call to tell what it slept.
    struct DelayCounts { uint64_t sleeps = 0, sleptNs = 0, yields = 0; };
    DelayCounts& ThreadDelays();

    // The threads blocked in a kernel wait, and on what: for the watchdog.
    void ReportWaits();

    void Initialise(const std::filesystem::path& gameRoot);
    const std::filesystem::path& GameRoot();

    uint32_t AllocateGuest(uint32_t size, uint32_t alignment = 16);
    uint32_t AllocatePhysical(uint32_t size, uint32_t alignment = 4096);
    // False when `address` is not the start of a live allocation.
    bool     FreeGuest(uint32_t address);
    bool     FreePhysical(uint32_t address);
    void     ReportMemory();
    // The heap block holding `address` (as the window it was named through).
    bool     FindAllocation(uint32_t address, uint32_t& at, uint32_t& size);
    // Which allocations (by either API) cover the bank bytes this guest address names
    // through any window, with the guest call chain that made each. For hunting memory
    // that two owners think is theirs.
    std::string OwnersOf(uint32_t guestAddress);
    // Walks the XAPI process heap's large-block free list (PES's malloc); true if it is
    // intact. Logs the first broken node and its owners. `quiet` skips the OK line.
    bool     CheckProcessHeap(bool quiet);
    // One line: what the allocator did since the last call (allocations, free-list scan,
    // zeroing, lock waits, free-list length, write faults). Called every ~2 s while
    // files/alloc_watch exists.
    void     ReportAllocRate();
    // What the bank (the console's 512 MB) has left for the title.
    uint64_t BankFreeBytes();
    void     ReportApiMemory();
    void     DumpYieldStacks(int perThread);   // log where yield-spinning threads are

    // Returns the value r13 must hold. The recompiled code reads the thread id
    // straight out of these structures (KPCR.CurrentThread -> KTHREAD.ThreadId),
    // so they have to be populated or every thread reports id 0.
    uint32_t CreateThreadBlock(uint32_t threadId);
    // A fresh id from the same sequence guest threads draw on, for a host
    // thread that runs guest code (the audio worker).
    uint32_t NewThreadId();

    // The kernel publishes a small block of clock values the title reads directly
    // rather than through a call. It arrives as a data import, which nothing
    // resolves, so it has to be created and kept ticking here. 100ns ticks since
    // 1601-01-01, the epoch the console reports.
    uint64_t SystemTime100ns();

    // The bundle, made on first use; every module's KeTimeStampBundle import points here.
    uint32_t TimeStampBundle();
    // Every data import of one module (its XEX header in guest memory) still
    // holding its import record: kernel variables get a block of their own (a
    // critical section is made a free one; the time stamp bundle and the debug
    // monitor word are shared), and a function's import-table slot gets its
    // thunk, so a call through the table reaches the recompiled import
    // (pes_imports.cpp). kernel::modules calls it for each module it maps.
    void ResolveDataImports(uint32_t header, uint32_t headerSize);
    void InitialiseCriticalSection(uint32_t cs);
    // KeDebugMonitorData's word, made on first use: null, as on a retail console.
    uint32_t DebugMonitorData();
    void UpdateTimeStampBundle();

    // The recompiled function at a guest address, or null when the address is
    // outside the image's code or has no function starting there.
    PPCFunc* GuestFunction(uint32_t address);

    // The nth argument (from 0) of a guest call, for the ones past the eight that
    // arrive in r3-r10: the caller's parameter save area starts at r1+0x14 with
    // eight-byte slots, so the ninth is at r1+0x54.
    inline uint32_t StackArgument(const PPCContext& ctx, uint32_t n)
    {
        return *reinterpret_cast<const be32*>(guest::Base() + ctx.r1.u32 + 0x14 + n * 8);
    }

    // Borrows the caller's stack below its frame. Returns the routine's r3.
    uint32_t CallGuest(const PPCContext& caller, uint32_t address,
                   uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0);

    // User APCs. The title issues asynchronous reads and does its accounting in a
    // completion routine, which NT delivers only when the issuing thread enters an
    // alertable wait -- never inside the call that queued it.
    //
    // `work` runs just before the routine, at delivery time. Asynchronous reads
    // put the transfer itself there: the title double-buffers on the assumption
    // that a read it has issued is still in flight while it decompresses the other
    // buffer, so filling the buffer at issue time corrupts what it is reading.
    void QueueUserApc(uint32_t routine, uint32_t context, uint32_t ioStatusBlock,
                      std::function<void()> work = {});
    bool DeliverUserApcs(const PPCContext& caller);
    // APCs other threads queued to this one (NtQueueApcThread, KeInsertQueueApc; kernel/apc.cpp),
    // keyed by the KTHREAD the caller's r13 names. True if any ran.
    bool DeliverThreadApcs(const PPCContext& caller);
    bool HasThreadApcs(const PPCContext& caller);

    // XOVERLAPPED. Asynchronous XAM calls take one and the caller polls it, so a
    // call that completes inline still has to mark it finished and signal the
    // event the caller may be waiting on.
    void CompleteOverlapped(uint32_t address, uint32_t result);
    // The same, for a call that failed: the caller reads `result` from the
    // block, and why from its extended error.
    void CompleteOverlappedEx(uint32_t address, uint32_t result, uint32_t extendedError,
                              uint32_t length);

    // The same, a few milliseconds later and from another thread, the way Xenia completes an
    // asynchronous XAM call (CompleteOverlappedDeferredEx): the overlapped reads ERROR_IO_PENDING until
    // then and the caller's event stays unsignalled. FIFA Street's save library reads the block back
    // between the call and its completion.
    void CompleteOverlappedLater(uint32_t address, uint32_t result, uint32_t extendedError,
                                 uint32_t length, uint32_t delayMilliseconds = 5);

    // System modules the title looks into at run time (XexGetModuleHandle("xam.xex") + XexGetProcedureAddress
    // by ordinal) rather than importing. A system export is a host function with a guest address of its
    // own: a stub in the unused part of the image that the function table maps to the function, so the
    // title's indirect call into it lands here. 0 when the ordinal is not one that is provided.
    int  SystemModule(const std::string& name);                   // index, or -1
    uint32_t SystemModuleHandle(int module);                       // a guest address, made on first use
    int  SystemModuleOfHandle(uint32_t handle);                    // index, or -1
    uint32_t SystemExport(int module, uint32_t ordinal);           // the stub's guest address, or 0
    // The provided exports (xam.xex): XamContentAggregateCreateEnumerator (633).
    void XamContentAggregateCreateEnumerator(PPCContext& ctx, uint8_t* base);
    // xam.xex 625: XamContentCreateEx taking the account and title id in the record.
    void XamContentCreateTitle(PPCContext& ctx, uint8_t* base);
    // xam.xex 624: XamContentDelete for that record.
    void XamContentDeleteTitle(PPCContext& ctx, uint8_t* base);

    // XNotify: queues a notification for every listener that asked for its area.
    void PostNotification(uint32_t id, uint32_t param);
    // An accepted Xbox LIVE invitation: XInviteGetAcceptedInfo reports it from
    // here on, and the title is told it arrived.
    void AcceptInvite(uint64_t inviterAccount, const uint8_t* session60, bool fromInvite);

    // Content packages -- saved games. The title mounts one as a lettered drive
    // ("save0:") and then opens "save0:\savegame.svg" through the ordinary file
    // API, so XAM and the file layer have to share one table of device roots.
    // A package here is a directory under SaveRoot(); nothing writes the console's
    // STFS container format.
    void MountDevice(const std::string& name, const std::filesystem::path& root);
    void UnmountDevice(const std::string& name);
    const std::filesystem::path& SaveRoot();
    // What is kept under a player's number -- his rank, offline and Live, and
    // the settings of a profile at the second to fourth controllers -- renamed
    // to another number, where that one has none yet (kernel/xam.cpp).
    void MovePlayerData(uint64_t from, uint64_t to);

    // The one storage device this runtime offers. The title takes the id from the
    // device selector, hands it back in every content call and stores it in the
    // package it creates, so the selector and the content layer have to agree.
    constexpr uint32_t kSaveDeviceId = 1;

    // The console's own network address, as XNetGetTitleXnAddr reports it: 36
    // bytes of XNADDR. XSessionCreate has to put the same one in the session it
    // hands out, so the two answers come from one place.
    void TitleXnAddr(void* out36);
    // The 8-byte id of the session this machine hosts or joined, which
    // XNetInAddrToXnAddr names for a player the title did not look up itself.
    void NoteSessionId(const void* id8);

    // Called by generated stubs; logs the first hit and counts the rest.
    void Unimplemented(const char* name);

    // End-of-run reports.
    void ReportUnimplemented();
    void ReportMissingFiles();
}
