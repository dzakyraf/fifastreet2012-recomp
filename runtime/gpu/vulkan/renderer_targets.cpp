// EDRAM surfaces as Vulkan images: making them, reading them back, the
// rendering instances they are drawn in, and clearing them.
#include "renderer_state.h"
#include "../../log.h"

#include <algorithm>
#include <atomic>

#ifdef PES17_HAVE_VULKAN

namespace vk::renderer::detail
{
    bool MakeImage(Target& target, VkImageUsageFlags usage, VkImageAspectFlags aspect)
    {
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = target.format;
        info.extent = { HostOf(target.width, target.scale), HostOf(target.height, target.scale), 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = target.samples;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(g.device, &info, nullptr, &target.image) != VK_SUCCESS)
        {
            target.image = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(g.device, target.image, &needs);
        const uint32_t type = FindMemory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX)
        {
            vkDestroyImage(g.device, target.image, nullptr);
            target.image = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type;
        if (vkAllocateMemory(g.device, &allocate, nullptr, &target.memory) != VK_SUCCESS)
        {
            vkDestroyImage(g.device, target.image, nullptr);
            target.image = VK_NULL_HANDLE;
            target.memory = VK_NULL_HANDLE;
            return false;
        }
        if (vkBindImageMemory(g.device, target.image, target.memory, 0) != VK_SUCCESS)
        {
            vkFreeMemory(g.device, target.memory, nullptr);
            vkDestroyImage(g.device, target.image, nullptr);
            target.image = VK_NULL_HANDLE;
            target.memory = VK_NULL_HANDLE;
            return false;
        }

        if (aspect)
        {
            VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view.image = target.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = target.format;
            view.subresourceRange = { aspect, 0, 1, 0, 1 };
            if (vkCreateImageView(g.device, &view, nullptr, &target.view) != VK_SUCCESS)
            {
                target.view = VK_NULL_HANDLE;
                vkFreeMemory(g.device, target.memory, nullptr);
                vkDestroyImage(g.device, target.image, nullptr);
                target.image = VK_NULL_HANDLE;
                target.memory = VK_NULL_HANDLE;
                return false;
            }
        }
        return true;
    }

    VkFormat ColourFormatFor(uint32_t format)
    {
        switch (format)
        {
        case 0: case 1: case 10: return VK_FORMAT_R8G8B8A8_UNORM;   // 8_8_8_8 and gamma
        case 2:  return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        // 2_10_10_10_FLOAT is the console's 7e3 HDR format: floats up to about
        // 32. A UNORM target clamps them to 1, and a scene lit for that range --
        // PES 2017's, tone-mapped from a luminance chain -- comes out black.
        // Kept as half floats, as Xenia keeps it; resolves inherit the format.
        case 3: case 12: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 4:  return VK_FORMAT_R16G16_SNORM;
        case 5:  return VK_FORMAT_R16G16B16A16_SNORM;
        case 6:  return VK_FORMAT_R16G16_SFLOAT;
        case 7:  return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 14: return VK_FORMAT_R32_SFLOAT;
        case 15: return VK_FORMAT_R32G32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
        }
    }

    // EDRAM has one depth buffer, not one per way of describing it. A pass that
    // writes depth at 1040 wide without multisampling and a pass that tests it at
    // 1040 wide with two samples are the same tiles on the hardware; keeping an
    // image per (pitch, samples) gives the second pass a buffer the first never
    // wrote, and MW2 draws its whole world in the second pass. So the pitch is
    // not part of a depth target's key.
    //
    // With the surfaces drawn multisampled the sample count stays in the key:
    // a depth image has one sample count, and the colour target it is bound
    // beside has to share it. MW2 tests and writes its world depth at 2x in
    // both passes, so nothing is split by that.
    TargetKey DepthKey(uint32_t baseTile, uint32_t format, uint32_t samples)
    {
        return { baseTile, 0, format, g.msaa ? samples : 0, true };
    }

    // The console's 1x, 2x and 4x, as the device can draw them. A count the
    // device lacks is rounded up to the next one it has; 1x is the last resort.
    //
    // PES17_MSAA=<n> draws every surface the title multisamples at n instead --
    // 8 on most devices -- for seeing that it works: at the console's own 2x,
    // upscaled from 1024x600 to the window with a linear blit, the difference is
    // slight. A count past what the device offers is rounded down to what it has.
    VkSampleCountFlagBits HostSamples(uint32_t guestSamples)
    {
        if (!g.msaa) return VK_SAMPLE_COUNT_1_BIT;
        static const uint32_t forced = uint32_t(env::Number("PES17_MSAA"));
        if (forced > 1 && guestSamples)
        {
            for (uint32_t bit = 64; bit > 1; bit >>= 1)
                if (bit <= forced && (g.sampleCounts & bit)) return VkSampleCountFlagBits(bit);
        }
        for (uint32_t bit = 1u << (guestSamples & 3); bit <= 64; bit <<= 1)
            if (g.sampleCounts & bit) return VkSampleCountFlagBits(bit);
        return VK_SAMPLE_COUNT_1_BIT;
    }

    // Besides the clears a resolve asks for (ClearAfterResolve), MW2 clears
    // EDRAM by drawing:
    // it points the surface at a base with a pitch and height that span the rest
    // of the memory and draws rectangles over all of it with the depth test
    // ALWAYS and depth writes on. The rectangles are in EDRAM's own coordinates
    // -- the viewport transform is switched off for them -- and they come in
    // pairs, because a tile is 80 samples wide and a surface is not a whole
    // number of tiles: one rectangle covers the columns that divide evenly, a
    // second, describing the same memory at a different pitch and sample count,
    // covers the column left over. Together they name one span of tiles from the
    // surface's base upwards, which is where every depth buffer the frame is
    // about to draw against lives.
    //
    // Here each (base, pitch, samples) is an image of its own, so a rectangle
    // covers whatever part of one image its own coordinates happen to fall in
    // and nothing else. So the idiom is recognised and answered with clears of
    // the images it names, and the rectangles themselves are not drawn. It is not
    // a model of EDRAM, but it is what this particular idiom means.

    void ClearAliasedDepth(uint32_t fromTile, uint32_t endTile, float depth, bool clearStencil, uint32_t stencilValue)
    {
        bool ended = false;
        for (auto& [key, target] : g.targets)
        {
            // Only the surfaces that start inside the tiles the rectangle covers.
            // PES 2017 clears tiles 0-719 to zero before its crowd pass, whose
            // depth buffer starts at tile 1328 and still holds the depth the
            // crowd is tested against; clearing that too left one row of the
            // crowd standing.
            if (!target.depth || key.baseTile < fromTile || key.baseTile >= endTile) continue;
            // Nothing has been drawn since the last time this was cleared, so
            // there is nothing to clear. The idiom arrives as a pair of
            // rectangles covering one tile span between them, and each pass
            // clears before it draws, so without this every target is cleared
            // several times a frame for nothing.
            if (target.clearedAtDraw == g.drawsRecorded) continue;
            if (!ended) { EndPass(); ended = true; }
            const VkImageAspectFlags aspect =
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            RecordBarrier(target.image, aspect,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          kDepthStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
            // The depth the rectangles write: MW2's are at 0 (its levels test
            // GEQUAL), PES 2017's at 1 (it tests LESS). A fixed 0 left every
            // PES depth buffer at the near plane, nothing passed, and the 3D
            // view was the one full-screen pass that tests no depth: white.
            // The stencil goes only when the rectangle writes it: a clear drawn with the stencil test
            // off leaves the stencil bits of the word alone on the console, and PES 2017's crowd
            // marks its pixels in stencil in one pass and tests the mark two passes later, with
            // exactly such a depth clear in between (the whole crowd was drawn against a zeroed mask).
            const VkClearDepthStencilValue value{ std::clamp(depth, 0.0f, 1.0f), stencilValue };
            const VkImageSubresourceRange range{ clearStencil ? aspect : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT),
                                                 0, 1, 0, 1 };
            const VkImage image = target.image;
            Record([=](VkCommandBuffer command) {
                vkCmdClearDepthStencilImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            &value, 1, &range);
            });
            RecordBarrier(target.image, aspect,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, kDepthAccess,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, kDepthStages);
            target.clearedAtDraw = g.drawsRecorded;
            target.depthClearedIn = g.frames;
        }
    }

