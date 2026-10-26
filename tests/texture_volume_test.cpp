// Volume (3D) textures, which the world's light map and the calibration slider
// are made of: where each level of one lives in guest memory, and that untiling
// one reads every level from its own place rather than from its neighbour's.
//
// texture.cpp said the depth term of a volume's level stride was "Xenia's
// reading, not yet checked against a volume with mips" -- the one thing no
// desktop run of this runtime could confirm, because a volume is untiled from
// memory this repository has no copy of. What can be confirmed here is the
// arithmetic and the placement. The reference below is Xenia's
// GetGuestTextureLayout for a tiled 3D texture, transcribed for the comparison
// (BSD-3, as the runtime's own port credits it), so the two are independent
// readings of one algorithm rather than one reading used twice: the spans, the
// 4 KB-strided level offsets and the per-level content are all checked against
// it, and a level read from anywhere but its own place shows up as bytes that
// belong to another level.
//
// It asserts rather than reporting, so ctest passes or fails, and it needs no
// disc, no game file and no GPU.
#include "gpu/texture.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void Check(bool ok, const std::string& what)
    {
        g_checks++;
        if (ok) return;
        g_failures++;
        std::fprintf(stdout, "FAIL: %s\n", what.c_str());
    }

    uint32_t AlignUp(uint32_t value, uint32_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    size_t AlignUp(size_t value, size_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    uint32_t NextPow2(uint32_t value)
    {
        uint32_t result = 1;
        while (result < value) result <<= 1;
        return result;
    }

    uint32_t Log2Floor(uint32_t value)
    {
        uint32_t result = 0;
        while ((value >> (result + 1)) != 0) result++;
        return result;
    }

    // ---- the fetch constant a volume is described by ------------------------
    // Field layout as gpu::TextureFetch reads it (Xenia's xe_gpu_texture_fetch_t).
    gpu::TextureFetch Volume(uint32_t width, uint32_t height, uint32_t depth,
                             uint32_t format, uint32_t minLevel, uint32_t maxLevel,
                             uint32_t baseAddress, uint32_t mipAddress)
    {
        gpu::TextureFetch f{};
        f.d[0] = ((width >> 5) << 22) | (1u << 31);           // pitch, tiled
        f.d[1] = format | (baseAddress >> 12 << 12);          // format, endian none
        f.d[2] = (width - 1) | ((height - 1) << 11) | ((depth - 1) << 22);
        f.d[4] = (minLevel << 2) | (maxLevel << 6);
        f.d[5] = (2u << 9) | (mipAddress >> 12 << 12);        // k3D, no packed tail
        return f;
    }

    // ---- the reference: Xenia's GetGuestTextureLayout, tiled and 3D ---------
    // `span` is what one level's blocks cover -- row pitch, rows and the depth
    // rounded up to the four slices of a volume tile. `stride` is where the next
    // level starts: that span rounded up to a 4 KB boundary.
    struct Ref
    {
        size_t span = 0;
        size_t stride = 0;
    };

    Ref RefLevel(uint32_t width, uint32_t height, uint32_t depth,
                 const gpu::TextureFormatInfo& info, bool isBase, uint32_t level,
                 uint32_t pitchTexels)
    {
        const uint32_t bytesPerBlock = info.bitsPerBlock / 8;
        const uint32_t pitch = isBase ? pitchTexels
                                      : std::max(NextPow2(width) >> level, 1u);
        const uint32_t rows = isBase ? height
                                     : std::max(NextPow2(height) >> level, 1u);
        const uint32_t rowBytes =
            AlignUp(AlignUp(pitch, info.blockWidth) / info.blockWidth, 32) * bytesPerBlock;
        const uint32_t sliceRows =
            AlignUp(AlignUp(rows, info.blockHeight) / info.blockHeight, 32);
        Ref r;
        r.span = size_t(rowBytes) * sliceRows * AlignUp(depth, 4);
        r.stride = AlignUp(r.span, size_t(4096));
        return r;
    }

    size_t RefMipOffset(uint32_t width, uint32_t height, uint32_t depth,
                        const gpu::TextureFormatInfo& info, uint32_t level)
    {
        size_t offset = 0;
        for (uint32_t l = 1; l < level; l++)
            offset += RefLevel(width, height, depth, info, false, l, 0).stride;
        return offset;
    }

    struct Case
    {
        uint32_t width, height, depth, format;
    };

    // Wide, tall, deep, small and block-compressed, all with a full mip chain.
    const Case kCases[] = {
        {64, 64, 8, 6},   // 8_8_8_8, a byte a texel
        {64, 32, 8, 6},   // not square: the rows and the pitch diverge
        {32, 32, 16, 6},  // deeper than the tile's four slices
        {16, 16, 8, 6},
        {8, 8, 4, 6},     // one tile and a bit
        {64, 64, 8, 18},  // DXT1: four-by-four blocks
        {32, 32, 8, 18},
    };

    // The layout the runtime computes, against the reference's.
    void ExtentsMatchTheReference()
    {
        for (const Case& c : kCases)
        {
            const gpu::TextureFormatInfo info = gpu::TextureFormatOf(c.format);
            Check(info.known, "format " + std::to_string(c.format) + " is a known texture format");
            if (!info.known) continue;

            const uint32_t maxLevel = Log2Floor(std::max({c.width, c.height, c.depth}));
            const gpu::TextureFetch fetch = Volume(c.width, c.height, c.depth, c.format,
                                                   0, maxLevel, 0x10000, 0x200000);
            const gpu::TextureExtents extents = gpu::ExtentsOf(fetch);
            const std::string what = std::to_string(c.width) + "x" +
                                     std::to_string(c.height) + "x" + std::to_string(c.depth) +
                                     " format " + std::to_string(c.format);

            const Ref base = RefLevel(c.width, c.height, c.depth, info, true, 0, c.width);
            Check(extents.baseBytes == base.span,
                  what + ": the base level spans " + std::to_string(base.span) + " bytes, the "
                  "runtime says " + std::to_string(extents.baseBytes));

            const Ref last = RefLevel(c.width, c.height, c.depth, info, false, maxLevel, 0);
            const size_t expectedMip =
                RefMipOffset(c.width, c.height, c.depth, info, maxLevel) + last.span;
            Check(extents.mipBytes == expectedMip,
                  what + ": the mip levels span " + std::to_string(expectedMip) + " bytes, the "
                  "runtime says " + std::to_string(extents.mipBytes));

            Check(extents.maxLevel == maxLevel,
                  what + ": max level " + std::to_string(maxLevel) + ", the runtime says " +
                      std::to_string(extents.maxLevel));
        }
    }

    // Every level untiled from a buffer where each level's own place holds its
    // own byte: a level read from anywhere else comes back as its neighbour.
    void EveryLevelIsReadFromItsOwnPlace()
    {
        for (const Case& c : kCases)
        {
            const gpu::TextureFormatInfo info = gpu::TextureFormatOf(c.format);
            if (!info.known) continue;

            const uint32_t maxLevel = Log2Floor(std::max({c.width, c.height, c.depth}));
            const gpu::TextureFetch fetch = Volume(c.width, c.height, c.depth, c.format,
                                                   0, maxLevel, 0x10000, 0x200000);
            const std::string what = std::to_string(c.width) + "x" +
                                     std::to_string(c.height) + "x" + std::to_string(c.depth) +
                                     " format " + std::to_string(c.format);

            // A byte a level, and a byte for anything outside: reading past a
            // level is then not silent.
            const uint8_t outside = 0xEE;
            const Ref baseRef = RefLevel(c.width, c.height, c.depth, info, true, 0, c.width);
            const Ref lastRef = RefLevel(c.width, c.height, c.depth, info, false, maxLevel, 0);
            const size_t mipBytes =
                RefMipOffset(c.width, c.height, c.depth, info, maxLevel) + lastRef.stride;

            std::vector<uint8_t> base(baseRef.span + 4096, outside);
            std::vector<uint8_t> mips(mipBytes + 4096, outside);
            std::memset(base.data(), 0x40, baseRef.span);
            for (uint32_t level = 1; level <= maxLevel; level++)
            {
                const Ref r = RefLevel(c.width, c.height, c.depth, info, false, level, 0);
                const size_t at = RefMipOffset(c.width, c.height, c.depth, info, level);
                std::memset(mips.data() + at, uint8_t(0x40 + level), r.span);
            }

            const gpu::TextureData out = gpu::ReadTexture(fetch, base.data(), mips.data());
            Check(out.ok, what + ": reads as a volume");
            if (!out.ok)
            {
                Check(false, what + ": " + (out.error ? out.error : "no reason given"));
                continue;
            }
            Check(out.volume, what + ": is marked a volume");
            Check(out.layers == c.depth, what + ": has " + std::to_string(c.depth) +
                                              " slices, the runtime says " +
                                              std::to_string(out.layers));
            Check(out.levels.size() == maxLevel + 1,
                  what + ": has " + std::to_string(maxLevel + 1) + " levels, the runtime has " +
                      std::to_string(out.levels.size()));
            if (out.levels.size() != maxLevel + 1) continue;

            for (uint32_t level = 0; level <= maxLevel; level++)
            {
                const gpu::TextureData::Level& l = out.levels[level];
                const uint32_t expectedWidth = std::max(c.width >> level, 1u);
                const uint32_t expectedHeight = std::max(c.height >> level, 1u);
                const uint32_t expectedDepth = std::max(c.depth >> level, 1u);
                const std::string at = what + " level " + std::to_string(level);

                Check(l.width == expectedWidth && l.height == expectedHeight &&
                          l.layers == expectedDepth,
                      at + " is " + std::to_string(expectedWidth) + "x" +
                          std::to_string(expectedHeight) + "x" + std::to_string(expectedDepth) +
                          ", the runtime says " + std::to_string(l.width) + "x" +
                          std::to_string(l.height) + "x" + std::to_string(l.layers));

                // Contact with the buffer's guard bytes, or with another level.
                size_t wrong = 0;
                uint8_t firstWrong = 0;
                for (size_t i = 0; i < l.size; i++)
                {
                    const uint8_t got = out.bytes[l.offset + i];
                    if (got == uint8_t(0x40 + level)) continue;
                    if (!wrong) firstWrong = got;
                    wrong++;
                }
                Check(wrong == 0,
                      at + ": " + std::to_string(wrong) + " bytes of " + std::to_string(l.size) +
                          " are not this level's (first is " + std::to_string(firstWrong) +
                          ", this level is " + std::to_string(0x40 + level) + ")");
                Check(l.offset + l.size <= out.bytes.size(), at + " fits in the read");
            }
        }
    }

    // A volume addresses its three coordinates at once, so no two texels of a
    // level may share an address -- the shape of the bug that left three
    // quarters of a volume empty -- and none may fall outside the level's span.
    void TheVolumeAddressingIsOneToOne()
    {
        for (const Case& c : kCases)
        {
            const gpu::TextureFormatInfo info = gpu::TextureFormatOf(c.format);
            if (!info.known) continue;
            const uint32_t bytesPerBlock = info.bitsPerBlock / 8;
            const Ref r = RefLevel(c.width, c.height, c.depth, info, true, 0, c.width);

            const uint32_t rowBytes =
                AlignUp(AlignUp(c.width, info.blockWidth) / info.blockWidth, 32) * bytesPerBlock;
            const uint32_t sliceRows =
                AlignUp(AlignUp(c.height, info.blockHeight) / info.blockHeight, 32);
            const uint32_t blocksWide = AlignUp(c.width, info.blockWidth) / info.blockWidth;
            const uint32_t blocksHigh = AlignUp(c.height, info.blockHeight) / info.blockHeight;
            const uint32_t log2Block = Log2Floor(bytesPerBlock);

            std::set<int64_t> seen;
            bool outside = false;
            for (uint32_t z = 0; z < c.depth; z++)
                for (uint32_t y = 0; y < blocksHigh; y++)
                    for (uint32_t x = 0; x < blocksWide; x++)
                    {
                        const int64_t at = gpu::TiledOffset3D(int32_t(x), int32_t(y), int32_t(z),
                                                              rowBytes / bytesPerBlock, sliceRows,
                                                              log2Block);
                        if (at < 0 || size_t(at) + bytesPerBlock > r.span) outside = true;
                        seen.insert(at);
                    }
            const size_t expected = size_t(blocksWide) * blocksHigh * c.depth;
            const std::string what = std::to_string(c.width) + "x" + std::to_string(c.height) +
                                     "x" + std::to_string(c.depth) + " format " +
                                     std::to_string(c.format);
            Check(seen.size() == expected,
                  what + ": " + std::to_string(expected) + " blocks give " +
                      std::to_string(seen.size()) + " distinct addresses");
            Check(!outside, what + ": every block address is inside the level's span");
        }
    }
}

int main()
{
    ExtentsMatchTheReference();
    EveryLevelIsReadFromItsOwnPlace();
    TheVolumeAddressingIsOneToOne();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
