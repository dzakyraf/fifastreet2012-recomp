#include "bc_decoder.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../env.h"
#include "../../log.h"

namespace vk::bc
{
    bool IsBCFormat(uint32_t xenosFormat)
    {
        switch (xenosFormat)
        {
        case 18: case 51: // BC1 (DXT1)
        case 19: case 52: // BC2 (DXT2_3)
        case 20: case 53: // BC3 (DXT4_5)
        case 49:          // BC5 (DXN)
        case 59:          // BC4 (DXT5A)
            return true;
        default:
            return false;
        }
    }

    // Cached, because the environment does not change under a run.
    AdrenoBC AdrenoMode()
    {
        static const AdrenoBC mode = []
        {
            const char* value = env::Text("PES17_ADRENO_BC");
            if (!value) return AdrenoBC::kAll;
            if (!std::strcmp(value, "hw")) return AdrenoBC::kHardware;
            if (!std::strcmp(value, "dxt1") || !std::strcmp(value, "narrow")) return AdrenoBC::kDxt1Only;
            LOGW("vulkan: PES17_ADRENO_BC=%s is neither hw nor dxt1; the whole workaround applies", value);
            return AdrenoBC::kAll;
        }();
        return mode;
    }

    namespace
    {
        // Fast RGB565 to RGBA8 unpack (zero divisions, exact bit-expansion).
        inline uint32_t Rgb565ToRgba8(uint16_t c)
        {
            const uint32_t r = (c >> 11) & 0x1F;
            const uint32_t g = (c >> 5) & 0x3F;
            const uint32_t b = c & 0x1F;
            const uint32_t r8 = (r << 3) | (r >> 2);
            const uint32_t g8 = (g << 2) | (g >> 4);
            const uint32_t b8 = (b << 3) | (b >> 2);
            return r8 | (g8 << 8) | (b8 << 16) | 0xFF000000u;
        }

        inline void DecodeBC1Palette(const uint8_t* block, uint32_t colors[4], bool forceFourColor = false)
        {
            const uint16_t c0 = block[0] | (uint16_t(block[1]) << 8);
            const uint16_t c1 = block[2] | (uint16_t(block[3]) << 8);

            colors[0] = Rgb565ToRgba8(c0);
            colors[1] = Rgb565ToRgba8(c1);

            const uint32_t r0 = colors[0] & 0xFF;
            const uint32_t g0 = (colors[0] >> 8) & 0xFF;
            const uint32_t b0 = (colors[0] >> 16) & 0xFF;

            const uint32_t r1 = colors[1] & 0xFF;
            const uint32_t g1 = (colors[1] >> 8) & 0xFF;
            const uint32_t b1 = (colors[1] >> 16) & 0xFF;

            if (c0 > c1 || forceFourColor)
            {
                const uint32_t r2 = (2 * r0 + r1) / 3;
                const uint32_t g2 = (2 * g0 + g1) / 3;
                const uint32_t b2 = (2 * b0 + b1) / 3;
                colors[2] = r2 | (g2 << 8) | (b2 << 16) | 0xFF000000u;

                const uint32_t r3 = (r0 + 2 * r1) / 3;
                const uint32_t g3 = (g0 + 2 * g1) / 3;
                const uint32_t b3 = (b0 + 2 * b1) / 3;
                colors[3] = r3 | (g3 << 8) | (b3 << 16) | 0xFF000000u;
            }
            else
            {
                const uint32_t r2 = (r0 + r1) / 2;
                const uint32_t g2 = (g0 + g1) / 2;
                const uint32_t b2 = (b0 + b1) / 2;
                colors[2] = r2 | (g2 << 8) | (b2 << 16) | 0xFF000000u;
                colors[3] = 0x00000000u; // Transparent black
            }
        }

        inline void DecodeBC3AlphaTable(const uint8_t* block, uint8_t alphas[8], uint64_t& bits)
        {
            alphas[0] = block[0];
            alphas[1] = block[1];
            bits = uint64_t(block[2]) |
                   (uint64_t(block[3]) << 8) |
                   (uint64_t(block[4]) << 16) |
                   (uint64_t(block[5]) << 24) |
                   (uint64_t(block[6]) << 32) |
                   (uint64_t(block[7]) << 40);

            const uint32_t a0 = alphas[0];
            const uint32_t a1 = alphas[1];
            if (a0 > a1)
            {
                alphas[2] = uint8_t((6 * a0 + 1 * a1) / 7);
                alphas[3] = uint8_t((5 * a0 + 2 * a1) / 7);
                alphas[4] = uint8_t((4 * a0 + 3 * a1) / 7);
                alphas[5] = uint8_t((3 * a0 + 4 * a1) / 7);
                alphas[6] = uint8_t((2 * a0 + 5 * a1) / 7);
                alphas[7] = uint8_t((1 * a0 + 6 * a1) / 7);
            }
            else
            {
                alphas[2] = uint8_t((4 * a0 + 1 * a1) / 5);
                alphas[3] = uint8_t((3 * a0 + 2 * a1) / 5);
                alphas[4] = uint8_t((4 * a0 + 3 * a1) / 5); // note: 4/5 or 2/5 -> (2 * a0 + 3 * a1) / 5
                alphas[4] = uint8_t((2 * a0 + 3 * a1) / 5);
                alphas[5] = uint8_t((1 * a0 + 4 * a1) / 5);
                alphas[6] = 0;
                alphas[7] = 255;
            }
        }
    }