    // A clearing rectangle that replaces the stencil clears it in every depth image at the same tile, not
    // only the one it is drawn into: EDRAM holds one depth/stencil buffer, and here a 1x and a 4x image
    // of it are two. PES 2017's crowd marks its pixels in the 1x image's stencil, the interface then clears
    // through the 4x one, and its sprites test the stencil for EQUAL 1 -- with the marks left in the 1x
    // image they failed wherever there was crowd behind them (a half-time board with the crowd drawn
    // through its text).
    void ClearAliasedStencil(const Target& except, uint32_t baseTile, uint32_t value)
    {
        bool ended = false;
        for (auto& [key, target] : g.targets)
        {
            if (!target.depth || key.baseTile != baseTile || target.image == except.image) continue;
            if (g_dumpDraws) LOGW("DUMP   stencil of the %ux depth image at tile %u cleared to %u along with the clear's own image", unsigned(target.samples), baseTile, value);
            if (!ended) { EndPass(); ended = true; }
            const VkImageAspectFlags both = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            RecordBarrier(target.image, both,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          kDepthStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
            const VkClearDepthStencilValue clear{ 0.0f, value };
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
            const VkImage image = target.image;
            Record([=](VkCommandBuffer command) {
                vkCmdClearDepthStencilImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            });
            RecordBarrier(target.image, both,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, kDepthAccess,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, kDepthStages);
        }
    }

    // EDRAM holds colour and depth in the same memory, and a surface is named
    // by the tile it starts at whatever it holds. A depth clear to zero at a
    // tile leaves zero in every byte there, which is what a colour surface at
    // that tile reads afterwards: PES 2017 clears its depth at tile 0 and then
    // draws its player-shadow mask -- a MAX blend into the red channel -- over
    // the colour surface at tile 0, counting on it being black. Here colour and
    // depth are images of their own, so the colour ones at the tile are cleared
    // too. Only for a zero clear: the bytes of any other depth value read as a
    // colour would need the format's packing worked out.
    void ClearAliasedColour(uint32_t tile)
    {
        bool ended = false;
        for (auto& [key, target] : g.targets)
        {
            if (target.depth || key.baseTile != tile || !target.image) continue;
            if (!ended) { EndPass(); ended = true; }
            const VkClearColorValue value{};
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          kColourAccess, VK_ACCESS_TRANSFER_WRITE_BIT,
                          kColourStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
            const VkImage image = target.image;
            Record([=](VkCommandBuffer command) {
                vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
            });
            RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, kColourAccess,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, kColourStages);
        }
    }

