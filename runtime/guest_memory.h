#pragma once
#include <cstdint>
#include <cstddef>

// The recompiled code addresses memory as a 32-bit offset from a single host
// base pointer, so the whole guest address space is one reservation.
//
// Layout (guest addresses). The console has one 512 MB bank of RAM and reaches
// it through several windows that differ only in page size and caching: a
// virtual address in 0x80000000-0xDFFFFFFF names physical (address & 0x1FFFFFFF).
// The same holds here -- one bank, mapped at all three windows -- so the
// command processor and the title always see the same bytes, whichever window
// either of them used:
//
//   0x80000000 .. 0xA0000000   cached window: XEX image, heap
//   0x10000000 .. 0x13300000   the recompiler's function lookup table (host
//                              pointers, not guest memory; outside the bank)
//   0xA0000000 .. 0xC0000000   physical window (MmAllocatePhysicalMemoryEx)
//   0xC0000000 .. 0xE0000000   the same bank again
//   0x20000000 .. 0x80000000   far heap: reservations too big for the bank,
//                              not visible to the GPU
//
// Bank offsets are handed out once, whichever window asks: [0, 32 MB) below the
// image, and from the end of the image to 512 MB. The image sits at its own
// offset as it would on the console. The function lookup table (2 x
// PPC_CODE_SIZE bytes, 51 MB for PES 2017) used to follow the image, inside the
// bank, and cost the title 51 MB of the console's 512; it lives below the far
// heap now, where generated code reaches it through PPC_CALL_INDIRECT_FUNC
// (redefined in mmio_hook.h).
namespace guest
{
    // The first heap address above the image, which ends at 0x840E0000 for
    // PES 2017 (guest_memory.cpp static_asserts it fits).
    inline constexpr uint32_t kHeapBase      = 0x84100000u;
    // The function lookup table: one host pointer per guest instruction word.
    inline constexpr uint32_t kFuncTableBase = 0x10000000u;
    inline constexpr uint32_t kHeapEnd       = 0xA0000000u;
    inline constexpr uint32_t kCachedBase    = 0x80000000u;   // the bank, cached window
    inline constexpr uint32_t kImagePhysical = 0x02000000u;   // where the XEX image sits in the bank
    inline constexpr uint32_t kPhysicalBase  = 0xA0000000u;
    inline constexpr uint32_t kPhysicalEnd   = 0xC0000000u;
    inline constexpr uint32_t kApertureBase  = 0xC0000000u;   // the same memory again
    // Large virtual reservations the general heap cannot hold (PES 2017 reserves
    // 512 MB in one NtAllocateVirtualMemory). Address space only: nothing is
    // backed until the title touches it, so the size costs no memory.
    inline constexpr uint32_t kFarBase       = 0x20000000u;
    inline constexpr uint32_t kFarEnd        = 0x80000000u;

    // Reserves the 4 GiB space and commits the regions above.
    uint8_t* Initialise();
    uint8_t* Base();

    // Used for stacks and late allocations.
    bool Commit(uint32_t address, uint32_t size);
    // From the fault handler: backs a page of the space that is reserved and
    // not yet committed, as Linux does on first touch. Windows only.
    bool CommitOnTouch(const void* hostAddress);

    inline void* Translate(uint32_t address) { return Base() + address; }

    // The table slot of guest code at `address` (PPC_CODE_BASE-relative, 8 bytes
    // per 4-byte instruction).
    inline uint64_t FunctionSlotOffset(uint32_t address, uint32_t codeBase)
    {
        return kFuncTableBase + uint64_t(address - codeBase) * 2;
    }
}
