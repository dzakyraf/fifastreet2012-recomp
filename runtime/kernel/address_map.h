#pragma once
#include <cstdint>
#include "../guest_memory.h"

// Which virtual address a physical one names.
//
// The title reduces a virtual address to a physical one by masking off the top
// three bits, so two arenas come out the same: the general heap at 0x84600000
// and the physical arena at 0xA4600000 are both physical 0x04600000. On the
// console those are two windows onto one 512 MB bank. Here they are separate
// memory (guest_memory.h), so the window meant has to be identified rather than
// guessed -- get it wrong and the command processor writes one copy while the
// title polls the other, which is a WAIT_REG_MEM that can never be satisfied.
//
// Everything at or above physical 0x04600000 -- the whole heap -- is ambiguous
// on the arithmetic alone, so the caller says what each arena actually has live
// at that offset and this picks between them.
//
// Pure and dependency-free, so tests/address_map_test.cpp can check the decision
// table without the guest or the recompiled image behind it.
namespace kernel
{
    // What an arena says about an offset. Unknown is a lock that could not be
    // taken without waiting: the command processor asks this per packet, and
    // the allocator holds its lock across a memset, so blocking there would
    // trade one stall for another.
    enum class Coverage { No, Yes, Unknown };

    inline uint32_t ResolvePhysical(uint32_t phys, Coverage inPhysicalArena, Coverage inHeap,
                                    uint32_t physicalHighWater, bool* corrected = nullptr)
    {
        if (corrected) *corrected = false;

        const uint32_t arena  = guest::kPhysicalBase + phys;
        const uint32_t cached = 0x80000000u + phys;
        const bool mightBeArena  = arena >= guest::kPhysicalBase && arena < guest::kPhysicalEnd;
        const bool mightBeCached = cached >= 0x82000000u && cached < guest::kHeapEnd;

        // The image's own data and the function lookup table below the heap are
        // not allocated through either arena, so nothing covers them and there
        // is nothing to tell apart.
        if (mightBeCached && cached < guest::kHeapBase) return cached;

        // The ceiling the answer used to rest on: it only rises, so once the
        // title had allocated past a heap address' offset every heap address
        // came out as the physical arena.
        const bool belowHighWater =
            mightBeArena && physicalHighWater > guest::kPhysicalBase &&
            phys < (physicalHighWater - guest::kPhysicalBase);

        // Where both arenas hold something there the address is genuinely
        // ambiguous, and the physical arena is what the GPU is handed.
        if (inPhysicalArena == Coverage::Yes) return arena;
        if (inHeap == Coverage::Yes)
        {
            // The case that used to come out wrong, worth counting.
            if (corrected) *corrected = belowHighWater;
            return cached;
        }

        // Nothing live either side, or a lock that could not be taken. An
        // address can be named before the title allocates it, so answering as
        // before beats refusing to answer.
        if (belowHighWater) return arena;
        if (mightBeCached) return cached;
        return 0;
    }
}