    Target* FindTarget(const TargetKey& key)
    {
        auto found = g.targets.find(key);
        return found == g.targets.end() ? nullptr : &found->second;
    }

    void DestroySingle(Target& target)
    {
        if (target.singleView) vkDestroyImageView(g.device, target.singleView, nullptr);
        if (target.singleImage) vkDestroyImage(g.device, target.singleImage, nullptr);
        if (target.singleMemory) vkFreeMemory(g.device, target.singleMemory, nullptr);
        target.singleView = VK_NULL_HANDLE;
        target.singleImage = VK_NULL_HANDLE;
        target.singleMemory = VK_NULL_HANDLE;
        target.singleReady = false;
    }

    void DestroyTarget(Target& target)
    {
        InvalidateFramebuffers();
        if (target.view) vkDestroyImageView(g.device, target.view, nullptr);
        if (target.image) vkDestroyImage(g.device, target.image, nullptr);
        if (target.memory) vkFreeMemory(g.device, target.memory, nullptr);
        DestroySingle(target);
    }

    // The single-sampled twin of a multisampled target, made on first use. A
    // colour twin is a transfer destination the resolve fills; a depth twin is
    // an attachment, because a depth resolve only happens inside a render pass.
    bool EnsureSingle(Target& target)
    {
        if (target.singleImage) return true;
        Target one = target;
        one.samples = VK_SAMPLE_COUNT_1_BIT;
        one.image = VK_NULL_HANDLE;
        one.memory = VK_NULL_HANDLE;
        one.view = VK_NULL_HANDLE;
        const VkImageUsageFlags usage = target.depth
            ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
            : (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        // A view only for the depth twin, which is bound as an attachment; a
        // transfer-only image may not have one.
        const VkImageAspectFlags aspect = target.depth
            ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : 0;
        if (!MakeImage(one, usage, aspect))
        {
            if (one.view) vkDestroyImageView(g.device, one.view, nullptr);
            if (one.image) vkDestroyImage(g.device, one.image, nullptr);
            if (one.memory) vkFreeMemory(g.device, one.memory, nullptr);
            return false;
        }
        target.singleImage = one.image;
        target.singleMemory = one.memory;
        target.singleView = one.view;
        target.singleReady = false;
        return true;
    }

    // What a copy or blit out of a colour target reads, in TRANSFER_SRC. For a
    // single-sampled target that is the target itself; for a multisampled one
    // the rectangle is resolved into the twin first. Outside any render pass.
    VkImage BeginReadingColour(Target& target, VkOffset2D at, uint32_t width, uint32_t height)
    {
        if (target.samples == VK_SAMPLE_COUNT_1_BIT)
        {
            RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          kColourStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
            return target.image;
        }
        if (!EnsureSingle(target)) return VK_NULL_HANDLE;
        RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      kColourStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
        // From UNDEFINED: only the rectangle is read afterwards. After the
        // copies out of the twin's last contents.
        RecordBarrier(target.singleImage, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageResolve region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.srcOffset = Scaled(at.x, at.y, 0, target.scale);
        region.dstSubresource = region.srcSubresource;
        region.dstOffset = region.srcOffset;
        region.extent = { HostOf(width, target.scale), HostOf(height, target.scale), 1 };
        const VkImage from = target.image, into = target.singleImage;
        Record([=](VkCommandBuffer command) {
            vkCmdResolveImage(command, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              into, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        });
        RecordBarrier(target.singleImage, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, kColourAccess,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, kColourStages);
        target.singleReady = true;
        return target.singleImage;
    }

    void EndReadingColour(Target& target)
    {
        if (target.samples != VK_SAMPLE_COUNT_1_BIT) return;   // never left the attachment layout
        RecordBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, kColourAccess,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, kColourStages);
    }

    // The same for a depth target, whose resolve is a render pass over the
    // rectangle with nothing drawn in it.
    VkImage BeginReadingDepth(Target& target, VkOffset2D at, uint32_t width, uint32_t height)
    {
        constexpr VkImageAspectFlags kBoth = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (target.samples == VK_SAMPLE_COUNT_1_BIT)
        {
            RecordBarrier(target.image, kBoth,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          kDepthStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
            return target.image;
        }
        if (!EnsureSingle(target)) return VK_NULL_HANDLE;
        // A rendering instance with nothing drawn in it, which loads the
        // multisampled depth and resolves it into the twin: a transfer cannot.
        // The first sample, as the console's own depth resolve takes one
        // sample rather than an average -- an averaged depth is a surface
        // nothing was drawn at. The target stays an attachment throughout, and
        // the twin comes out in TRANSFER_SRC, ready to be copied from.
        const VkImageView view = target.view, twinView = target.singleView;
        const VkImage twin = target.singleImage;
        const VkRect2D area{ Scaled(at, target.scale), Scaled(VkExtent2D{ width, height }, target.scale) };
        Record([=](VkCommandBuffer command) {
            constexpr VkImageAspectFlags kBoth = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            // The draws before it wrote the depth; the copies before it read the twin.
            VkMemoryBarrier written{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            written.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            written.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            VkImageMemoryBarrier into{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            into.srcAccessMask = 0;
            into.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            into.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            into.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            into.srcQueueFamilyIndex = into.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            into.image = twin;
            into.subresourceRange = { kBoth, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(command, kDepthStages | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 kDepthStages | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 1, &written, 0, nullptr, 1, &into);

            VkRenderingAttachmentInfoKHR depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
            depth.imageView = view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            depth.resolveImageView = twinView;
            depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfoKHR info{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
            info.renderArea = area;
            info.layerCount = 1;
            info.pDepthAttachment = &depth;
            info.pStencilAttachment = &depth;
            if (dispatch.beginRendering && dispatch.endRendering)
            {
                dispatch.beginRendering(command, &info);
                dispatch.endRendering(command);
            }

            // A resolve writes at the colour output stage whatever it
            // resolves; the copy after it reads the twin.
            VkImageMemoryBarrier out = into;
            out.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            out.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            out.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            out.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(command,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthStages,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &out);
        });
        target.singleReady = true;
        return target.singleImage;
    }

    void EndReadingDepth(Target& target)
    {
        if (target.samples != VK_SAMPLE_COUNT_1_BIT) return;
        RecordBarrier(target.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, kDepthAccess,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, kDepthStages);
    }

    Target* EnsureTarget(const TargetKey& key, uint32_t width, uint32_t height,
                         uint32_t guestFormat)
    {
        // Up to 4096 rows: PES 2017's crowd atlas is 3328 tall (renderer_draw.cpp).
        width = std::min(width, 2048u);
        height = std::min(height, 4096u);
        auto found = g.targets.find(key);
        if (found != g.targets.end() &&
            found->second.width >= width && found->second.height >= height)
            return &found->second;

        if (found != g.targets.end())
        {
            // The surface grew. Rare enough to rebuild rather than plan for.
            RetireBeforeDestroy();
            DestroyTarget(found->second);
            width = std::min(2048u, std::max(width, found->second.width));
            height = std::min(4096u, std::max(height, found->second.height));
            g.targets.erase(found);
        }

        Target target;
        target.depth = key.depth;
        target.width = width;
        target.height = height;
        target.scale = SceneScaleFor(width);
        target.format = VkFormat(key.format);
        target.guestFormat = guestFormat;
        target.samples = HostSamples(key.samples);
        if (target.format == VK_FORMAT_UNDEFINED) { Skip("render target format"); return nullptr; }

        // TRANSFER_DST because targets are cleared with transfer commands --
        // on a new target, after a resolve, and for the EDRAM clear idiom. An
        // image created without it is laid out on the assumption nothing will
        // ever write it that way.
        const VkImageUsageFlags usage = key.depth
            ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        const VkImageAspectFlags aspect = key.depth
            ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : VK_IMAGE_ASPECT_COLOR_BIT;
        if (!MakeImage(target, usage, aspect)) { Skip("render target allocation"); return nullptr; }

        LOGI("renderer: %s target %ux%u at EDRAM tile %u (%s), %ux for the title's %ux",
             key.depth ? "depth" : "colour", HostOf(target.width, target.scale), HostOf(target.height, target.scale),
             key.baseTile,
             key.depth ? gpu::DepthTargetFormatName(guestFormat)
                       : (gpu::ColorTargetFormatName(guestFormat) ?: "unknown"),
             unsigned(target.samples), 1u << key.samples);
        Target& stored = g.targets.emplace(key, target).first->second;
        ClearNewTarget(stored);
        return &stored;
    }

    void ApplySceneScale()
    {
        if (!g.dynamicScale) return;
        const float wanted = float(g.dynScale.Current()) / 16.0f;
        if (wanted == g.sceneScale) return;
        const float was = g.sceneScale;

        // The full-frame surfaces carry their scale (Target::scale) and the resolves out of
        // them carry it too (renderer_resolve.cpp makes a copy over when its scale differs),
        // so only these are rebuilt: the next draw into one makes it at the new size.
        RetireBeforeDestroy();
        for (auto found = g.targets.begin(); found != g.targets.end();)
        {
            if (found->second.width >= 1152 && found->second.scale != wanted)
            {
                DestroyTarget(found->second);
                found = g.targets.erase(found);
            }
            else ++found;
        }
        g.sceneScale = wanted;
        LOGI("renderer: the scene scale is now %.4g of 1280x720 (%ux%u), was %.4g (heat status %d)", wanted,
             HostOf(1280, wanted), HostOf(720, wanted), was, g.thermalStatus.load(std::memory_order_relaxed));
    }

    VkRenderPass GetRenderPass(VkFormat colourFormat, VkFormat depthFormat, VkSampleCountFlagBits samples)
    {
        static std::mutex s_renderPassMutex;
        std::lock_guard lock(s_renderPassMutex);
        const RenderPassKey key{ colourFormat, depthFormat, samples };
        auto found = g.legacyRenderPasses.find(key);
        if (found != g.legacyRenderPasses.end()) return found->second;

        VkAttachmentDescription attachments[2]{};
        uint32_t count = 0;

        VkAttachmentReference colourRef{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
        if (colourFormat != VK_FORMAT_UNDEFINED)
        {
            colourRef = { count, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkAttachmentDescription& desc = attachments[count++];
            desc.format = colourFormat;
            desc.samples = samples;
            desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        VkAttachmentReference depthRef{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
        if (depthFormat != VK_FORMAT_UNDEFINED)
        {
            depthRef = { count, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            VkAttachmentDescription& desc = attachments[count++];
            desc.format = depthFormat;
            desc.samples = samples;
            desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            // Stencil survives the pass too: PES's menus mark a scrolling list's area
            // in stencil and draw its rows with an EQUAL test, often in a later pass
            // than the marking. Discarded, every team name in the list was clipped away.
            desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            desc.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            desc.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = (colourFormat != VK_FORMAT_UNDEFINED) ? 1 : 0;
        subpass.pColorAttachments = (colourFormat != VK_FORMAT_UNDEFINED) ? &colourRef : nullptr;
        subpass.pDepthStencilAttachment = (depthFormat != VK_FORMAT_UNDEFINED) ? &depthRef : nullptr;

        // Subpass dependencies to prevent tile memory writeback hazards on TBDR GPUs (Mali, Adreno).
        // Using VK_DEPENDENCY_BY_REGION_BIT ensures dependencies stay within on-chip tile SRAM.
        VkSubpassDependency dependencies[2]{};
        uint32_t depCount = 0;

        if (colourFormat != VK_FORMAT_UNDEFINED)
        {
            VkSubpassDependency& dep = dependencies[depCount++];
            dep.srcSubpass = VK_SUBPASS_EXTERNAL;
            dep.dstSubpass = 0;
            dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        }

        if (depthFormat != VK_FORMAT_UNDEFINED)
        {
            VkSubpassDependency& dep = dependencies[depCount++];
            dep.srcSubpass = VK_SUBPASS_EXTERNAL;
            dep.dstSubpass = 0;
            dep.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        }

        VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        info.attachmentCount = count;
        info.pAttachments = attachments;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = depCount;
        info.pDependencies = dependencies;

        VkRenderPass pass = VK_NULL_HANDLE;
        VkResult r = vkCreateRenderPass(g.device, &info, nullptr, &pass);
        if (r != VK_SUCCESS)
        {
            LOGE("renderer: vkCreateRenderPass failed: %d (colour=%d, depth=%d, samples=%d)",
                 r, colourFormat, depthFormat, samples);
            return VK_NULL_HANDLE;
        }

        g.legacyRenderPasses[key] = pass;
        return pass;
    }

    VkFramebuffer GetFramebuffer(VkRenderPass pass, VkImageView colourView, VkImageView depthView,
                                 uint32_t width, uint32_t height)
    {
        if (!pass) return VK_NULL_HANDLE;
        const FramebufferKey key{ pass, colourView, depthView, width, height };
        auto found = g.legacyFramebuffers.find(key);
        if (found != g.legacyFramebuffers.end()) return found->second;

        VkImageView views[2]{};
        uint32_t count = 0;
        if (colourView) views[count++] = colourView;
        if (depthView) views[count++] = depthView;
        if (!count) return VK_NULL_HANDLE;

        VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        info.renderPass = pass;
        info.attachmentCount = count;
        info.pAttachments = views;
        info.width = width;
        info.height = height;
        info.layers = 1;

        VkFramebuffer fb = VK_NULL_HANDLE;
        VkResult res = vkCreateFramebuffer(g.device, &info, nullptr, &fb);
        if (res != VK_SUCCESS)
        {
            LOGE("renderer: vkCreateFramebuffer failed: %d (pass=%p, count=%u, %ux%u)",
                 res, pass, count, width, height);
            return VK_NULL_HANDLE;
        }

        g.legacyFramebuffers[key] = fb;
        return fb;
    }

    void InvalidateFramebuffers()
    {
        for (auto& [k, fb] : g.legacyFramebuffers)
            if (fb) vkDestroyFramebuffer(g.device, fb, nullptr);
        g.legacyFramebuffers.clear();
    }

    void ClearRenderPassCache()
    {
        InvalidateFramebuffers();
        for (auto& [k, pass] : g.legacyRenderPasses)
            if (pass) vkDestroyRenderPass(g.device, pass, nullptr);
        g.legacyRenderPasses.clear();
    }

    void BeginRendering(const Target& colour, const Target& depth, uint32_t width,
                        uint32_t height, uint32_t areaWidth, uint32_t areaHeight)
    {
        EndPass();
        const VkImageView colourView = colour.view, depthView = depth.view;
        if (!colourView && !depthView) return;

        // Both attachments of a pass have to be drawn at one scale.
        const float scale = colourView ? colour.scale : depth.scale;
        if (colourView && depthView && colour.scale != depth.scale)
        {
            static std::atomic<int> told{ 0 };
            if (told++ < 4)
                LOGW("renderer: a pass pairs a %ux%u colour surface at scale %.2f with a %ux%u depth surface at %.2f",
                     colour.width, colour.height, colour.scale, depth.width, depth.height, depth.scale);
        }
        uint32_t maxW = HostOf(width, scale);
        uint32_t maxH = HostOf(height, scale);
        if (colourView)
        {
            maxW = std::min(maxW, HostOf(colour.width, colour.scale));
            maxH = std::min(maxH, HostOf(colour.height, colour.scale));
        }
        if (depthView)
        {
            maxW = std::min(maxW, HostOf(depth.width, depth.scale));
            maxH = std::min(maxH, HostOf(depth.height, depth.scale));
        }
        const VkExtent2D full{ std::max(1u, maxW), std::max(1u, maxH) };
        // The part of it this pass is opened over: never more than all of it.
        VkExtent2D area = full;
        uint32_t coveredW = width, coveredH = height;
        if (g.smallPasses && areaWidth && areaHeight && (areaWidth < width || areaHeight < height))
        {
            coveredW = std::min(areaWidth, width);
            coveredH = std::min(areaHeight, height);
            area.width = std::clamp(HostOf(coveredW, scale), 1u, full.width);
            area.height = std::clamp(HostOf(coveredH, scale), 1u, full.height);
        }

        const bool legacy = vk::pipeline::LegacyMode();
        VkRenderPass legacyPass = VK_NULL_HANDLE;
        VkFramebuffer legacyFb = VK_NULL_HANDLE;
        if (legacy)
        {
            const VkFormat cFmt = colourView ? colour.format : VK_FORMAT_UNDEFINED;
            const VkFormat dFmt = depthView ? depth.format : VK_FORMAT_UNDEFINED;
            VkSampleCountFlagBits samples = (cFmt != VK_FORMAT_UNDEFINED) ? colour.samples : depth.samples;
            if (samples == 0) samples = VK_SAMPLE_COUNT_1_BIT;
            legacyPass = GetRenderPass(cFmt, dFmt, samples);
            legacyFb = GetFramebuffer(legacyPass, colourView, depthView, full.width, full.height);
            if (!legacyPass || !legacyFb)
            {
                LOGE("renderer: BeginRendering failed to acquire render pass (%p) or framebuffer (%p)",
                     legacyPass, legacyFb);
                return;
            }
        }
        const VkQueryPool timestamps = g.timingOn ? g.passTiming[g.slot].pool : VK_NULL_HANDLE;
        uint32_t timed = ~0u;
        if (timestamps && g.passTiming[g.slot].count < State::kTimedPasses)
        {
            timed = g.passTiming[g.slot].count++;
            g.passTiming[g.slot].key[timed] = PassTimingKey(area.width, area.height,
                colourView ? uint32_t(colour.samples) : uint32_t(depth.samples), depthView != VK_NULL_HANDLE);
        }
        g.timedPass = timed;
        Record([=](VkCommandBuffer command) {
            if (timed != ~0u)
                vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 2 * timed);
            // What a render pass's dependency in from the outside was: a frame
            // is many passes over the same attachments, with copies and clears
            // between them, and this one waits for their writes.
            VkMemoryBarrier before{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            before.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_TRANSFER_WRITE_BIT;
            before.dstAccessMask = kColourAccess | kDepthAccess;
            vkCmdPipelineBarrier(command,
                                 kColourStages | kDepthStages | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 kColourStages | kDepthStages, 0, 1, &before, 0, nullptr, 0,
                                 nullptr);
            if (legacy)
            {
                VkRenderPassBeginInfo beginInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                beginInfo.renderPass = legacyPass;
                beginInfo.framebuffer = legacyFb;
                beginInfo.renderArea.extent = area;
                vkCmdBeginRenderPass(command, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
            }
            else
            {
                // LOAD, not CLEAR: a frame is many rendering instances -- one per
                // change of target -- and only the title's own draws clear anything.
                VkRenderingAttachmentInfoKHR colourAttachment{
                    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
                colourAttachment.imageView = colourView;
                colourAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colourAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                colourAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingAttachmentInfoKHR depthAttachment = colourAttachment;
                depthAttachment.imageView = depthView;
                depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                // What the pass has, and no more: a pipeline made for a colour-only or
                // depth-only target names no attachment of the other kind, and a render
                // pass instance that lists one (a null view) does not match it. A driver
                // that checks, as Adreno 740's does, draws tile-shaped garbage.
                VkRenderingInfoKHR info{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
                info.renderArea.extent = area;
                info.layerCount = 1;
                info.colorAttachmentCount = colourView ? 1 : 0;
                info.pColorAttachments = colourView ? &colourAttachment : nullptr;
                info.pDepthAttachment = depthView ? &depthAttachment : nullptr;
                info.pStencilAttachment = depthView ? &depthAttachment : nullptr;
                dispatch.beginRendering(command, &info);
            }
        });
        OpenGuestQuery();
        g.currentColour = colour.view;
        g.currentDepth = depth.view;
        g.currentSamples = uint32_t(colour.samples);
        g.currentWidth = width;
        g.currentHeight = height;
        g.currentScale = scale;
        g.passAreaWidth = coveredW;
        g.passAreaHeight = coveredH;
    }

    // The Xenos clears the EDRAM surface as part of the resolve that reads it
    // (bits 8 and 9 of RB_COPY_CONTROL). Without the clear every frame is drawn
    // on top of the last.
    void ClearAfterResolve(const gpu::RegisterFile& r, const gpu::CopyControl& control)
    {
        if (g_dumpDraws)
        {
            const gpu::SurfaceInfo surface{ r[gpu::RB_SURFACE_INFO] };
            const gpu::ColorInfo colour{ r[gpu::RB_COLOR_INFO] };
            const bool found = FindTarget({ colour.BaseTile(), surface.Pitch(), uint32_t(ColourFormatFor(colour.Format())),
                                            surface.MsaaSamples(), false }) != nullptr;
            LOGW("DUMP   resolve clear: control %08X colour %d (value %08X, target tile %u pitch %u fmt %u x%u %s) depth %d (value %08X)",
                 r[gpu::RB_COPY_CONTROL], control.ClearsColor() ? 1 : 0, r[gpu::RB_COLOR_CLEAR], colour.BaseTile(),
                 surface.Pitch(), colour.Format(), surface.MsaaSamples(), found ? "found" : "NOT FOUND",
                 control.ClearsDepth() ? 1 : 0, r[gpu::RB_DEPTH_CLEAR]);
        }
        if (!control.ClearsColor() && !control.ClearsDepth()) return;

        if (control.ClearsColor())
        {
            const gpu::SurfaceInfo surface{ r[gpu::RB_SURFACE_INFO] };
            const gpu::ColorInfo colour{ r[gpu::RB_COLOR_INFO] };
            if (Target* target = FindTarget({ colour.BaseTile(), surface.Pitch(),
                                              uint32_t(ColourFormatFor(colour.Format())),
                                              surface.MsaaSamples(), false }))
            {
                // RB_COLOR_CLEAR is the packed colour in the surface's own format;
                // for the 8_8_8_8 family that is ARGB, most significant byte first.
                const uint32_t packed = r[gpu::RB_COLOR_CLEAR];
                VkClearColorValue value{};
                value.float32[0] = ((packed >> 16) & 0xFF) / 255.0f;
                value.float32[1] = ((packed >>  8) & 0xFF) / 255.0f;
                value.float32[2] = ((packed >>  0) & 0xFF) / 255.0f;
                value.float32[3] = ((packed >> 24) & 0xFF) / 255.0f;
                VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                RecordBarrier(target->image, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                              kColourStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
                const VkImage image = target->image;
                Record([=](VkCommandBuffer command) {
                    vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         &value, 1, &range);
                });
                RecordBarrier(target->image, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_ACCESS_TRANSFER_WRITE_BIT, kColourAccess,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, kColourStages);
            }
        }

        if (control.ClearsDepth())
        {
            const gpu::SurfaceInfo surface{ r[gpu::RB_SURFACE_INFO] };
            const gpu::DepthInfo depthInfo{ r[gpu::RB_DEPTH_INFO] };
            if (Target* target = FindTarget(DepthKey(depthInfo.BaseTile(), uint32_t(g.depthFormat),
                                                     surface.MsaaSamples())))
            {
                // The stored value is 24-bit depth with stencil in the low byte.
                const uint32_t packed = r[gpu::RB_DEPTH_CLEAR];
                VkClearDepthStencilValue value{ float(packed >> 8) / float(0xFFFFFF),
                                                uint32_t(packed & 0xFF) };
                constexpr VkImageAspectFlags kBoth =
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                VkImageSubresourceRange range{ kBoth, 0, 1, 0, 1 };
                RecordBarrier(target->image, kBoth,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                              kDepthStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
                const VkImage image = target->image;
                Record([=](VkCommandBuffer command) {
                    vkCmdClearDepthStencilImage(command, image,
                                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                &value, 1, &range);
                });
                RecordBarrier(target->image, kBoth,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                              VK_ACCESS_TRANSFER_WRITE_BIT, kDepthAccess,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, kDepthStages);
            }
        }
    }

    // One-shot, on the set-up command buffer: a new target leaves UNDEFINED
    // and is cleared before any frame's render pass loads it.
    void ClearNewTarget(Target& target)
    {
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCommandBuffer cmd = g.setup;
        if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return;

        const VkImageAspectFlags aspect = target.depth
            ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : VK_IMAGE_ASPECT_COLOR_BIT;
        Barrier(cmd, target.image, aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageSubresourceRange range{ aspect, 0, 1, 0, 1 };
        if (target.depth)
        {
            // Zero, not one: a level tests GEQUAL, and RB_DEPTH_CLEAR is zero.
            VkClearDepthStencilValue value{ 0.0f, 0 };
            vkCmdClearDepthStencilImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &value, 1, &range);
        }
        else
        {
            // Not black: a target the title has not drawn into, and a frame that reached
            // the screen without a draw, are then distinguishable from an empty resolve.
            VkClearColorValue value{};
            value.float32[0] = 0.06f; value.float32[1] = 0.07f;
            value.float32[2] = 0.10f; value.float32[3] = 1.0f;
            vkCmdClearColorImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &value, 1, &range);
        }
        Barrier(cmd, target.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                target.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                             : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                target.depth ? kDepthAccess : kColourAccess,
                VK_PIPELINE_STAGE_TRANSFER_BIT, target.depth ? kDepthStages : kColourStages);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        std::lock_guard lock(vk::pipeline::QueueMutex());
        vkResetFences(g.device, 1, &g.setupFence);
        vkQueueSubmit(g.queue, 1, &submit, g.setupFence);
        VkResult res = vkWaitForFences(g.device, 1, &g.setupFence, VK_TRUE, 2000000000ull);
        if (res == VK_TIMEOUT)
        {
            LOGW("renderer: waiting for the set-up fence took >2 seconds; continuing wait...");
            res = vkWaitForFences(g.device, 1, &g.setupFence, VK_TRUE, UINT64_MAX);
        }
        vk::pipeline::Failed(res, "the set-up fence");
    }
}

#endif  // PES17_HAVE_VULKAN
