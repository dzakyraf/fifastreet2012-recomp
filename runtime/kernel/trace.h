#pragma once
// A trace of every kernel/XAM import the title calls, in the order it calls
// them: the first few calls of each with their arguments, then every 2000th.
// What a port needs most is the *sequence* -- "the title asks for X, we answer
// Y, and then it asks for nothing else" -- which no single hook shows.
//
// Include this after kernel.h in a kernel/*.cpp that defines imports; it replaces
// PPC_FUNC for that file. (Not in files defining hooks: PPC_FUNC_IMPL builds on it.)
#include <atomic>
#include <cstdint>

namespace kernel
{
    // Logs `name(r3..r8) lr=... thread`.
    void TraceImport(const char* name, uint32_t count, const PPCContext& ctx);
    // `touch .../files/trace_all` (polled once a few frames, android/renderer): every call is logged,
    // not just the first three -- to follow one exchange (a dialog, a save) from end to end.
    extern std::atomic<bool> g_traceAll;
    // With trace_all: what an import returned (r3), on its own line after the call's.
    void TraceReturn(const char* name, const PPCContext& ctx);
}

#undef PPC_FUNC
#define PPC_FUNC(x)                                                                         \
    static void x##__body(PPCContext& __restrict ctx, uint8_t* base);                       \
    void x(PPCContext& __restrict ctx, uint8_t* base)                                       \
    {                                                                                       \
        static std::atomic<uint32_t> calls{ 0 };                                            \
        const uint32_t n = ++calls;                                                         \
        if (n <= 3 || n % 200000 == 0 || kernel::g_traceAll.load(std::memory_order_relaxed)) kernel::TraceImport(#x, n, ctx);                       \
        x##__body(ctx, base);                                                               \
        if (kernel::g_traceAll.load(std::memory_order_relaxed)) kernel::TraceReturn(#x, ctx);   \
    }                                                                                       \
    static void x##__body(PPCContext& __restrict ctx, uint8_t* base)