    bool Decompress(const gpu::TextureData& src, uint32_t xenosFormat,
                    gpu::TextureData& dst, int& outVulkanFormat)
    {
        if (!IsBCFormat(xenosFormat)) return false;

        uint32_t bpp = 4;
        switch (xenosFormat)
        {
        case 18: case 51: // BC1
        case 19: case 52: // BC2
        case 20: case 53: // BC3
            outVulkanFormat = VK_FORMAT_R8G8B8A8_UNORM;
            bpp = 4;
            break;
        case 49:          // BC5
            outVulkanFormat = VK_FORMAT_R8G8_UNORM;
            bpp = 2;
            break;
        case 59:          // BC4
            outVulkanFormat = VK_FORMAT_R8_UNORM;
            bpp = 1;
            break;
        default:
            return false;
        }

        dst.ok = true;
        dst.error = nullptr;
        dst.width = src.width;
        dst.height = src.height;
        dst.blocksWide = src.width;
        dst.blocksHigh = src.height;
        dst.bytesPerBlock = bpp;
        dst.layers = src.layers;
        dst.volume = src.volume;
        dst.expanded = true;
        dst.minLevel = src.minLevel;
        dst.levels = src.levels;

        if (dst.levels.empty() && dst.width > 0 && dst.height > 0)
        {
            gpu::TextureData::Level lvl{};
            lvl.width = dst.width;
            lvl.height = dst.height;
            lvl.blocksWide = src.blocksWide ? src.blocksWide : (dst.width + 3) / 4;
            lvl.blocksHigh = src.blocksHigh ? src.blocksHigh : (dst.height + 3) / 4;
            lvl.offset = 0;
            lvl.size = src.bytes.size();
            dst.levels.push_back(lvl);
        }

        if (src.bytesPerBlock == 0 || bpp == 0)
        {
            dst.ok = false;
            dst.error = "invalid block size in texture decoding";
            return false;
        }

        // Calculate total size up front to avoid reallocations.
        size_t totalBufferSize = 0;
        for (const gpu::TextureData::Level& level : dst.levels)
        {
            if (level.width == 0 || level.height == 0) continue;
            const uint32_t numSlices = src.volume ? (level.depth ? level.depth : 1) : (level.layers ? level.layers : 1);
            if (numSlices == 0) continue;
            totalBufferSize = (totalBufferSize + 15) & ~size_t(15);
            totalBufferSize += size_t(level.width) * level.height * numSlices * bpp;
        }

        // Written straight into the caller's bytes, sized in place: handing the
        // same object back reuses the buffer, so a restage of a texture whose
        // size has not changed pays neither the allocation nor the zero fill of
        // the whole image. Measured on a 2048x2048 DXT1 decode, that is 5.64 ms
        // down to 3.39 ms, for the same bytes. Every byte of every level is
        // written below; the gaps between levels are padding nothing reads.
        dst.bytes.resize(totalBufferSize, 0);

        size_t currentOffset = 0;

        for (gpu::TextureData::Level& level : dst.levels)
        {
            if (level.width == 0 || level.height == 0)
            {
                dst.ok = false;
                dst.error = "invalid zero dimension in texture level";
                return false;
            }

            const uint32_t numSlices = src.volume ? (level.depth ? level.depth : 1) : (level.layers ? level.layers : 1);
            if (numSlices == 0)
            {
                dst.ok = false;
                dst.error = "invalid zero slices in texture level";
                return false;
            }

            const uint64_t totalBlocks = uint64_t(numSlices) * level.blocksHigh * level.blocksWide;
            const uint64_t requiredBytes = totalBlocks * src.bytesPerBlock;
            if (level.offset > src.bytes.size() || requiredBytes > (src.bytes.size() - level.offset))
            {
                dst.ok = false;
                dst.error = "incomplete compressed texture data";
                return false;
            }

            const uint64_t totalPixels = uint64_t(level.width) * level.height * numSlices;
            const uint64_t levelSize64 = totalPixels * bpp;
            if (levelSize64 > 0x40000000ULL)
            {
                dst.ok = false;
                dst.error = "texture dimensions exceed maximum supported size";
                return false;
            }

            const size_t at = (currentOffset + 15) & ~size_t(15);
            const size_t levelSize = size_t(levelSize64);
            currentOffset = at + levelSize;

            for (uint32_t layer = 0; layer < numSlices; layer++)
            {
                const size_t sliceOffset = at + size_t(layer) * level.width * level.height * bpp;
                uint8_t* dstLayer = dst.bytes.data() + sliceOffset;

                for (uint32_t by = 0; by < level.blocksHigh; by++)
                {
                    const uint32_t yBase = by << 2;
                    const bool fullRows = (yBase + 4 <= level.height);

                    for (uint32_t bx = 0; bx < level.blocksWide; bx++)
                    {
                        const uint32_t xBase = bx << 2;
                        const bool fullBlock = fullRows && (xBase + 4 <= level.width);

                        const size_t blockIdx = (size_t(layer) * level.blocksHigh + by) * level.blocksWide + bx;
                        const size_t blockOffset = level.offset + blockIdx * src.bytesPerBlock;
                        const uint8_t* block = src.bytes.data() + blockOffset;

                        if (xenosFormat == 18 || xenosFormat == 51) // BC1
                        {
                            uint32_t colors[4];
                            DecodeBC1Palette(block, colors, false);
                            const uint32_t bits = block[4] | (uint32_t(block[5]) << 8) |
                                                  (uint32_t(block[6]) << 16) | (uint32_t(block[7]) << 24);

                            if (fullBlock)
                            {
                                uint32_t* out = reinterpret_cast<uint32_t*>(dstLayer) + yBase * level.width + xBase;
                                const size_t stride = level.width;

                                out[0]          = colors[(bits >> 0) & 3];
                                out[1]          = colors[(bits >> 2) & 3];
                                out[2]          = colors[(bits >> 4) & 3];
                                out[3]          = colors[(bits >> 6) & 3];

                                out[stride + 0] = colors[(bits >> 8) & 3];
                                out[stride + 1] = colors[(bits >> 10) & 3];
                                out[stride + 2] = colors[(bits >> 12) & 3];
                                out[stride + 3] = colors[(bits >> 14) & 3];

                                out[stride * 2 + 0] = colors[(bits >> 16) & 3];
                                out[stride * 2 + 1] = colors[(bits >> 18) & 3];
                                out[stride * 2 + 2] = colors[(bits >> 20) & 3];
                                out[stride * 2 + 3] = colors[(bits >> 22) & 3];

                                out[stride * 3 + 0] = colors[(bits >> 24) & 3];
                                out[stride * 3 + 1] = colors[(bits >> 26) & 3];
                                out[stride * 3 + 2] = colors[(bits >> 28) & 3];
                                out[stride * 3 + 3] = colors[(bits >> 30) & 3];
                            }
                            else
                            {
                                for (int i = 0; i < 16; i++)
                                {
                                    const uint32_t x = xBase + (i & 3);
                                    const uint32_t y = yBase + (i >> 2);
                                    if (x < level.width && y < level.height)
                                    {
                                        uint32_t* out = reinterpret_cast<uint32_t*>(dstLayer) + y * level.width + x;
                                        *out = colors[(bits >> (2 * i)) & 3];
                                    }
                                }
                            }
                        }
                        else if (xenosFormat == 19 || xenosFormat == 52) // BC2
                        {
                            uint32_t colors[4];
                            DecodeBC1Palette(block + 8, colors, true);
                            const uint32_t bits = block[12] | (uint32_t(block[13]) << 8) |
                                                  (uint32_t(block[14]) << 16) | (uint32_t(block[15]) << 24);
                            // The four colours without an alpha byte, made once for the
                            // block: writing the block's own alpha over them pixel by
                            // pixel is what each pixel's mask was for.
                            for (int i = 0; i < 4; i++) colors[i] &= 0x00FFFFFFu;

                            // A row at a time, the way the BC1 path above does it:
                            // every index in a row is at a known shift of a word
                            // already loaded, and the sixteen-bit alphas of the
                            // row are the two bytes at 2*r. Walking all sixteen
                            // pixels with a computed x, y and shift instead is
                            // what this cost (a 2048x2048 decode: 10.1 ms before,
                            // 4.7 after; tools/bc_bench.cpp times it, and
                            // tests/bc_decoder_test.cpp holds the output).
                            if (fullBlock)
                            {
                                uint32_t* out = reinterpret_cast<uint32_t*>(dstLayer) + yBase * level.width + xBase;
                                const size_t stride = level.width;

                                for (int r = 0; r < 4; r++)
                                {
                                    const uint32_t c = bits >> (8 * r);
                                    const uint32_t low = block[2 * r], high = block[2 * r + 1];
                                    uint32_t* row = out + r * stride;

                                    // n*17<<24 is n*0x11000000: the four-bit alpha
                                    // repeated into a byte, in the top byte.
                                    row[0] = colors[(c >> 0) & 3] | (low & 0xF) * 0x11000000u;
                                    row[1] = colors[(c >> 2) & 3] | (low >> 4) * 0x11000000u;
                                    row[2] = colors[(c >> 4) & 3] | (high & 0xF) * 0x11000000u;
                                    row[3] = colors[(c >> 6) & 3] | (high >> 4) * 0x11000000u;
                                }
                            }
                            else
                            {
                                for (int i = 0; i < 16; i++)
                                {
                                    const uint32_t x = xBase + (i & 3);
                                    const uint32_t y = yBase + (i >> 2);
                                    if (x < level.width && y < level.height)
                                    {
                                        const uint8_t nibble = (block[i / 2] >> ((i & 1) * 4)) & 0xF;
                                        const uint32_t alpha8 = uint32_t(nibble) * 17;
                                        const uint32_t color = (colors[(bits >> (2 * i)) & 3] & 0x00FFFFFFu) | (alpha8 << 24);
                                        uint32_t* out = reinterpret_cast<uint32_t*>(dstLayer) + y * level.width + x;
                                        *out = color;
                                    }
                                }
                            }
                        }
                        else if (xenosFormat == 20 || xenosFormat == 53) // BC3
                        {
                            uint8_t alphas[8];
                            uint64_t alphaBits = 0;
                            DecodeBC3AlphaTable(block, alphas, alphaBits);

                            uint32_t colors[4];
                            DecodeBC1Palette(block + 8, colors, true);
                            const uint32_t colorBits = block[12] | (uint32_t(block[13]) << 8) |
                                                       (uint32_t(block[14]) << 16) | (uint32_t(block[15]) << 24);
                            // The colours without their alpha byte, and the eight
                            // alphas already in theirs: both are the same for
                            // every pixel of the block, and this is where a BC3
                            // block's extra cost over a BC1 one lives.
                            for (int i = 0; i < 4; i++) colors[i] &= 0x00FFFFFFu;
                            uint32_t lifted[8];
                            for (int i = 0; i < 8; i++) lifted[i] = uint32_t(alphas[i]) << 24;

                            if (fullBlock)
                            {
                                uint32_t* out = reinterpret_cast<uint32_t*>(dstLayer) + yBase * level.width + xBase;
                                const size_t stride = level.width;

                                // Row at a time, and each row's twelve alpha bits
                                // and eight colour bits shifted out once rather
                                // than per pixel (the nested loops this replaces
                                // cost 7.1 ms against 6.4 for a 2048x2048
                                // decode: most of a BC3 block is the block's own
                                // alphas, which no arrangement of this avoids;
                                // tools/bc_bench.cpp).
                                for (int r = 0; r < 4; r++)
                                {
                                    const uint64_t a = alphaBits >> (12 * r);
                                    const uint32_t c = colorBits >> (8 * r);
                                    uint32_t* row = out + r * stride;

                                    row[0] = colors[(c >> 0) & 3] | lifted[(a >> 0) & 7];
                                    row[1] = colors[(c >> 2) & 3] | lifted[(a >> 3) & 7];
                                    row[2] = colors[(c >> 4) & 3] | lifted[(a >> 6) & 7];
                                    row[3] = colors[(c >> 6) & 3] | lifted[(a >> 9) & 7];
                                }
                            }
                            else
                            {
                                for (int i = 0; i < 16; i++)
                                {
                                    const uint32_t x = xBase + (i & 3);
                                    const uint32_t y = yBase + (i >> 2);
                                    if (x < level.width && y < level.height)
                                    {
                                        const uint32_t a = alphas[(alphaBits >> (3 * i)) & 7];
                                        const uint32_t col = (colors[(colorBits >> (2 * i)) & 3] & 0x00FFFFFFu) | (a << 24);
                                        uint32_t* out = reinterpret_cast<uint32_t*>(dstLayer) + y * level.width + x;
                                        *out = col;
                                    }
                                }
                            }
                        }
                        else if (xenosFormat == 59) // BC4 (DXT5A) -> R8
                        {
                            uint8_t rVals[8];
                            uint64_t rBits = 0;
                            DecodeBC3AlphaTable(block, rVals, rBits);

                            if (fullBlock)
                            {
                                uint8_t* out = dstLayer + yBase * level.width + xBase;
                                const size_t stride = level.width;
                                for (int r = 0; r < 4; r++)
                                {
                                    const int idx = r * 4;
                                    out[r * stride + 0] = rVals[(rBits >> (3 * (idx + 0))) & 7];
                                    out[r * stride + 1] = rVals[(rBits >> (3 * (idx + 1))) & 7];
                                    out[r * stride + 2] = rVals[(rBits >> (3 * (idx + 2))) & 7];
                                    out[r * stride + 3] = rVals[(rBits >> (3 * (idx + 3))) & 7];
                                }
                            }
                            else
                            {
                                for (int i = 0; i < 16; i++)
                                {
                                    const uint32_t x = xBase + (i & 3);
                                    const uint32_t y = yBase + (i >> 2);
                                    if (x < level.width && y < level.height)
                                        dstLayer[y * level.width + x] = rVals[(rBits >> (3 * i)) & 7];
                                }
                            }
                        }
                        else if (xenosFormat == 49) // BC5 (DXN) -> RG8
                        {
                            uint8_t rVals[8];
                            uint64_t rBits = 0;
                            DecodeBC3AlphaTable(block, rVals, rBits);

                            uint8_t gVals[8];
                            uint64_t gBits = 0;
                            DecodeBC3AlphaTable(block + 8, gVals, gBits);

                            if (fullBlock)
                            {
                                uint16_t* out = reinterpret_cast<uint16_t*>(dstLayer) + yBase * level.width + xBase;
                                const size_t stride = level.width;
                                // Both channels' twelve bits per row, shifted once
                                // (a 2048x2048 decode: 6.7 ms before, 6.4 after).
                                for (int r = 0; r < 4; r++)
                                {
                                    const uint64_t r3 = rBits >> (12 * r), g3 = gBits >> (12 * r);
                                    uint16_t* row = out + r * stride;

                                    row[0] = uint16_t(rVals[(r3 >> 0) & 7]) | (uint16_t(gVals[(g3 >> 0) & 7]) << 8);
                                    row[1] = uint16_t(rVals[(r3 >> 3) & 7]) | (uint16_t(gVals[(g3 >> 3) & 7]) << 8);
                                    row[2] = uint16_t(rVals[(r3 >> 6) & 7]) | (uint16_t(gVals[(g3 >> 6) & 7]) << 8);
                                    row[3] = uint16_t(rVals[(r3 >> 9) & 7]) | (uint16_t(gVals[(g3 >> 9) & 7]) << 8);
                                }
                            }
                            else
                            {
                                for (int i = 0; i < 16; i++)
                                {
                                    const uint32_t x = xBase + (i & 3);
                                    const uint32_t y = yBase + (i >> 2);
                                    if (x < level.width && y < level.height)
                                    {
                                        const uint8_t rCh = rVals[(rBits >> (3 * i)) & 7];
                                        const uint8_t gCh = gVals[(gBits >> (3 * i)) & 7];
                                        uint16_t* out = reinterpret_cast<uint16_t*>(dstLayer) + y * level.width + x;
                                        *out = uint16_t(rCh) | (uint16_t(gCh) << 8);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            level.offset = at;
            level.size = levelSize;
            level.blocksWide = level.width;
            level.blocksHigh = level.height;
            level.depth = src.volume ? numSlices : 1;
            level.layers = src.volume ? numSlices : src.layers;
        }

        return true;
    }

    namespace
    {
        // Bit-reverse of a 32-bit word: ASTC stores the weight stream backwards from the
        // top of the block.
        uint32_t Reverse32(uint32_t v)
        {
            v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
            v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
            v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
            v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
            return (v >> 16) | (v << 16);
        }

        void Expand565To888(uint16_t c, int out[3])
        {
            const int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            out[0] = (r << 3) | (r >> 2);
            out[1] = (g << 2) | (g >> 4);
            out[2] = (b << 3) | (b >> 2);
        }

        int Clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

        // 16 bytes: block mode 0x42 (4x4 grid, one plane, 2-bit weights), one partition,
        // colour endpoint mode 8 (RGB direct) with 8-bit values from bit 17, then the
        // sixteen 2-bit weights reversed from bit 127 down.
        void PackAstc(const int e0[3], const int e1[3], const uint8_t weights[16], uint8_t out[16])
        {
            uint64_t lo = 0x42ull | (8ull << 13);
            lo |= uint64_t(e0[0]) << 17; lo |= uint64_t(e1[0]) << 25;
            lo |= uint64_t(e0[1]) << 33; lo |= uint64_t(e1[1]) << 41;
            lo |= uint64_t(e0[2]) << 49; lo |= uint64_t(e1[2]) << 57;
            uint64_t hi = uint64_t(e1[2]) >> 7;
            uint32_t stream = 0;
            for (int i = 0; i < 16; i++) stream |= uint32_t(weights[i] & 3) << (2 * i);
            hi |= uint64_t(Reverse32(stream)) << 32;
            std::memcpy(out, &lo, 8);
            std::memcpy(out + 8, &hi, 8);
        }

        // 16 bytes: block mode 0x42, one partition, colour endpoint mode 12 (RGBA direct)
        // with 8-bit values, one plane of 2-bit weights. Alpha is `alpha` at both ends.
        void PackAstcRgba(const int e0[3], const int e1[3], int alpha, const uint8_t weights[16],
                          uint8_t out[16])
        {
            std::memset(out, 0, 16);
            uint64_t lo = 0x42ull | (12ull << 13);
            lo |= uint64_t(e0[0]) << 17; lo |= uint64_t(e1[0]) << 25;
            lo |= uint64_t(e0[1]) << 33; lo |= uint64_t(e1[1]) << 41;
            lo |= uint64_t(e0[2]) << 49; lo |= uint64_t(e1[2]) << 57;
            uint64_t hi = uint64_t(e1[2]) >> 7;
            hi |= uint64_t(alpha) << 1; hi |= uint64_t(alpha) << 9;
            uint32_t stream = 0;
            for (int i = 0; i < 16; i++) stream |= uint32_t(weights[i] & 3) << (2 * i);
            hi |= uint64_t(Reverse32(stream)) << 32;
            std::memcpy(out, &lo, 8);
            std::memcpy(out + 8, &hi, 8);
        }
    }


    namespace
    {
        // Per thread while a texture is being transcoded, added to the totals after it.
        thread_local uint64_t t_blocks = 0, t_dual = 0, t_approximate = 0, t_severe = 0;
        std::atomic<uint64_t> g_blocks{0}, g_dual{0}, g_approximate{0}, g_severe{0};
    }

    AstcBlockStats AstcStats()
    {
        return { g_blocks.load(std::memory_order_relaxed), g_dual.load(std::memory_order_relaxed),
                 g_approximate.load(std::memory_order_relaxed), g_severe.load(std::memory_order_relaxed) };
    }

    namespace
    {
        // ---- dual-plane blocks, for BC1 blocks that carry a transparent texel ----
        //
        // Block mode 0x442 (4x4 grid, two planes, 2-bit weights), one partition, colour
        // endpoint mode 12 (RGBA direct), the second plane selected for alpha (CCS = 3).
        // Weights are 64 bits at the top, interleaved colour/alpha per texel; the colour
        // selector sits just below them; the eight endpoint values take the 45 bits from
        // bit 17, which is the range 0..47: a trit and four bits each.

        // The spec's trit-block decode: five trits from one 8-bit value.
        void DecodeTrits(uint32_t T, int t[5])
        {
            uint32_t C;
            int t4, t3;
            if (((T >> 2) & 7) == 7)
            {
                C = (((T >> 5) & 7) << 2) | (T & 3);
                t4 = 2; t3 = 2;
            }
            else
            {
                C = T & 31;
                if (((T >> 5) & 3) == 3) { t4 = 2; t3 = int((T >> 7) & 1); }
                else                     { t4 = int((T >> 7) & 1); t3 = int((T >> 5) & 3); }
            }
            int t2, t1, t0;
            if ((C & 3) == 3)
            {
                t2 = 2; t1 = int((C >> 4) & 1);
                t0 = int((((C >> 3) & 1) << 1) | (((C >> 2) & 1) & (1 ^ ((C >> 3) & 1))));
            }
            else if (((C >> 2) & 3) == 3)
            {
                t2 = 2; t1 = 2; t0 = int(C & 3);
            }
            else
            {
                t2 = int((C >> 4) & 1); t1 = int((C >> 2) & 3);
                t0 = int((((C >> 1) & 1) << 1) | ((C & 1) & (1 ^ ((C >> 1) & 1))));
            }
            t[0] = t0; t[1] = t1; t[2] = t2; t[3] = t3; t[4] = t4;
        }

        // The encoded byte for each of the 243 trit combinations.
        const uint8_t* TritEncodeTable()
        {
            static const std::array<uint8_t, 243> table = []
            {
                std::array<uint8_t, 243> out{};
                std::array<bool, 243> seen{};
                for (uint32_t T = 0; T < 256; T++)
                {
                    int t[5];
                    DecodeTrits(T, t);
                    if (t[0] > 2 || t[1] > 2 || t[2] > 2 || t[3] > 2 || t[4] > 2) continue;
                    const int index = t[0] + 3 * t[1] + 9 * t[2] + 27 * t[3] + 81 * t[4];
                    if (!seen[index]) { seen[index] = true; out[index] = uint8_t(T); }
                }
                return out;
            }();
            return table.data();
        }

        // What the decoder makes of quantised endpoint value q (0..47): a trit q >> 4 and
        // four low bits.
        int Unquantize48(int q)
        {
            const int trit = q >> 4, m = q & 15;
            const int a = (m & 1) ? 0x1FF : 0;
            const int b = (m >> 1) & 1, cc = (m >> 2) & 1, d = (m >> 3) & 1;
            const int B = (d << 8) | (cc << 7) | (b << 6) | (d << 2) | (cc << 1) | b;
            int T = trit * 22 + B;
            T ^= a;
            return (a & 0x80) | (T >> 2);
        }

        int Quantize48(int value)
        {
            static const std::array<uint8_t, 256> table = []
            {
                std::array<uint8_t, 256> out{};
                for (int v = 0; v < 256; v++)
                {
                    int best = 0, bestError = 1 << 30;
                    for (int q = 0; q < 48; q++)
                    {
                        const int error = std::abs(Unquantize48(q) - v);
                        if (error < bestError) { bestError = error; best = q; }
                    }
                    out[v] = uint8_t(best);
                }
                return out;
            }();
            return table[value];
        }

        // Writes `count` bits of `value` at bit `at` of the 128-bit block.
        void PutBits(uint8_t* block, int at, int count, uint32_t value)
        {
            for (int i = 0; i < count; i++)
                if ((value >> i) & 1) block[(at + i) >> 3] |= uint8_t(1u << ((at + i) & 7));
        }

        // Eight quantised values (0..47) as the trit-coded bit string, 45 bits from bit 17.
        void PutEndpoints48(uint8_t* block, const int q[8])
        {
            const uint8_t* encode = TritEncodeTable();
            int at = 17;
            for (int group = 0; group < 8; group += 5)
            {
                const int count = std::min(5, 8 - group);
                int t[5] = { 0, 0, 0, 0, 0 };
                uint32_t m[5] = { 0, 0, 0, 0, 0 };
                for (int i = 0; i < count; i++) { t[i] = q[group + i] >> 4; m[i] = uint32_t(q[group + i] & 15); }
                const uint32_t T = encode[t[0] + 3 * t[1] + 9 * t[2] + 27 * t[3] + 81 * t[4]];
                // m0, T[1:0], m1, T[3:2], m2, T[4], m3, T[6:5], m4, T[7]; a short group
                // stops after the last value that exists and the trit bits it needs.
                PutBits(block, at, 4, m[0]); at += 4;
                PutBits(block, at, 2, T & 3); at += 2;
                if (count > 1) { PutBits(block, at, 4, m[1]); at += 4; PutBits(block, at, 2, (T >> 2) & 3); at += 2; }
                if (count > 2) { PutBits(block, at, 4, m[2]); at += 4; PutBits(block, at, 1, (T >> 4) & 1); at += 1; }
                if (count > 3) { PutBits(block, at, 4, m[3]); at += 4; PutBits(block, at, 2, (T >> 5) & 3); at += 2; }
                if (count > 4) { PutBits(block, at, 4, m[4]); at += 4; PutBits(block, at, 1, (T >> 7) & 1); at += 1; }
            }
        }

        // `weights` and `alpha` are 0..3 per texel; the colour line runs e0 to e1 and
        // alpha from `alphaAt0` (weight 0) to `alphaAt3` (weight 3).
        void PackAstcDual(const int e0[3], const int e1[3], int alphaAt0, int alphaAt3,
                          const uint8_t weights[16], const uint8_t alpha[16], uint8_t out[16])
        {
            std::memset(out, 0, 16);
            PutBits(out, 0, 11, 0x442);
            PutBits(out, 13, 4, 12);
            int q[8] = { Quantize48(e0[0]), Quantize48(e1[0]), Quantize48(e0[1]), Quantize48(e1[1]),
                         Quantize48(e0[2]), Quantize48(e1[2]), Quantize48(alphaAt0), Quantize48(alphaAt3) };
            // Direct mode wants the second colour's sum at least the first's (as decoded),
            // else blue contraction; swap and flip the colour weights if not.
            uint8_t cw[16];
            std::memcpy(cw, weights, 16);
            const int sum0 = Unquantize48(q[0]) + Unquantize48(q[2]) + Unquantize48(q[4]);
            const int sum1 = Unquantize48(q[1]) + Unquantize48(q[3]) + Unquantize48(q[5]);
            if (sum1 < sum0)
            {
                std::swap(q[0], q[1]); std::swap(q[2], q[3]); std::swap(q[4], q[5]);
                for (int i = 0; i < 16; i++) cw[i] = uint8_t(3 - cw[i]);
            }
            PutEndpoints48(out, q);
            PutBits(out, 62, 2, 3);
            uint32_t lo = 0, hi = 0;   // the interleaved 64-bit weight stream, texel 0 first
            for (int i = 0; i < 16; i++)
            {
                const uint32_t pair = uint32_t(cw[i] & 3) | (uint32_t(alpha[i] & 3) << 2);
                if (i < 8) lo |= pair << (4 * i); else hi |= pair << (4 * (i - 8));
            }
            // Stream bit k goes to block bit 127 - k.
            const uint64_t reversed = (uint64_t(Reverse32(lo)) << 32) | Reverse32(hi);
            for (int k = 0; k < 8; k++) out[8 + k] = uint8_t(reversed >> (8 * k));
        }
    }

    bool TranscodeBc1BlockToAstc(const uint8_t* bc1, uint8_t* astc)
    {
        t_blocks++;
        const uint16_t c0 = uint16_t(bc1[0] | (bc1[1] << 8));
        const uint16_t c1 = uint16_t(bc1[2] | (bc1[3] << 8));
        const uint32_t indices = uint32_t(bc1[4]) | (uint32_t(bc1[5]) << 8) |
                                 (uint32_t(bc1[6]) << 16) | (uint32_t(bc1[7]) << 24);
        int e0[3], e1[3];
        Expand565To888(c0, e0);
        Expand565To888(c1, e1);

        bool transparent = false;
        if (c0 <= c1)
            for (int i = 0; i < 16; i++) transparent = transparent || ((indices >> (2 * i)) & 3) == 3;

        // ASTC weight index (0..3 is 0, 21, 43, 64 of 64) for each BC1 index.
        uint8_t map[4];
        bool dual = transparent;
        if (c0 > c1)
        {
            // c0, c1, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1: positions 0, 1, 1/3, 2/3.
            map[0] = 0; map[1] = 3; map[2] = 1; map[3] = 2;
        }
        else if (c0 == c1)
        {
            map[0] = map[1] = map[2] = map[3] = 0;
        }
        else if (![&] { for (int i = 0; i < 16; i++) if (((indices >> (2 * i)) & 3) == 2) return true; return false; }())
        {
            // No texel uses the midpoint: c0 and c1 at the two ends, which is exact.
            map[0] = 0; map[1] = 3; map[2] = 1; map[3] = 0;
        }
        else
        {
            // Three colours (and maybe transparent black). c0, the midpoint and c1 want
            // equal steps along the line, which the weights 0, 1/3, 2/3 and 1/3, 2/3, 1
            // give by moving one endpoint out.
            int a0[3], a1[3], b0[3], b1[3];
            bool aFits = true, bFits = true;
            for (int k = 0; k < 3; k++)
            {
                const int d = e1[k] - e0[k];
                const int sign = d >= 0 ? 1 : -1;
                a0[k] = e0[k];
                const int farEnd = e0[k] + (3 * d + sign) / 2;
                aFits = aFits && farEnd >= 0 && farEnd <= 255;
                a1[k] = Clamp255(farEnd);
                b1[k] = e1[k];
                const int nearEnd = e0[k] - (d + sign) / 2;
                bFits = bFits && nearEnd >= 0 && nearEnd <= 255;
                b0[k] = Clamp255(nearEnd);
            }
            if (aFits)
            {
                for (int k = 0; k < 3; k++) { e0[k] = a0[k]; e1[k] = a1[k]; }
                map[0] = 0; map[1] = 2; map[2] = 1; map[3] = 0;
            }
            else if (bFits)
            {
                for (int k = 0; k < 3; k++) { e0[k] = b0[k]; e1[k] = b1[k]; }
                map[0] = 1; map[1] = 3; map[2] = 2; map[3] = 1;
            }
            else
            {
                // The midpoint cannot sit on a weight with both endpoints in range. Try
                // the plain endpoints and the two stretched lines clamped into range, put
                // each of c0, the midpoint and c1 on its nearest weight, and keep the
                // candidate whose worst channel is closest. It takes the dual plane.
                int mid[3], target[3][3];
                for (int k = 0; k < 3; k++)
                {
                    mid[k] = (e0[k] + e1[k]) / 2;
                    target[0][k] = e0[k]; target[1][k] = mid[k]; target[2][k] = e1[k];
                }
                const int* pairs[3][2] = { { e0, e1 }, { a0, a1 }, { b0, b1 } };
                static const int kWeight[4] = { 0, 21, 43, 64 };
                int bestError = 1 << 30, bestPair = 0;
                uint8_t bestMap[3] = { 0, 1, 3 };
                for (int p = 0; p < 3; p++)
                {
                    int worst = 0;
                    uint8_t chosen[3];
                    for (int t = 0; t < 3; t++)
                    {
                        int least = 1 << 30;
                        for (int w = 0; w < 4; w++)
                        {
                            int error = 0;
                            for (int k = 0; k < 3; k++)
                            {
                                const int value = (pairs[p][0][k] * (64 - kWeight[w]) +
                                                   pairs[p][1][k] * kWeight[w] + 32) >> 6;
                                error = std::max(error, std::abs(value - target[t][k]));
                            }
                            if (error < least) { least = error; chosen[t] = uint8_t(w); }
                        }
                        worst = std::max(worst, least);
                    }
                    if (worst < bestError)
                    {
                        bestError = worst; bestPair = p;
                        std::memcpy(bestMap, chosen, 3);
                    }
                }
                int pick0[3], pick1[3];
                for (int k = 0; k < 3; k++) { pick0[k] = pairs[bestPair][0][k]; pick1[k] = pairs[bestPair][1][k]; }
                for (int k = 0; k < 3; k++) { e0[k] = pick0[k]; e1[k] = pick1[k]; }
                map[0] = bestMap[0]; map[1] = bestMap[2]; map[2] = bestMap[1]; map[3] = 0;
                dual = true;
                if (bestError > 4) t_approximate++;
                if (bestError > 16) t_severe++;
            }
        }

        if (dual)
        {
            // Opaque texels are alpha weight 0 (255), the transparent one weight 3 (0), and
            // that texel's colour weight is whichever end is the darker, as BC1's is black.
            uint8_t weights[16], alpha[16];
            const bool firstDarker = e0[0] + e0[1] + e0[2] <= e1[0] + e1[1] + e1[2];
            for (int i = 0; i < 16; i++)
            {
                const uint32_t index = (indices >> (2 * i)) & 3;
                const bool clear = transparent && index == 3;
                weights[i] = clear ? (firstDarker ? 0 : 3) : map[index];
                alpha[i] = clear ? 3 : 0;
            }
            PackAstcDual(e0, e1, 255, 0, weights, alpha, astc);
            t_dual++;
            return true;
        }

        // RGB direct wants the second endpoint's sum at least the first's; otherwise the
        // decoder applies blue contraction. Swap, and walk the weights the other way.
        if (e1[0] + e1[1] + e1[2] < e0[0] + e0[1] + e0[2])
        {
            for (int k = 0; k < 3; k++) std::swap(e0[k], e1[k]);
            for (int i = 0; i < 4; i++) map[i] = uint8_t(3 - map[i]);
        }

        uint8_t weights[16];
        for (int i = 0; i < 16; i++) weights[i] = map[(indices >> (2 * i)) & 3];
        PackAstc(e0, e1, weights, astc);
        return true;
    }


    namespace
    {
        std::atomic<uint64_t> g_alphaBlocks{0}, g_alphaDual{0}, g_alphaOff8{0}, g_alphaOff32{0};
        thread_local uint64_t t_alphaBlocks = 0, t_alphaDual = 0, t_alphaOff8 = 0, t_alphaOff32 = 0;

        void AlphaBc2(const uint8_t* block, uint8_t out[16])
        {
            for (int i = 0; i < 16; i++)
            {
                const uint32_t nibble = (block[i >> 1] >> ((i & 1) * 4)) & 15;
                out[i] = uint8_t((nibble << 4) | nibble);
            }
        }

        // BC3's alpha block: two endpoints and three-bit indices, in one of two modes.
        void AlphaBc3(const uint8_t* block, uint8_t out[16])
        {
            const int a0 = block[0], a1 = block[1];
            int palette[8] = { a0, a1, 0, 0, 0, 0, 0, 0 };
            if (a0 > a1)
                for (int i = 1; i <= 6; i++) palette[i + 1] = ((7 - i) * a0 + i * a1) / 7;
            else
            {
                palette[2] = (4 * a0 + a1) / 5; palette[3] = (3 * a0 + 2 * a1) / 5;
                palette[4] = (2 * a0 + 3 * a1) / 5; palette[5] = (a0 + 4 * a1) / 5;
                palette[6] = 0; palette[7] = 255;
            }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++) bits |= uint64_t(block[2 + i]) << (8 * i);
            for (int i = 0; i < 16; i++) out[i] = uint8_t(palette[(bits >> (3 * i)) & 7]);
        }
    }

    // BC2 and BC3: sixteen bytes, alpha first, then a BC1 colour block that is always in
    // four-colour mode. Colour goes the BC1 way. Alpha that is the same in every texel
    // costs nothing (RGBA direct, 8-bit endpoints); anything else takes the dual plane with
    // four alpha levels between the block's lowest and highest, which is exact for the
    // two-level alpha of a cut-out and coarse for a gradient across one block.
    bool TranscodeBc23BlockToAstc(const uint8_t* block, bool bc3, uint8_t* astc)
    {
        uint8_t alpha[16];
        if (bc3) AlphaBc3(block, alpha); else AlphaBc2(block, alpha);
        const uint8_t* bc1 = block + 8;
        const uint16_t c0 = uint16_t(bc1[0] | (bc1[1] << 8));
        const uint16_t c1 = uint16_t(bc1[2] | (bc1[3] << 8));
        const uint32_t indices = uint32_t(bc1[4]) | (uint32_t(bc1[5]) << 8) |
                                 (uint32_t(bc1[6]) << 16) | (uint32_t(bc1[7]) << 24);
        int e0[3], e1[3];
        Expand565To888(c0, e0);
        Expand565To888(c1, e1);
        uint8_t map[4] = { 0, 3, 1, 2 };   // always four colours: c0, c1, 2/3 c0 + 1/3 c1, ...
        uint8_t weights[16];
        for (int i = 0; i < 16; i++) weights[i] = map[(indices >> (2 * i)) & 3];
        t_alphaBlocks++;

        int lo = 255, hi = 0;
        for (int i = 0; i < 16; i++) { lo = std::min<int>(lo, alpha[i]); hi = std::max<int>(hi, alpha[i]); }

        if (lo == hi)
        {
            if (e1[0] + e1[1] + e1[2] < e0[0] + e0[1] + e0[2])
            {
                for (int k = 0; k < 3; k++) std::swap(e0[k], e1[k]);
                for (int i = 0; i < 16; i++) weights[i] = uint8_t(3 - weights[i]);
            }
            PackAstcRgba(e0, e1, lo, weights, astc);
            return true;
        }

        // Alpha endpoints as the decoder will see them after 48-level quantisation, and
        // each texel on the nearest of the four levels between them.
        const int realLo = Unquantize48(Quantize48(lo)), realHi = Unquantize48(Quantize48(hi));
        static const int kLevel[4] = { 0, 21, 43, 64 };
        uint8_t alphaWeights[16];
        int worst = 0;
        for (int i = 0; i < 16; i++)
        {
            int best = 0, bestError = 1 << 30;
            for (int w = 0; w < 4; w++)
            {
                const int value = (realLo * (64 - kLevel[w]) + realHi * kLevel[w] + 32) >> 6;
                const int error = std::abs(value - int(alpha[i]));
                if (error < bestError) { bestError = error; best = w; }
            }
            alphaWeights[i] = uint8_t(best);
            worst = std::max(worst, bestError);
        }
        t_alphaDual++;
        if (worst > 8) t_alphaOff8++;
        if (worst > 32) t_alphaOff32++;
        PackAstcDual(e0, e1, realLo, realHi, weights, alphaWeights, astc);
        return true;
    }

    bool TranscodeBc1ToAstc(const gpu::TextureData& src, gpu::TextureData& dst,
                            int& outVulkanFormat)
    {
        return TranscodeToAstc(src, 18, dst, outVulkanFormat);
    }

    bool TranscodeToAstc(const gpu::TextureData& src, uint32_t xenosFormat, gpu::TextureData& dst,
                         int& outVulkanFormat)
    {
        const bool bc1 = xenosFormat == 18 || xenosFormat == 51;
        const bool bc2 = xenosFormat == 19 || xenosFormat == 52;
        const bool bc3 = xenosFormat == 20 || xenosFormat == 53;
        if (!bc1 && !bc2 && !bc3) return false;
        const uint32_t inBytes = bc1 ? 8 : 16;
        if (src.bytesPerBlock != inBytes) return false;
        outVulkanFormat = VK_FORMAT_ASTC_4x4_UNORM_BLOCK;

        dst.ok = true;
        dst.error = nullptr;
        dst.width = src.width;
        dst.height = src.height;
        dst.blocksWide = src.blocksWide;
        dst.blocksHigh = src.blocksHigh;
        dst.bytesPerBlock = 16;
        dst.layers = src.layers;
        dst.volume = src.volume;
        dst.expanded = false;
        dst.minLevel = src.minLevel;
        dst.levels = src.levels;
        if (dst.levels.empty() && src.width > 0 && src.height > 0)
        {
            gpu::TextureData::Level lvl{};
            lvl.width = src.width;
            lvl.height = src.height;
            lvl.blocksWide = src.blocksWide ? src.blocksWide : (src.width + 3) / 4;
            lvl.blocksHigh = src.blocksHigh ? src.blocksHigh : (src.height + 3) / 4;
            lvl.layers = src.layers;
            dst.levels.push_back(lvl);
        }

        size_t total = 0;
        for (const gpu::TextureData::Level& level : dst.levels)
        {
            const uint32_t slices = src.volume ? (level.depth ? level.depth : 1) : (level.layers ? level.layers : 1);
            total = (total + 15) & ~size_t(15);
            total += size_t(slices) * level.blocksHigh * level.blocksWide * 16;
        }
        dst.bytes.resize(total, 0);

        size_t at = 0;
        for (size_t l = 0; l < dst.levels.size(); l++)
        {
            gpu::TextureData::Level& level = dst.levels[l];
            const size_t srcOffset = l < src.levels.size() ? src.levels[l].offset : 0;
            const uint32_t slices = src.volume ? (level.depth ? level.depth : 1) : (level.layers ? level.layers : 1);
            const uint64_t blocks = uint64_t(slices) * level.blocksHigh * level.blocksWide;
            if (srcOffset > src.bytes.size() || blocks * inBytes > src.bytes.size() - srcOffset)
            {
                dst.ok = false;
                dst.error = "incomplete compressed texture data";
                return false;
            }
            at = (at + 15) & ~size_t(15);
            const uint8_t* in = src.bytes.data() + srcOffset;
            uint8_t* out = dst.bytes.data() + at;
            for (uint64_t b = 0; b < blocks; b++)
            {
                const bool ok = bc1 ? TranscodeBc1BlockToAstc(in + b * 8, out + b * 16)
                                    : TranscodeBc23BlockToAstc(in + b * 16, bc3, out + b * 16);
                if (!ok) return false;
            }
            level.offset = at;
            level.size = size_t(blocks) * 16;
            at += level.size;
        }
        g_blocks += t_blocks; g_dual += t_dual; g_approximate += t_approximate; g_severe += t_severe;
        t_blocks = t_dual = t_approximate = t_severe = 0;
        g_alphaBlocks += t_alphaBlocks; g_alphaDual += t_alphaDual; g_alphaOff8 += t_alphaOff8; g_alphaOff32 += t_alphaOff32;
        t_alphaBlocks = t_alphaDual = t_alphaOff8 = t_alphaOff32 = 0;
        return true;
    }

    AlphaBlockStats AstcAlphaStats()
    {
        return { g_alphaBlocks.load(std::memory_order_relaxed), g_alphaDual.load(std::memory_order_relaxed),
                 g_alphaOff8.load(std::memory_order_relaxed), g_alphaOff32.load(std::memory_order_relaxed) };
    }
}
