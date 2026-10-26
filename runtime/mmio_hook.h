#pragma once
// Force-included into every recompiled translation unit (see CMakeLists.txt).
//
// The recompiler emits PPC_MM_STORE_* for a store it can tell is aimed at a
// hardware register -- one followed by eieio. In this runtime those registers
// are ordinary memory, so the store still lands there (the title reads some of
// them back), and the XMA decoder's window is additionally reported to the
// runtime: its kick, lock and clear registers are commands, not state, and
// nothing could poll a bit mask for a second write of the same value.
//
// `value` is what the guest's stw would write, i.e. before the byte swap. Only
// 32-bit stores are hooked; the title issues no other width at the window.
#include <cstdint>
#include "guest_memory.h"

extern "C" void pes17_xma_register_store(uint32_t address, uint32_t value);

#define PPC_MM_STORE_U32(x, y)                                              \
    do {                                                                    \
        const uint32_t pes17_ea = (x);                                        \
        const uint32_t pes17_value = (y);                                     \
        PPC_STORE_U32(pes17_ea, pes17_value);                                   \
        if ((pes17_ea & 0xFFFF0000u) == 0x7FEA0000u)                          \
            pes17_xma_register_store(pes17_ea, pes17_value);                      \
    } while (0)

// Diagnostics: a stored +/-infinity, which a title computing 1/0 leaves where
// it was meant to leave a scale. Armed at run time (`touch .../files/trap_inf`),
// so the check costs a load and a compare per 32-bit store.
#ifdef PES_TRAP_INF
extern "C" void pes_trap_inf_store(uint32_t address, uint32_t value);
extern "C" volatile int pes_trap_inf_armed;
#define PPC_STORE_U32(x, y)                                                 \
    do {                                                                    \
        const uint32_t pt_ea = (x);                                         \
        const uint32_t pt_v = (y);                                          \
        if (__builtin_expect(pes_trap_inf_armed &&                          \
                             (pt_v & 0x7FFFFFFFu) == 0x7F800000u, 0))       \
            pes_trap_inf_store(pt_ea, pt_v);                                \
        *(volatile uint32_t*)(base + pt_ea) = __builtin_bswap32(pt_v);      \
    } while (0)
#endif

// The function lookup table sits at guest::kFuncTableBase, not after the image
// where ppc_context.h's PPC_LOOKUP_FUNC looks (that spot is bank memory the
// title needs). ppc_recomp_shared.h's tracing PPC_CALL_INDIRECT_FUNC goes
// through PPC_FUNC_SLOT; ppc_context.h's plain one is only defined when it is
// not defined already.
#define PPC_FUNC_SLOT(b, x)                                                 \
    (*(PPCFunc**)((b) + guest::FunctionSlotOffset(uint32_t(x), uint32_t(PPC_CODE_BASE))))
#ifndef PES17_TRACE_INDIRECT
#define PPC_CALL_INDIRECT_FUNC(x) (PPC_FUNC_SLOT(base, x))(ctx, base)
#endif
