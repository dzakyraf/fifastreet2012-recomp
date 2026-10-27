// Native memcpy / memset / memmove. The recompiled versions are byte and word loops of volatile,
// byte-swapped loads and stores that the compiler cannot vectorise; their guest contract is simple
// (r3 = destination, r4 = source or fill byte, r5 = byte count, r3 returned), so the host's does the
// same work. Ideas and addresses from the ReStreet port (reference/fifa-street-pc-port).
//
// Falls back to the recompiled body for a range that leaves the 4 GiB space, and for a memcpy whose
// ranges overlap: the original copies forward byte by byte, which a title can rely on to repeat a
// pattern. The bank is mapped at three windows (guest_memory.h), so overlap is judged on the bank
// offset. PES17_NATIVE_LIBC=0 uses the original code (A/B testing).
#include <ppc_recomp_shared.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "title.h"
#include "guest_memory.h"

namespace
{
    bool Enabled()
    {
        static const bool on = [] { const char* v = std::getenv("PES17_NATIVE_LIBC"); return !(v && v[0] == '0'); }();
        return on;
    }

    inline bool Plain(uint32_t address, uint32_t size) { return uint64_t(address) + size <= 0x100000000ull; }

    // The address a byte has in the bank, whichever window names it; other addresses stand for themselves.
    inline uint64_t Identity(uint32_t address)
    {
        return address >= guest::kCachedBase && address < 0xE0000000u ? (address & 0x1FFFFFFFu) | (1ull << 32) : address;
    }

    inline bool Overlaps(uint32_t a, uint32_t b, uint32_t size)
    {
        const uint64_t x = Identity(a), y = Identity(b);
        return x < y + size && y < x + size;
    }
}

// memcpy
GUEST_HOOK(82738530)
{
    const uint32_t destination = ctx.r3.u32, source = ctx.r4.u32, size = ctx.r5.u32;
    if (Enabled() && Plain(destination, size) && Plain(source, size) && !Overlaps(destination, source, size))
    {
        if (size) std::memcpy(base + destination, base + source, size);
        return;   // r3 still holds the destination, as the original returns it
    }
    GUEST_ORIG(82738530)(ctx, base);
}

// memset
GUEST_HOOK(82737D50)
{
    const uint32_t destination = ctx.r3.u32, size = ctx.r5.u32;
    if (Enabled() && Plain(destination, size))
    {
        if (size) std::memset(base + destination, int(ctx.r4.u32 & 0xFF), size);
        return;
    }
    GUEST_ORIG(82737D50)(ctx, base);
}

// memmove
GUEST_HOOK(82738F80)
{
    const uint32_t destination = ctx.r3.u32, source = ctx.r4.u32, size = ctx.r5.u32;
    if (Enabled() && Plain(destination, size) && Plain(source, size))
    {
        if (size && destination != source) std::memmove(base + destination, base + source, size);
        return;
    }
    GUEST_ORIG(82738F80)(ctx, base);
}
