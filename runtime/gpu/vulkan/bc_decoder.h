#pragma once

#include "gpu/texture.h"
#include <cstdint>

namespace vk::bc
{
    // Whether this Xenos texture format is a Block-Compressed format (BC1..BC5).
    bool IsBCFormat(uint32_t xenosFormat);

    // How much of the Snapdragon workaround applies where the driver's own BC
    // sampling is left enabled (pipeline.cpp turns it off on Adreno, because
    // its compressed-sampling path renders BC1-with-1-bit-alpha surfaces black
    // or missing for a frame at a time). PES17_ADRENO_BC picks a narrower rule
    // for a driver that proves clean:
    //   hw    -- keep the driver's BC sampling, as PES17_FORCE_BC_HW=1 does
    //   dxt1  -- keep it, and decode only the BC1 family in software
    // Unset, or anything else, is every BC format in software.
    enum class AdrenoBC { kAll, kHardware, kDxt1Only };
    AdrenoBC AdrenoMode();

    // Decompress a BC texture into uncompressed formats (RGBA8, R8, RG8).
    // Returns true on success and updates outVulkanFormat to the uncompressed VkFormat.
    bool Decompress(const gpu::TextureData& src, uint32_t xenosFormat,
                    gpu::TextureData& dst, int& outVulkanFormat);

    // Prototype: a BC1 texture re-encoded as ASTC 4x4 LDR for a GPU that samples ASTC but
    // not BC (Adreno 650's stock driver). Sixteen bytes a block against RGBA8's sixty-four.
    // The four-colour BC1 block is two endpoints and four evenly spaced colours, which one
    // ASTC block (RGB direct endpoints, 2-bit weights 0, 21, 43, 64 of 64) reproduces to
    // within a level or two. The block counts and level layout stay those of `src`.
    // A block with a transparent texel (BC1's three-colour mode, index 3) takes the
    // dual-plane form: colour endpoints at 0..47 levels, alpha 255 or 0 in its own plane.
    // Returns false only for data that is not a whole number of blocks.
    bool TranscodeBc1ToAstc(const gpu::TextureData& src, gpu::TextureData& dst,
                            int& outVulkanFormat);

    // BC1, BC2 and BC3 (Xenos formats 18/51, 19/52, 20/53) to ASTC 4x4: BC1 as above;
    // BC2/BC3 with an alpha plane (exact where alpha is constant or two-level, four levels
    // between the block's extremes otherwise). Same layout rules as TranscodeBc1ToAstc.
    bool TranscodeToAstc(const gpu::TextureData& src, uint32_t xenosFormat, gpu::TextureData& dst,
                         int& outVulkanFormat);

    // One BC2 or BC3 block: sixteen bytes in, sixteen out.
    bool TranscodeBc23BlockToAstc(const uint8_t* block, bool bc3, uint8_t* astc);

    // BC2/BC3 blocks transcoded, those with varying alpha (dual plane), and of those the
    // blocks whose alpha is off by more than 8 and by more than 32 levels.
    struct AlphaBlockStats { uint64_t blocks, dual, off8, off32; };
    AlphaBlockStats AstcAlphaStats();

    // Totals over every texture transcoded: blocks, those that took the dual plane, and
    // those whose midpoint was off by more than four levels, and by more than sixteen.
    struct AstcBlockStats { uint64_t blocks, dual, approximate, severe; };
    AstcBlockStats AstcStats();

    // One block: eight BC1 bytes in, sixteen ASTC bytes out.
    bool TranscodeBc1BlockToAstc(const uint8_t* bc1, uint8_t* astc);
}
