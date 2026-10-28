#include <sys/resource.h>
#include <string>
#include "../../crash.h"
#include <map>
#include <chrono>
#include <unordered_map>
#include <cstring>
#include <atomic>
#include "../../kernel/kernel.h"
#include <atomic>
namespace kernel { extern std::atomic<bool> g_traceAll; }
#include "../../guest_memory.h"
#include <unordered_set>
// Resolves: EDRAM copied out to a texture the title reads back, or the
// finished frame copied out for the window.
#include "renderer_state.h"
#include <sys/stat.h>
#include "display_table.h"
#include "presenter.h"
#include "texture_cache.h"
#include "texture_formats.h"
#include "../internal.h"
#include "../texture.h"
#include "../gpu.h"
#include "../../log.h"
#ifdef PES17_ANDROID
#include "../../android/android.h"
#endif

// Where the touch-a-file debug switches live: the app's files folder on Android,
// $PES17_FILES (or the working directory) elsewhere.
static std::string DebugFilesDir()
{
#ifdef PES17_ANDROID
    return android::GetPaths().external;
#else
    const char* dir = std::getenv("PES17_FILES");
    return dir ? dir : ".";
#endif
}

#include <algorithm>
#include <cstdio>
#include <unistd.h>

#ifdef PES17_HAVE_VULKAN

namespace vk::renderer::detail
{
    // A resolve is a copy in the command stream and nothing more, so it does
    // not end the submission: a frame is submitted once, when it is presented.
    // Ending one at every resolve spent a frame slot on each -- six a frame --
    // so the three slots held half a frame between them, and the CPU waited for
    // the GPU to draw the world while the GPU waited for the CPU to record the
    // next one. Only a slot whose arena share is half used goes early, so the
    // rest of the frame has room.
    bool SubmitIfFilling()
    {
        return g.arenaUsed <= g.arenaSlotBytes / 2 || Submit();
    }

    // The copy this frame's resolves write and this frame's draws read. The
    // first resolve of a frame moves on to the next one; the rest of the frame
    // -- the second shadow cascade, the draws that sample it -- get that same
    // one back.
    Resolved& ResolveCopyFor(uint32_t destination)
    {
        ResolveRing& ring = g.resolvedTo[destination];
        if (ring.frame != g.frames)
        {
            ring.frame = g.frames;
            ring.newest = (ring.newest + 1) % kFrameSlots;
        }
        return ring.copies[ring.newest];
    }

    bool MakeResolveImage(Resolved& into, uint32_t width, uint32_t height,
                          VkFormat format, bool depth = false, bool exact = false, float scale = 1.0f)
    {
        // A copy is drawn at its source surface's scale (the scene scale for a
        // full-frame one), and a different scale is a different image.
        if (into.image && into.scale != scale) exact = true;
        // Grow only within a frame: the second shadow cascade lands inside the
        // surface the first one made, and must not throw the first away.
        //
        // But the first resolve of a frame into this copy (`exact`) sizes it to
        // the surface exactly. The fetch normalises its coordinates over the
        // image, so an image wider than the surface stretches what it holds.
        const bool fits = exact ? into.width == width && into.height == height
                                : into.width >= width && into.height >= height;
        if (into.image && fits && into.format == format && into.depth == depth && into.scale == scale)
            return true;
        if (!exact && into.image && into.format == format && into.depth == depth && into.scale == scale)
        {
            width = std::max(width, into.width);
            height = std::max(height, into.height);
        }
        // A copy of another shape. FIFA Street resolves the match's depth three times a frame
        // into one address, at 1280x736, 1280x720 and a scene-scaled size; making each anew
        // destroyed the last, and destroying waits for the GPU to go idle -- three full stalls a
        // frame, which held the phone at 30. So shapes are kept: a spare of the wanted shape is
        // swapped in, and the image it replaces waits among the spares, alive, for its turn.
        // Nothing is freed, so nothing waits; work already recorded against an image is ordered
        // before a later copy into it by the copy's barrier, on the one queue.
        {
            static std::vector<Resolved> spares;
            constexpr size_t kMaxSpares = 48;
            for (size_t i = 0; i < spares.size(); i++)
            {
                Resolved& spare = spares[i];
                if (spare.format != format || spare.depth != depth || spare.scale != scale ||
                    spare.width != width || spare.height != height)
                    continue;
                std::swap(into, spare);
                into.writtenFrame = UINT64_MAX;   // holds nothing of this destination's
                into.writtenBack = false;
                if (!spares[i].image) spares.erase(spares.begin() + ptrdiff_t(i));
                return true;
            }
            if (into.image && spares.size() >= kMaxSpares)
            {
                // Full. Every scene-scale step leaves a set of shapes at a scale nobody asks for
                // again, and the pool never freed them: once it filled, each of the match's three
                // depth resolves a frame took the destroy path below (a wait for the GPU to go
                // idle: 60% of the command thread's time in FIFA Street's futsal hall). Free a
                // batch in one wait -- the shapes of other scales first, then the oldest.
                std::vector<Resolved> doomed;
                for (size_t i = 0; i < spares.size();)
                {
                    if (spares[i].scale != scale && spares[i].scale != 1.0f && spares[i].scale != g.sceneScale)
                    {
                        doomed.push_back(spares[i]);
                        spares.erase(spares.begin() + ptrdiff_t(i));
                    }
                    else i++;
                }
                for (size_t n = spares.size() >= kMaxSpares ? 8 : 0; n > 0 && !spares.empty(); n--)
                {
                    doomed.push_back(spares.front());
                    spares.erase(spares.begin());
                }
                if (!doomed.empty())
                {
                    RetireBeforeDestroy();
                    vk::textures::ForgetSets();
                    for (Resolved& old : doomed)
                    {
                        if (old.view) vkDestroyImageView(g.device, old.view, nullptr);
                        if (old.gammaView) vkDestroyImageView(g.device, old.gammaView, nullptr);
                        if (old.image) vkDestroyImage(g.device, old.image, nullptr);
                        if (old.memory) vkFreeMemory(g.device, old.memory, nullptr);
                    }
                }
            }
            if (into.image && spares.size() < kMaxSpares)
            {
                spares.push_back(into);
                into = Resolved{};
            }
        }
        // Past the spares' cap -- rare -- both a submitted slot and the buffer being recorded
        // may still name the image being replaced.
        if (into.image) RetireBeforeDestroy();
        // The texture cache has built sets on these views. Its own check -- the
        // view behind an id changed -- misses a replacement that gets the old
        // handle back, and a set on the freed image is a texture read of unbacked
        // memory: the TCP page faults that hung the GPU in play and under capture.
        if (into.image) vk::textures::ForgetSets();
        if (into.view) vkDestroyImageView(g.device, into.view, nullptr);
        if (into.gammaView) vkDestroyImageView(g.device, into.gammaView, nullptr);
        if (into.image) vkDestroyImage(g.device, into.image, nullptr);
        if (into.memory) vkFreeMemory(g.device, into.memory, nullptr);
        into = Resolved{};
        g.resolveImagesMade++;

        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { HostOf(width, scale), HostOf(height, scale), 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Two views of one image, so the format has to be allowed to differ from
        // the one the image was made with. The list is what lets a driver keep
        // the compression it would otherwise give up for a mutable image.
        const VkFormat srgb = depth ? VK_FORMAT_UNDEFINED
                                    : VkFormat(vk::formats::SrgbFormatFor(format));
        const VkFormat both[2] = { format, srgb };
        VkImageFormatListCreateInfo list{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
        list.viewFormatCount = 2;
        list.pViewFormats = both;
        if (srgb != VK_FORMAT_UNDEFINED)
        {
            info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            info.pNext = &list;
        }
        if (vkCreateImage(g.device, &info, nullptr, &into.image) != VK_SUCCESS) return false;

        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(g.device, into.image, &needs);
        const uint32_t type = FindMemory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type;
        if (vkAllocateMemory(g.device, &allocate, nullptr, &into.memory) != VK_SUCCESS) return false;
        vkBindImageMemory(g.device, into.image, into.memory, 0);

        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = into.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        // Depth only: a view carrying stencil as well cannot be sampled.
        view.subresourceRange = { VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                           : VK_IMAGE_ASPECT_COLOR_BIT),
                                  0, 1, 0, 1 };
        if (vkCreateImageView(g.device, &view, nullptr, &into.view) != VK_SUCCESS) return false;
        if (srgb != VK_FORMAT_UNDEFINED)
        {
            view.format = srgb;
            if (vkCreateImageView(g.device, &view, nullptr, &into.gammaView) != VK_SUCCESS)
                into.gammaView = VK_NULL_HANDLE;
        }
        into.format = format;
        into.width = width;
        into.height = height;
        into.scale = scale;
        into.depth = depth;
        static uint32_t shapes = 0;
        into.shape = ++shapes;
        LOGI("renderer: %ux%u image for a %s resolve destination", HostOf(width, scale),
             HostOf(height, scale),
             depth ? "depth" : "colour");
        return true;
    }
}

using namespace vk::renderer::detail;

namespace vk::renderer::detail
{
    // Resolves written back to guest memory (the kits, the crowd's colour atlas, the players'
    // name and number textures), without a wait each. Every one used to submit the frame, wait
    // for the GPU to finish it and read the image back on the spot, so the GPU stood idle while
    // the consumer recorded and the consumer while the GPU drew. Now the copy out is queued in
    // the frame, into a buffer the CPU can read, and the frame so far is submitted at once, so
    // the GPU is at work on it while the consumer carries on. It lands in guest memory when the
    // title could next look -- an end-of-pipe write, an interrupt, the swap
    // (vk::renderer::PublishWriteBacks) -- and only that submission is waited for.
    struct Staging
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        const uint8_t* mapped = nullptr;
        VkDeviceSize bytes = 0;
    };

    struct PendingWriteBack
    {
        Staging staging;
        Resolved* copy = nullptr;      // marked written back if it is still this resolve
        VkImage image = VK_NULL_HANDLE;
        uint64_t writtenFrame = 0, frame = 0;
        uint64_t serial = 0;           // the submission its copy is in
        uint32_t destination = 0, va = 0, destX = 0, destY = 0, width = 0, height = 0;
        uint32_t imageWidth = 0, imageHeight = 0, pitchBlocks = 0, pitch = 0, endian = 0;
        bool swap = false;
        std::chrono::steady_clock::time_point queued;
    };

    std::vector<PendingWriteBack> g_pendingWriteBacks;
    std::vector<Staging> g_spareStaging;
    constexpr size_t kMostPendingWriteBacks = 48;   // each holds a buffer
    constexpr size_t kMostSpareStaging = 32;

    bool TakeStaging(VkDeviceSize bytes, Staging& out)
    {
        // The smallest spare one that fits.
        size_t best = g_spareStaging.size();
        for (size_t i = 0; i < g_spareStaging.size(); i++)
            if (g_spareStaging[i].bytes >= bytes &&
                (best == g_spareStaging.size() || g_spareStaging[i].bytes < g_spareStaging[best].bytes))
                best = i;
        if (best != g_spareStaging.size())
        {
            out = g_spareStaging[best];
            g_spareStaging.erase(g_spareStaging.begin() + std::ptrdiff_t(best));
            return true;
        }
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(g.device, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(g.device, out.buffer, &needs);
        // Cached if the device has it: the CPU reads every byte back. Uncached memory (what
        // HOST_COHERENT alone gives on Mali) made the copy into guest memory 63% of the ring
        // consumer's time in the match intro on the POCO X7 Pro, where the title composes the
        // players' textures one by one and waits for each to land.
        constexpr VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        constexpr VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        constexpr VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = FindMemory(needs.memoryTypeBits, visible | cached | coherent);
        static bool told = false;
        if (!told)
        {
            told = true;
            LOGI("resolve: write-backs read through %s memory", type == UINT32_MAX ? "uncached" : "cached");
        }
        if (type == UINT32_MAX) type = FindMemory(needs.memoryTypeBits, visible | coherent);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type;
        void* mapped = nullptr;
        if (type == UINT32_MAX || vkAllocateMemory(g.device, &allocate, nullptr, &out.memory) != VK_SUCCESS ||
            vkBindBufferMemory(g.device, out.buffer, out.memory, 0) != VK_SUCCESS ||
            vkMapMemory(g.device, out.memory, 0, bytes, 0, &mapped) != VK_SUCCESS)
        {
            if (out.memory) vkFreeMemory(g.device, out.memory, nullptr);
            vkDestroyBuffer(g.device, out.buffer, nullptr);
            out = {};
            return false;
        }
        out.mapped = static_cast<const uint8_t*>(mapped);
        out.bytes = bytes;
        return true;
    }

    void GiveBackStaging(const Staging& staging)
    {
        if (g_spareStaging.size() < kMostSpareStaging) { g_spareStaging.push_back(staging); return; }
        vkUnmapMemory(g.device, staging.memory);
        vkDestroyBuffer(g.device, staging.buffer, nullptr);
        vkFreeMemory(g.device, staging.memory, nullptr);
    }

    // The copy's pixels into guest memory as the Xenos stores them: 2D tiled, with the R/B
    // swap the destination asks for and 8-in-32 endian.
    void ApplyWriteBack(const PendingWriteBack& p, double waitedMs)
    {
        // The image out of the mapped buffer in one go: a pixel at a time, four byte loads
        // each, is slow from memory the CPU does not cache.
        static thread_local std::vector<uint8_t> local;
        const size_t imageBytes = size_t(p.imageWidth) * p.imageHeight * 4;
        local.resize(imageBytes);
        std::memcpy(local.data(), p.staging.mapped, imageBytes);
        const uint8_t* const pixels = local.data();
        uint8_t* const base = guest::Base() + p.va;
        for (uint32_t y = p.destY; y < p.destY + p.height && y < p.imageHeight; y++)
            for (uint32_t x = p.destX; x < p.destX + p.width && x < p.imageWidth; x++)
            {
                const uint8_t* px = &pixels[(size_t(y) * p.imageWidth + x) * 4];
                uint8_t c[4] = { px[0], px[1], px[2], px[3] };
                if (p.swap) std::swap(c[0], c[2]);
                const uint32_t word = uint32_t(c[0]) | uint32_t(c[1]) << 8 |
                                      uint32_t(c[2]) << 16 | uint32_t(c[3]) << 24;
                const uint32_t stored = p.endian == 2 ? __builtin_bswap32(word) : word;
                const int32_t at = gpu::TiledOffset2D(int32_t(x), int32_t(y), p.pitchBlocks, 2);
                std::memcpy(base + at, &stored, 4);
            }
        gpu::NoteWriteBack();
        // Fetches read guest memory from now on -- unless the copy has been resolved to again
        // since, when it belongs to that later resolve.
        if (p.copy && p.copy->image == p.image && p.copy->writtenFrame == p.writtenFrame)
            p.copy->writtenBack = true;
        g.windowWriteBacks++;
        if (p.width == 256 && p.height == 256)
        {
            // A kit composition: its average colour (R G B), for tools/kits.sh.
            uint64_t sum[3] = {};
            uint32_t n = 0;
            for (uint32_t y = 0; y < 256 && y < p.imageHeight; y += 4)
                for (uint32_t x = 0; x < 256 && x < p.imageWidth; x += 4)
                {
                    const uint8_t* px = &pixels[(size_t(y) * p.imageWidth + x) * 4];
                    for (int c = 0; c < 3; c++) sum[c] += px[c];
                    n++;
                }
            if (n)
                LOGI("kitcompose %08X avg %02X %02X %02X frame %llu", p.destination, uint32_t(sum[0] / n),
                     uint32_t(sum[1] / n), uint32_t(sum[2] / n), (unsigned long long)p.frame);
        }
        LOGI("resolve: wrote %ux%u back to %08X..%08X (pitch %u, endian %u, swap %d), frame %llu, %lld ms after it was queued (%.0f ms waiting for the GPU)",
             p.width, p.height, p.destination, p.destination + p.pitchBlocks * ((p.height + 31u) & ~31u) * 4u,
             p.pitch, p.endian, p.swap ? 1 : 0, (unsigned long long)p.frame,
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - p.queued).count(),
             waitedMs);
    }

    // Submission `serial` finished on the GPU, waiting for it if it has not.
    void WaitForSubmission(uint64_t serial)
    {
        if (serial > g.submissionSerial)
        {
            // Still in the frame being recorded.
            const bool recording = g.recording;
            Submit(true);
            if (recording && !g.recording) BeginFrame();
            return;
        }
        for (State::Slot& slot : g.slots)
            if (slot.serial == serial && slot.inFlight) { WaitForSlot(slot, "a write-back's copy"); return; }
        // Its slot has been used again since, so it finished.
    }

    void PublishWriteBacksNow()
    {
        if (g_pendingWriteBacks.empty()) return;
        const auto from = std::chrono::steady_clock::now();
        // In order, so the newest is the last to finish.
        WaitForSubmission(g_pendingWriteBacks.back().serial);
        const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
        for (const PendingWriteBack& p : g_pendingWriteBacks)
        {
            ApplyWriteBack(p, waited);
            GiveBackStaging(p.staging);
        }
        g_pendingWriteBacks.clear();
    }

    void QueueWriteBack(Resolved& into, uint32_t destination, uint32_t va, uint32_t destX, uint32_t destY,
                        uint32_t width, uint32_t height, uint32_t pitchBlocks, uint32_t pitch, bool swap,
                        uint32_t endian)
    {
        if (g_pendingWriteBacks.size() >= kMostPendingWriteBacks) PublishWriteBacksNow();
        PendingWriteBack p;
        if (!TakeStaging(VkDeviceSize(into.width) * into.height * 4, p.staging)) return;
        p.copy = &into;
        p.image = into.image;
        p.writtenFrame = into.writtenFrame;
        p.frame = g.frames;
        p.destination = destination;
        p.va = va;
        p.destX = destX; p.destY = destY; p.width = width; p.height = height;
        p.imageWidth = into.width; p.imageHeight = into.height;
        p.pitchBlocks = pitchBlocks; p.pitch = pitch; p.swap = swap; p.endian = endian;
        p.queued = std::chrono::steady_clock::now();

        RecordBarrier(into.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT, kShaderStages,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkImage image = into.image;
        const VkBuffer buffer = p.staging.buffer;
        const uint32_t w = into.width, h = into.height;
        Record([=](VkCommandBuffer command) {
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { w, h, 1 };
            vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
            VkBufferMemoryBarrier toHost{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer = buffer;
            toHost.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                 0, nullptr, 1, &toHost, 0, nullptr);
        });
        RecordBarrier(into.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, kShaderStages);
        // The submission it goes out in: this one, sent now so the GPU starts on it.
        p.serial = g.submissionSerial + 1;
        g_pendingWriteBacks.push_back(p);
        // files/sync_writeback: each one waited for at once, as before, to compare.
        static const bool sync =
#ifdef PES17_ANDROID
            access((android::GetPaths().external + "/sync_writeback").c_str(), F_OK) == 0;
#else
            false;
#endif
        if (sync) { PublishWriteBacksNow(); return; }
        Submit(false);
        BeginFrame();
    }
}

void vk::renderer::PublishWriteBacks()
{
    if (!g.device) return;
    detail::PublishWriteBacksNow();
}

void vk::renderer::Resolve(const gpu::RegisterFile& r)
{
    Stopwatch watch(g.resolveNanoseconds);
    if (!g.device) return;
    const gpu::CopyControl control{ r[gpu::RB_COPY_CONTROL] };
    VkRect2D rect{};
    ScissorOf(r, rect);
    const uint32_t baseDestination = r[gpu::RB_COPY_DEST_BASE] & ~0xFFFu;
    const gpu::SurfaceInfo surface{ r[gpu::RB_SURFACE_INFO] };
    const gpu::ColorInfo colour{ r[gpu::RB_COLOR_INFO] };
    // The title reads the scene's depth back as an ordinary texture: a soft
    // particle fades where it meets geometry, and one that cannot find the depth
    // does not fade at all. Skipping this resolve is why the sprites are opaque
    // and the frame sits under a white wash.
    if (control.FromDepth())
    {
        const uint32_t destination = baseDestination;
        const gpu::DepthInfo depthInfo{ r[gpu::RB_DEPTH_INFO] };
        Target* from = FindDepthTarget(depthInfo.BaseTile(), uint32_t(g.depthFormat),
                                       surface.MsaaSamples());
        if (!from) { Skip("resolve of a depth surface never drawn into"); SubmitIfFilling(); return; }
        if (!g.recording && !BeginFrame()) { Skip("no command buffer"); return; }
        g.resolves++;
        EndPass();

        uint32_t width = std::min(rect.extent.width, from->width - uint32_t(rect.offset.x));
        uint32_t height = std::min(rect.extent.height, from->height - uint32_t(rect.offset.y));

        // The sun shadow map is one 1024x2048 surface holding two cascades, and
        // the title fills it with two resolves: the first to the surface's base,
        // the second to its lower half, four megabytes on, describing itself as
        // a surface of its own. The world's shaders then bind the whole thing,
        // at the first address, as one texture. Keeping an image per resolve
        // address gave that fetch the first cascade alone, stretched over the
        // lookup for both -- every shadow the near cascade casts landed twice as
        // far down the atlas as it should, and the far cascade was never read.
        // So a resolve that lands inside a surface an earlier resolve this frame
        // started is copied into that surface's image, at the row it lands on.
        const gpu::CopyDestPitch destPitch{ r[gpu::RB_COPY_DEST_PITCH] };
        const gpu::CopyDestInfo destInfo{ r[gpu::RB_COPY_DEST_INFO] };
        const uint32_t texelBytes = std::max(1u, gpu::TextureFormatOf(destInfo.Format()).bitsPerBlock / 8);
        const uint32_t rowBytes = destPitch.Pitch() * texelBytes;
        // The destination is tiled in 32x32 blocks, so the offset says which block: the cascades'
        // four megabytes on is 32 blocks down at x 0 (row 1024), and FIFA Street's right-hand predicated
        // match tile, 0x14000 on, is 20 blocks across (x 640) -- read as rows, it landed 16 rows down the
        // left half, the right half of the match's depth stayed stale, and the depth of field that reads
        // it blurred one half of every goal celebration and not the other.
        uint32_t surfaceBase = destination, row = 0, column = 0;
        for (const auto& [base, known] : g.resolveSurfaces)
        {
            const uint32_t tileBytes = 32u * 32u * texelBytes;
            const uint32_t tilesPerRow = rowBytes / texelBytes / 32u;
            if (known.frame == g.frames && base < destination &&
                destination - base < known.bytes && known.rowBytes == rowBytes && rowBytes && tilesPerRow &&
                (destination - base) % tileBytes == 0)
            {
                const uint32_t tile = (destination - base) / tileBytes;
                surfaceBase = base;
                column = (tile % tilesPerRow) * 32u;
                row = (tile / tilesPerRow) * 32u;
                break;
            }
        }
        if (surfaceBase == destination && rowBytes)
            g.resolveSurfaces[destination] = { rowBytes * std::min(destPitch.Height(), 8192u),
                                               rowBytes, g.frames };

        // A resolve lands where its rectangle sits: one that starts 16 rows down
        // the EDRAM surface starts 16 rows down the texture too, as Xenia's
        // copy_dest_coordinate_info has it. The shadow cascades are resolved
        // from the draw scissor rounded down to 8, which moves with the camera;
        // writing them at row 0 moved every shadow the atlas casts by that many
        // texels, and by a different amount as the view turned.
        const uint32_t destX = column + uint32_t(rect.offset.x);
        const uint32_t destY = row + uint32_t(rect.offset.y);

        Resolved& into = ResolveCopyFor(surfaceBase);
        // Sized for the whole surface, so the lower half has somewhere to go.
        const uint32_t imageWidth = std::max(destX + width, std::min(destPitch.Pitch(), 4096u));
        const uint32_t imageHeight = std::max(destY + height,
                                              std::min(destPitch.Height(), 4096u));
        const bool firstThisFrame = into.writtenFrame != g.frames;
        if (!destination || !width || !height ||
            !MakeResolveImage(into, imageWidth, imageHeight, from->format, true, firstThisFrame, from->scale))
        {
            Skip("depth resolve destination image");
            SubmitIfFilling();
            return;
        }
        width = std::min(width, into.width > destX ? into.width - destX : 0u);
        height = std::min(height, into.height > destY ? into.height - destY : 0u);
        const bool keep = into.writtenFrame == g.frames;
        into.writtenFrame = g.frames;

        constexpr VkImageAspectFlags kBoth =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        VkImage fromImage = BeginReadingDepth(*from, rect.offset, width, height);
        if (!fromImage) { Skip("depth resolve of a multisampled surface"); SubmitIfFilling(); return; }
        // The second cascade keeps the first: it comes out of the layout the
        // first left it in rather than out of UNDEFINED, which may discard.
        RecordBarrier(into.image, kBoth,
                      keep ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT, kShaderStages, VK_PIPELINE_STAGE_TRANSFER_BIT);

        // A copy rather than a blit: depth cannot be filtered on the way through,
        // and a plain copy asks nothing of the format beyond being transferable.
        // In Vulkan 1.1 (VUID-VkImageCopy-srcSubresource-01700), depth/stencil formats
        // must copy VK_IMAGE_ASPECT_DEPTH_BIT and VK_IMAGE_ASPECT_STENCIL_BIT separately.
        VkImageCopy copies[2]{};
        uint32_t copyCount = 1;
        copies[0].srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        const float s = from->scale;
        copies[0].srcOffset = Scaled(rect.offset.x, rect.offset.y, 0, s);
        copies[0].dstSubresource = copies[0].srcSubresource;
        copies[0].dstOffset = Scaled(int32_t(destX), int32_t(destY), 0, s);
        // Rounded at a fractional scale, so kept inside both images.
        const auto room = [](uint32_t size, int32_t at) { return size > uint32_t(at) ? size - uint32_t(at) : 0u; };
        copies[0].extent = { std::min({ HostOf(width, s), room(HostOf(from->width, s), copies[0].srcOffset.x),
                                        room(HostOf(into.width, s), copies[0].dstOffset.x) }),
                             std::min({ HostOf(height, s), room(HostOf(from->height, s), copies[0].srcOffset.y),
                                        room(HostOf(into.height, s), copies[0].dstOffset.y) }), 1 };

        if (from->format == VK_FORMAT_D24_UNORM_S8_UINT || from->format == VK_FORMAT_D32_SFLOAT_S8_UINT)
        {
            copies[1] = copies[0];
            copies[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            copies[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            copyCount = 2;
        }

        const VkImage intoImage = into.image;
        Record([=](VkCommandBuffer command) {
            vkCmdCopyImage(command, fromImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           intoImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copyCount, copies);
        });

        RecordBarrier(into.image, kBoth,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, kShaderStages);
        EndReadingDepth(*from);
        // In a frame dump, which depth image the match's depth resolve read (the depth of field reads it).
        if (g_dumpDraws && destination == 0x04870000u)
        {
            LOGW("DUMP depth resolve from tile %u, surface pitch %u samples %u: read %ux%u %ux scale %.3g, %llu draws, bound #%llu",
                 depthInfo.BaseTile(), surface.Pitch(), surface.MsaaSamples(), from->width, from->height,
                 unsigned(from->samples), from->scale, (unsigned long long)from->draws,
                 (unsigned long long)from->depthBound);
            for (const auto& [key, target] : g.targets)
                if (key.depth && key.baseTile == depthInfo.BaseTile())
                    LOGW("DUMP   depth image at the tile: key samples %u unscaled %d, %ux%u %ux scale %.3g, %llu draws, bound #%llu",
                         key.samples, int(key.unscaled), target.width, target.height, unsigned(target.samples),
                         target.scale, (unsigned long long)target.draws, (unsigned long long)target.depthBound);
        }
        g.depthResolves++;
        ClearAfterResolve(r, control);
        if (!SubmitIfFilling()) Skip("frame submission failed");
        return;
    }

    // The registers name the surface, and the last pass recorded is not always
    // it: the world's is resolved after a later pass has bound its own.
    const TargetKey sourceKey{ colour.BaseTile(), surface.Pitch(),
                               uint32_t(ColourFormatFor(colour.Format())),
                               surface.MsaaSamples(), false };
    Target* source = FindTarget(sourceKey);
    uint32_t gridScaleX = 1, gridScaleY = 1;
    if (!source)
    {
        // The same EDRAM surface described another way: PES 2017 draws a 4x
        // surface at pitch 640 and resolves it as a 1x one at pitch 1280, the
        // samples' row read as pixels. The bytes are the target drawn at that
        // tile in that format, whatever the pitch and sample count say now.
        for (auto& [key, target] : g.targets)
            if (!key.depth && key.baseTile == sourceKey.baseTile && key.format == sourceKey.format)
            {
                static bool told = false;
                if (!told)
                {
                    told = true;
                    LOGI("renderer: resolving tile %u (pitch %u, x%u) from the target drawn there"
                         " at pitch %u, x%u", sourceKey.baseTile, sourceKey.pitch, sourceKey.samples,
                         key.pitch, key.samples);
                }
                source = &target;
                // A 4x surface (2x2 samples a pixel) read as a 1x one at twice the
                // pitch is its sample grid, twice the size each way; a 2x one
                // (two samples stacked) is twice as tall. The target here holds
                // one colour a pixel (MSAA is off on phones), so the copy scales.
                if (sourceKey.samples == 0 && key.samples == 2 && sourceKey.pitch == key.pitch * 2)
                    gridScaleX = gridScaleY = 2;
                else if (sourceKey.samples == 0 && key.samples == 1 && sourceKey.pitch == key.pitch)
                    gridScaleY = 2;
                break;
            }
    }
    if (!source)
    {
        // Which surface, against the ones drawn into: the mismatch is the bug.
        static int told = 0;
        if (told < 4)
        {
            told++;
            std::string known;
            char item[64];
            for (const auto& [key, target] : g.targets)
            {
                std::snprintf(item, sizeof item, " [tile %u pitch %u fmt %u x%u%s]", key.baseTile,
                              key.pitch, key.format, key.samples, key.depth ? " depth" : "");
                known += item;
            }
            LOGW("renderer: resolve of tile %u pitch %u fmt %u (EDRAM fmt %u) x%u to %08X; targets:%s",
                 sourceKey.baseTile, sourceKey.pitch, sourceKey.format, uint32_t(colour.Format()),
                 sourceKey.samples, baseDestination, known.c_str());
        }
        Skip("resolve of a surface never drawn into");
        SubmitIfFilling();
        return;
    }

    // A resolve can arrive with nothing recording -- the one before it may have
    // submitted a full arena share. Its surface is still there.
    if (!g.recording && !BeginFrame()) { Skip("no command buffer"); return; }
    g.resolves++;

    EndPass();

    // The image is the destination surface RB_COPY_DEST_PITCH describes, not
    // the rectangle, and the rectangle lands at its own offset in it -- as
    // the depth resolve above does. The depth of field resolves a 256x152
    // rectangle, rounded up to 8 rows, into a 256x150 surface and fetches
    // 256x150; an image the size of the rectangle put two rows of whatever
    // the target held below the picture, the fetch's v = 1 reached them, and
    // their alpha of 1 -- the blur weight -- spread up through every blur
    // pass into a dark bar along the bottom of the screen when aiming down
    // the sights.
    const gpu::CopyDestPitch destPitch{ r[gpu::RB_COPY_DEST_PITCH] };
    const gpu::CopyDestInfo destInfo{ r[gpu::RB_COPY_DEST_INFO] };
    // The rectangle is in EDRAM (the window offset applied, ScissorOf), and lands at the same place
    // in the destination -- as Xenia's resolve has it. A base inside a tiled surface resolved to
    // earlier this frame is that surface, moved by whole 32x32 tiles: FIFA Street resolves its
    // right-hand predicated tile (EDRAM 0..640, after the window offset of -640) to 044D0000 +
    // 0x14000, twenty tiles across, x = 640. Kept apart, the right half of every match frame was
    // dropped as a resolve with no room in it, and the left half was all the screen showed.
    uint32_t destination = baseDestination;
    uint32_t tileX = 0, tileY = 0;
    {
        struct ColourSurface { uint32_t pitch = 0, height = 0, texelBytes = 0; uint64_t frame = UINT64_MAX; };
        static std::map<uint32_t, ColourSurface> surfaces;
        const uint32_t texelBytes = std::max(1u, gpu::TextureFormatOf(destInfo.Format()).bitsPerBlock / 8);
        const uint32_t pitch32 = (std::max(destPitch.Pitch(), 1u) + 31u) & ~31u;
        bool inside = false;
        auto it = surfaces.upper_bound(destination);
        if (it != surfaces.begin())
        {
            --it;
            const auto& [base, known] = *it;
            const uint32_t tileBytes = 32u * 32u * known.texelBytes;
            const uint32_t delta = destination - base;
            const uint32_t tilesPerRow = known.pitch / 32u;
            if (known.frame == g.frames && base < destination && known.texelBytes == texelBytes &&
                known.pitch == pitch32 && tilesPerRow && delta % tileBytes == 0 &&
                delta < uint64_t(known.pitch) * ((known.height + 31u) & ~31u) * known.texelBytes)
            {
                const uint32_t tile = delta / tileBytes;
                tileX = (tile % tilesPerRow) * 32u;
                tileY = (tile / tilesPerRow) * 32u;
                destination = base;
                inside = true;
            }
        }
        if (!inside && destPitch.Pitch())
            surfaces[destination] = { pitch32, std::max(destPitch.Height(), 1u), texelBytes, g.frames };
    }
    const uint32_t sourceX = uint32_t(rect.offset.x);
    const uint32_t sourceY = uint32_t(rect.offset.y);
    const uint32_t destX = tileX + sourceX;
    const uint32_t destY = tileY + sourceY;
    const uint32_t surfaceWidth = destPitch.Pitch() ? std::min(destPitch.Pitch(), 4096u)
                                                    : destX + rect.extent.width;
    const uint32_t surfaceHeight = destPitch.Height() ? std::min(destPitch.Height(), 4096u)
                                                      : destY + rect.extent.height;
    const auto room = [](uint32_t size, uint32_t at) { return size > at ? size - at : 0u; };
    const uint32_t width = std::min({ rect.extent.width, room(surfaceWidth, destX),
                                      room(source->width * gridScaleX, sourceX) });
    const uint32_t height = std::min({ rect.extent.height, room(surfaceHeight, destY),
                                       room(source->height * gridScaleY, sourceY) });
    Resolved& into = ResolveCopyFor(destination);
    const bool firstThisFrame = into.writtenFrame != g.frames;
    // The copy converts to the destination's format, as the console's resolve
    // does. PES 2017 lights its scene into a 7e3 target (half floats here, up to
    // about 32) and resolves it to 8_8_8_8 for the post chain, which counts on
    // the [0, 1] that conversion leaves: kept as half floats, the bright pass and
    // the tone map read the unclamped scene and the 3D view came out white. The
    // blit does the conversion and the clamp.
    const bool floatSource = source->format == VK_FORMAT_R16G16B16A16_SFLOAT ||
                             source->format == VK_FORMAT_R16G16_SFLOAT ||
                             source->format == VK_FORMAT_R32_SFLOAT ||
                             source->format == VK_FORMAT_R32G32_SFLOAT;
    VkFormat copyFormat = source->format;
    if (floatSource && destInfo.NumberFormat() == 0)
    {
        if (destInfo.Format() == 6) copyFormat = VK_FORMAT_R8G8B8A8_UNORM;                 // 8_8_8_8
        else if (destInfo.Format() == 7) copyFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // 2_10_10_10
    }
    // A copy written already this frame keeps its scale, the blit rescaling into it: a new scale
    // is a new image, and the title may read what is there. FIFA Street's match setup resolves
    // the whole frame at 1x, then its scene-scaled predicated tiles into the same copy, and the
    // right tile's backdrop samples the left one's resolve target -- recreated at 0.75, the right
    // half of the screen was empty on the phone.
    const float destScale = (into.image && !firstThisFrame) ? into.scale : source->scale;
    if (!destination || !width || !height ||
        !MakeResolveImage(into, surfaceWidth, surfaceHeight, copyFormat, false,
                          firstThisFrame, destScale))
    {
        Skip("resolve destination image");
        SubmitIfFilling();
        return;
    }
    // Written already this frame, and still the same image: the other predicated tile's half is in it.
    const bool keepContents = into.writtenFrame == g.frames;
    into.writtenFrame = g.frames;
    into.from = sourceKey;
    into.guestFormat = destInfo.Format();
    into.writtenBack = false;
    // What the destination's memory holds now, to tell later whether the title wrote there.
    into.memoryBytes = std::min(surfaceWidth * surfaceHeight * 4u, 8u << 20);
    into.memorySignature = SampleGuestMemory(destination, into.memoryBytes);
    {
        // Every destination's first resolve, for working out what the title
        // composes on the GPU and reads back as a texture (kits, previews).
        static std::unordered_set<uint32_t> seen;
        if (seen.size() < 4000 && seen.insert(destination).second)
            LOGI("resolve: first to %08X: %ux%u at %u,%u of a %ux%u surface (dest pitch reg %08X info %08X)"
                 " from tile %u pitch %u fmt %u x%u, frame %llu",
                 destination, width, height, destX, destY, surfaceWidth, surfaceHeight,
                 r[gpu::RB_COPY_DEST_PITCH], r[gpu::RB_COPY_DEST_INFO], sourceKey.baseTile, sourceKey.pitch,
                 uint32_t(colour.Format()), sourceKey.samples, (unsigned long long)g.frames);
    }
    if (g_dumpDraws)
    {
        LOGW("DUMP resolve after draw %u: tile %u pitch %u EDRAM fmt %u x%u -> %08X %ux%u at %u,%u (surface %ux%u);"
             " dest fmt %u number %u exp bias %d swap %d; colour exp bias %d",
             g_dumpIndex, sourceKey.baseTile, sourceKey.pitch, uint32_t(colour.Format()), sourceKey.samples,
             destination, width, height, destX, destY, surfaceWidth, surfaceHeight, destInfo.Format(),
             destInfo.NumberFormat(), destInfo.ExpBias(), destInfo.Swap() ? 1 : 0, colour.ExpBias());
    }
    VkImage sourceImage = BeginReadingColour(*source,
                                             { int32_t(sourceX / gridScaleX), int32_t(sourceY / gridScaleY) },
                                             (width + gridScaleX - 1) / gridScaleX,
                                             (height + gridScaleY - 1) / gridScaleY);
    if (!sourceImage) { Skip("resolve of a multisampled surface"); SubmitIfFilling(); return; }
    // After the draws that sampled this copy's last contents. A second resolve into the same
    // surface this frame (the other predicated tile) keeps what the first one wrote.
    RecordBarrier(into.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  keepContents ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT, kShaderStages, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit copy{};
    copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    const float s = source->scale;
    copy.srcOffsets[0] = Scaled(int32_t(sourceX / gridScaleX), int32_t(sourceY / gridScaleY), 0, s);
    copy.srcOffsets[1] = Scaled(int32_t((sourceX + width) / gridScaleX),
                                int32_t((sourceY + height) / gridScaleY), 1, s);
    copy.dstSubresource = copy.srcSubresource;
    copy.dstOffsets[0] = Scaled(int32_t(destX), int32_t(destY), 0, destScale);
    copy.dstOffsets[1] = Scaled(int32_t(destX + width), int32_t(destY + height), 1, destScale);
    // Rounded at a fractional scale: kept inside both images.
    copy.srcOffsets[1].x = std::min(copy.srcOffsets[1].x, int32_t(HostOf(source->width, s)));
    copy.srcOffsets[1].y = std::min(copy.srcOffsets[1].y, int32_t(HostOf(source->height, s)));
    copy.dstOffsets[1].x = std::min(copy.dstOffsets[1].x, int32_t(HostOf(into.width, destScale)));
    copy.dstOffsets[1].y = std::min(copy.dstOffsets[1].y, int32_t(HostOf(into.height, destScale)));
    const VkImage intoImage = into.image;
    Record([=](VkCommandBuffer command) {
        vkCmdBlitImage(command, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       intoImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy,
                       VK_FILTER_LINEAR);
    });

    // The corner a smaller fetch of this surface reads (ResolveRing::exact),
    // copied while the image is still a transfer destination's.
    ResolveRing& ring = g.resolvedTo[destination];
    const uint32_t exactWidth = std::min(ring.fetchWidth, into.width);
    const uint32_t exactHeight = std::min(ring.fetchHeight, into.height);
    Resolved* exact = nullptr;
    if (exactWidth && exactHeight && (exactWidth < into.width || exactHeight < into.height))
    {
        exact = &ring.exact[ring.newest];
        if (!MakeResolveImage(*exact, exactWidth, exactHeight, copyFormat, false, true, into.scale)) exact = nullptr;
    }
    if (exact)
    {
        RecordBarrier(into.image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        RecordBarrier(exact->image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT, kShaderStages, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy corner{};
        corner.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        corner.dstSubresource = corner.srcSubresource;
        corner.extent = { HostOf(exactWidth, into.scale), HostOf(exactHeight, into.scale), 1 };
        const VkImage intoImage = into.image, exactImage = exact->image;
        Record([=](VkCommandBuffer command) {
            vkCmdCopyImage(command, intoImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           exactImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &corner);
        });
        RecordBarrier(exact->image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, kShaderStages);
        exact->writtenFrame = g.frames;
    }
    RecordBarrier(into.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  exact ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  exact ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, kShaderStages);
    EndReadingColour(*source);

    // In a frame dump, the resolve as it was at this point of the frame, for the crowd's two G-buffers:
    // the same address is resolved to again later, and the end-of-frame images only keep the last.
    // On the phone also the match's post-processing inputs: the scene (044D0000), the bloom's bright
    // pass (06C80000) and its sum (06D30000). Not every resolve: a mid-frame Submit(true) for each
    // made the Adreno recorder fault in vkCmdClearColorImage.
    // On a desktop, every 8_8_8_8 resolve at least 320 wide: where a full-screen pass goes dark.
#ifdef PES17_ANDROID
    if (g_dumpDraws && !exact && (destination == 0x07EB0000u || destination == 0x08370000u || destination == 0x075D0000u ||
                         destination == 0x044D0000u || destination == 0x06D30000u || destination == 0x06C80000u ||
                         destination == 0x05C20000u) && into.format == VK_FORMAT_R8G8B8A8_UNORM)
#else
    if (g_dumpDraws && !exact && into.width >= 320 && into.format == VK_FORMAT_R8G8B8A8_UNORM)
#endif
    {
        Submit(true);
        const std::vector<uint8_t> pixels = ReadColourImage(
            into.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, into.width, into.height, 4);
        if (!pixels.empty())
        {
            const std::string dir = DebugFilesDir() + "/frame_images";
            mkdir(dir.c_str(), 0775);
            char name[96];
            std::snprintf(name, sizeof name, "/at_%08X_%u", destination, g_dumpIndex);
            if (std::FILE* out = std::fopen((dir + name + ".pnm").c_str(), "wb"))
            {
                std::fprintf(out, "P6\n%u %u\n255\n", into.width, into.height);
                for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::fwrite(&pixels[i], 1, 3, out);
                std::fclose(out);
            }
            if (std::FILE* out = std::fopen((dir + name + "_a.pgm").c_str(), "wb"))
            {
                std::fprintf(out, "P5\n%u %u\n255\n", into.width, into.height);
                for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::fwrite(&pixels[i + 3], 1, 1, out);
                std::fclose(out);
            }
        }
    }

    // (Up to 1024x1024: an unlicensed team's kit pattern is drawn on the GPU at that size and resolved,
    // and the title's CPU converts it to the 5_6_5 texture the kit is composed from. Held to 512 it read
    // zeros and every unlicensed kit came out black.)
    // Write-back. The console's resolve writes guest memory, and PES 2017 builds
    // its kits that way: each player's shirt, numbers and name are drawn and
    // resolved once during the load, and the textures the match then fetches are
    // made from those bytes. Here a resolve stays on the GPU, so the kits came out
    // of zeroed memory -- black players. Small 8_8_8_8 resolves are written back
    // the first couple of times their destination is used: synchronously, since
    // the title reads them as soon as its fence passes, which the command
    // processor signals without waiting for this GPU.
    // The crowd's colour atlas is the other case: the title draws 256x2048 swatches, resolves them, and
    // packs the memory into the DXT1 atlas on the CPU, so a resolve that tall has to land in memory too
    // (held to 1024 it read zeros and the whole crowd was black). Area, not each side, is what costs.
    if (destInfo.Format() == 6 && into.format == VK_FORMAT_R8G8B8A8_UNORM &&
        surfaceWidth <= 2048 && surfaceHeight <= 2048 &&
        uint64_t(surfaceWidth) * surfaceHeight <= 1024u * 1024u && g.scale == 1 && into.scale == 1.0f)
    {
        // The first 64 resolves of a destination. The kits reuse their scratch
        // destinations player after player -- a few dozen times in a load, in
        // bursts that any rule about how recently a destination was used took
        // for a screen effect, leaving one player or another in black. Screen
        // effects are resolved every frame, thousands of times a match, and stop
        // costing a stall once their 64 are spent.
        // And once a destination has been resolved in back-to-back frames eight
        // times it is a per-frame effect, and is left alone sooner: each
        // write-back stalls the CPU until the GPU is idle, and 697 of them at the
        // start of a match took the frame rate with them.
        struct Use { uint32_t times = 0, consecutive = 0; uint64_t frame = UINT64_MAX; };
        // Back-to-back frames before a destination counts as a per-frame effect. The match
        // intro opens with a dozen new ones at once (a 640x360 and 320x180 chain, the crowd's
        // 64x1664 to 16x416 atlases); at eight frames each that was 100-odd stalls, 740 ms of
        // them in its first eight frames, and the intro fell to 7 fps.
        constexpr uint32_t kPerFrameAfter = 3;
        static std::unordered_map<uint32_t, Use> uses;
        Use& use = uses[destination];
        if (use.frame + 1 == g.frames) use.consecutive++;
        use.frame = g.frames;
        uint32_t& times = use.times;
        // files/writeback_always: hex destinations written back on every resolve whatever the cap says.
        const bool always = std::find(g_writeBackAlways.begin(), g_writeBackAlways.end(), destination) !=
                            g_writeBackAlways.end();
        // FIFA Street resolves a 512x512 image to 05C20000 every frame and its CPU reads the bytes back
        // for the courts' lighting (the PC port's "Full readback" covers it). Capped, the guest copy kept
        // whatever the first dozen frames held and the Futsal Centre's floor was a third as bright as on
        // the console. Written back every time: about 5 fps on the S20 FE.
        const bool courtLighting = destination == 0x05C20000u && surfaceWidth == 512 && surfaceHeight == 512;
        const bool everyFrame = !g_noWriteBackCap && !always && !courtLighting &&
                                (times >= 12 || use.consecutive >= kPerFrameAfter);
        static std::unordered_map<uint32_t, bool> toldStop;
        if (everyFrame && !toldStop[destination])
        {
            toldStop[destination] = true;
            LOGI("renderer: write-back stops for %08X (%ux%u at %u,%u) after %u resolves, %u frames in a row",
                 destination, width, height, destX, destY, times, use.consecutive);
        }

        times++;
        const uint32_t va = kernel::FromPhysical(destination);
        if (!everyFrame && va)
        {
            const gpu::CopyDestPitch pitchInfo{ r[gpu::RB_COPY_DEST_PITCH] };
            QueueWriteBack(into, destination, va, destX, destY, width, height,
                           (std::max(pitchInfo.Pitch(), 1u) + 31u) & ~31u, pitchInfo.Pitch(),
                           destInfo.Swap(), destInfo.Endian());
        }
    }
    ClearAfterResolve(r, control);
    if (!SubmitIfFilling()) Skip("frame submission failed");
}

// The frame is whatever the title swaps to: the front buffer the swap packet
// names, which a resolve filled like any other destination. A resolve the size
// of the display is not the frame for being that size -- the title keeps the
// screen in one for the next frame's blur, before the interface is drawn.
static int g_captureBurst = 0;   // consecutive frames still to dump after a CAPTURE

static std::atomic<bool> g_cacheFlushRequested{ false };
void vk::renderer::RequestCacheFlush() { g_cacheFlushRequested.store(true, std::memory_order_release); }

void vk::renderer::Swap(uint32_t frontBuffer)
{
    if (!g.device) return;
    // A frame's write-backs are in guest memory before the next frame starts.
    detail::PublishWriteBacksNow();
    if (g_cacheFlushRequested.exchange(false, std::memory_order_acq_rel)) vk::renderer::detail::FlushShaderCache();
    {
        using namespace vk::renderer::detail;
#ifdef PES17_ANDROID
        // The capture button (GameActivity writes files/capture_request): keep the
        // last frames' draw counts and times, and on a request print them with the
        // frame number, then dump the next frame, so a flicker the user just saw can
        // be matched to what the renderer did around it.
        {
            struct FrameNote { uint32_t swap, draws; uint32_t ms; };
            static FrameNote ring[240];
            static uint32_t count = 0;
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            ring[count % 240] = { count, g_frameDraw,
                uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count()) };
            last = now; count++;
            // Once a second: what tools/live.py watches (frame rate, worst frame, draw counts,
            // and how many frames in a row had the same draw count -- a frozen scene).
            {
                static uint32_t swaps = 0, worst = 0, dMin = ~0u, dMax = 0, same = 0, lastDraws = ~0u;
                static auto windowStart = now;
                const uint32_t ms = ring[(count - 1) % 240].ms;
                swaps++; worst = std::max(worst, ms);
                dMin = std::min(dMin, g_frameDraw); dMax = std::max(dMax, g_frameDraw);
                same = (g_frameDraw == lastDraws) ? same + 1 : 0; lastDraws = g_frameDraw;
                if (now - windowStart >= std::chrono::seconds(1))
                {
                    LOGW("tele: fps %u worst %u draws %u-%u same %u swap %u", swaps, worst, dMin, dMax, same, count);
                    swaps = 0; worst = 0; dMin = ~0u; dMax = 0; windowStart = now;
                }
            }
            static uint32_t polled = 0;
            if (++polled % 6 == 0)
            {
                const std::string request = android::GetPaths().external + "/capture_request";
                if (access(request.c_str(), F_OK) == 0)
                {
                    unlink(request.c_str());
                    LOGW("capture: requested at swap %u; the last %u frames (swap draws ms):", count, std::min(count, 240u));
                    std::string line;
                    for (uint32_t i = count > 240 ? count - 240 : 0; i < count; i++)
                    {
                        const FrameNote& n = ring[i % 240];
                        char t[40]; std::snprintf(t, sizeof t, " %u:%u:%u", n.swap, n.draws, n.ms);
                        line += t;
                        if (line.size() > 700) { LOGW("capture:%s", line.c_str()); line.clear(); }
                    }
                    if (!line.empty()) LOGW("capture:%s", line.c_str());
                    kernel::DumpYieldStacks(int(count % 1000) + 1);   // a fresh budget per capture
                    crash::DumpAllThreads("CAPTURE button");   // where every guest thread is (for a hang)
                    g_captureBurst = 8;
                    if (FILE* f = std::fopen((android::GetPaths().external + "/dump_frame").c_str(), "w")) std::fclose(f);
                }
            }
        }
#endif
        g_frameDraw = 0;
        static uint32_t tick = 0;
        if (++tick % 20 == 0)
        {
            unsigned lo = 0, hi = 0xFFFFFFFFu;
#ifdef PES17_ANDROID
            if (FILE* f = std::fopen((android::GetPaths().external + "/draw_range").c_str(), "r"))
            {
                if (std::fscanf(f, "%u %u", &lo, &hi) < 1) { lo = 0; hi = 0xFFFFFFFFu; }
                std::fclose(f);
            }
#endif
            g_drawLo = lo; g_drawHi = hi;
            g_noWriteBackCap = access((DebugFilesDir() + "/no_writeback_cap").c_str(), F_OK) == 0;
            {
                std::vector<uint32_t> always;
                if (std::FILE* f = std::fopen((DebugFilesDir() + "/writeback_always").c_str(), "r"))
                {
                    unsigned value = 0;
                    while (std::fscanf(f, "%x", &value) == 1) always.push_back(value);
                    std::fclose(f);
                }
                g_writeBackAlways = std::move(always);
            }
            g_noVertexReuse = access((DebugFilesDir() + "/no_vertex_reuse").c_str(), F_OK) == 0;
            kernel::g_traceAll.store(access((DebugFilesDir() + "/trace_all").c_str(), F_OK) == 0, std::memory_order_relaxed);
        }
    }
#ifdef PES17_ANDROID
    {
        static uint32_t swaps = 0;
        if (++swaps % 120 == 0)
        {
            std::vector<uint64_t> skip;
            if (FILE* f = std::fopen((android::GetPaths().external + "/skip_ps").c_str(), "r"))
            {
                unsigned long long hash = 0;
                while (std::fscanf(f, "%llx", &hash) == 1) skip.push_back(hash);
                std::fclose(f);
            }
            if (skip != vk::renderer::detail::g_skipPixelShaders) LOGW("skip_ps: %zu shaders", skip.size());
            vk::renderer::detail::g_skipPixelShaders = std::move(skip);
        }
    }
#endif
    // Frame dump: a file named dump_frame in the app's folder turns the draw log
    // on for exactly one frame (this swap to the next).
    {
        static bool armed = false, samplesDue = false;
        // The dumped frame was submitted at the end of the last swap: its images
        // are read now, before this frame's work goes to the queue.
        if (samplesDue) { samplesDue = false; vk::renderer::detail::LogTargetSamples(); LOGW("DUMP samples end"); }
        if (armed)
        {
            const bool lite = vk::renderer::detail::g_dumpHeaders;
            vk::renderer::detail::g_dumpDraws = false; vk::renderer::detail::g_dumpHeaders = false; armed = false;
            if (!lite)
            {
                vk::renderer::detail::SnapshotForSamples();
                samplesDue = true;
            }
            LOGW("DUMP frame ends");
#ifdef PES17_ANDROID
            if (g_captureBurst > 0 && --g_captureBurst > 0)
                if (FILE* f = std::fopen((android::GetPaths().external + "/dump_frame").c_str(), "w")) std::fclose(f);
#endif
        }
        else if (vk::renderer::detail::DumpRequested())
        {
            // A CAPTURE burst logs only the draws: the full dump costs about two seconds a frame.
            if (g_captureBurst > 0) vk::renderer::detail::g_dumpHeaders = true;
            else vk::renderer::detail::g_dumpDraws = true;
            vk::renderer::detail::g_dumpIndex = 0; armed = true;
            LOGW("DUMP frame begins (swap to %08X)", frontBuffer);
        }
    }
    // Live statistics, always on: what the renderer has done since the last
    // line (on the GPU thread, so the counters need no lock). Draws against
    // draws recorded, and why the rest were skipped, say whether a black or
    // blank screen is nothing being drawn or something being drawn wrongly.
    {
        static uint64_t swaps = 0, lastDraws = 0, lastRecorded = 0, lastResolves = 0;
        static std::map<const char*, uint64_t> lastSkipped;
        if (++swaps % 600 == 0)
        {
            std::string skipped;
            char item[160];
            for (const auto& [why, count] : g.skipped)
                if (count > lastSkipped[why])
                {
                    std::snprintf(item, sizeof item, " [%s x%llu]", why, (unsigned long long)(count - lastSkipped[why]));
                    skipped += item;
                    lastSkipped[why] = count;
                }
            static auto lastAt = std::chrono::steady_clock::now();
            const auto nowAt = std::chrono::steady_clock::now();
            const double took = std::chrono::duration<double>(nowAt - lastAt).count();
            lastAt = nowAt;
            static uint64_t lastFence = 0;
            const uint64_t fence = g.fenceNanoseconds;
            LOGI("perf: %.1f fps over the last 600 swaps (%.1f ms a frame); vertex data %.1f MB copied, %.1f MB reused a frame;"
                 " %.2f ms a frame waiting for the GPU; %llu write-backs",
                 600.0 / took, took * 1000.0 / 600.0, g.vertexBytesCopied / 600.0 / 1048576.0,
                 g.vertexBytesReused / 600.0 / 1048576.0, (fence - lastFence) / 600.0 / 1e6,
                 (unsigned long long)g.windowWriteBacks);
            {
                // The process's CPU time (user + system, every thread) over the window, a frame, and
                // whether the crowd was drawn in it (the flip is made after this report's frame).
                static double lastCpu = 0.0;
                rusage usage{};
                getrusage(RUSAGE_SELF, &usage);
                const double cpu = double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                                   double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
                LOGI("perf: cpu %.2f ms a frame (%.0f%% of a core), crowd %s", (cpu - lastCpu) * 1000.0 / 600.0,
                     (cpu - lastCpu) / took * 100.0, CrowdOff() ? "off" : "on");
                lastCpu = cpu;
            }
            {
                FenceWaits& waits = gpu::detail::g_fenceWaits;
                const uint64_t calls = waits.calls.exchange(0), ns = waits.nanoseconds.exchange(0);
                const uint64_t sleeps = waits.sleeps.exchange(0), slept = waits.sleptNs.exchange(0);
                const uint64_t yields = waits.yields.exchange(0);
                LOGI("perf: title fence waits %.1f a frame, %.2f ms a frame; inside them %.1f sleeps"
                     " (%.3f ms asked each) and %.1f yields a frame",
                     calls / 600.0, ns / 600.0 / 1e6, sleeps / 600.0, sleeps ? slept / double(sleeps) / 1e6 : 0.0,
                     yields / 600.0);
            }
            lastFence = fence;
            g.vertexBytesCopied = g.vertexBytesReused = 0;
            g.windowWriteBacks = 0;
            LOGI("renderer: last 600 swaps: %llu draws (%llu recorded), %llu resolves; skipped:%s",
                 (unsigned long long)(g.draws - lastDraws), (unsigned long long)(g.drawsRecorded - lastRecorded),
                 (unsigned long long)(g.resolves - lastResolves), skipped.empty() ? " nothing" : skipped.c_str());
            lastDraws = g.draws; lastRecorded = g.drawsRecorded; lastResolves = g.resolves;
        }
    }
    auto found = g.resolvedTo.find(frontBuffer & ~0xFFFu);
    if (found == g.resolvedTo.end())
    {
        // Nothing is presented for this swap. Say so the first few times, with
        // what was resolved, since that is the whole question.
        static uint64_t misses = 0;
        if (misses++ % 300 < 2)
        {
            std::string known;
            char item[16];
            for (const auto& [address, ring] : g.resolvedTo)
            {
                std::snprintf(item, sizeof item, " %08X", address);
                known += item;
            }
            LOGW("renderer: swap to %08X, which no resolve wrote; resolve destinations:%s",
                 frontBuffer, known.empty() ? " none" : known.c_str());
        }
        return;
    }
    Resolved& frame = found->second.copies[found->second.newest];
    static bool presentedOnce = false;
    if (!presentedOnce) { presentedOnce = true; LOGI("renderer: first frame presented, from the resolve at %08X", frontBuffer); }
    if (!frame.image || frame.depth) return;
    if (!g.recording && !BeginFrame()) return;
    EndPass();
    if (Target* source = FindTarget(frame.from)) source->presented++;

    // Copying the frame into an image of its own, rather than handing the
    // resolve's to the window thread, keeps the next frame from overwriting it.
    const uint32_t index = g.presentIndex;
    VkImage present = g.present[index];
    vk::WaitUntilTaken(g.presentSerial[index]);

    // After every write to the resolve, whichever stage made it: chained only from the
    // shader stages, FIFA Street's front buffer (an 8_8_8_8 resolve) was blitted before
    // the copy into it landed, and every presented frame came out black -- while the same
    // blit alone, a moment later, showed the picture.
    RecordBarrier(frame.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // After the window's copy out of it, and the dumps' and the display
    // table's work on it, the last time round.
    RecordBarrier(present, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = Scaled(int32_t(frame.width), int32_t(frame.height), 1, frame.scale);
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = { int32_t(g.presentWidth), int32_t(g.presentHeight), 1 };
    const VkImage frameImage = frame.image;
    Record([=](VkCommandBuffer command) {
        vkCmdBlitImage(command, frameImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       present, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    });
    RecordBarrier(frame.image, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, kShaderStages);

    // What a television is handed has been through the display's colour table,
    // so what the window and the frame dumps show has to be too.
    vk::display::Apply(present, g.presentWidth, g.presentHeight,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    RecordBarrier(present, VK_IMAGE_ASPECT_COLOR_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // (In $PES17_FILES/frame_images on a desktop.)
    // In a frame dump, the image the window is handed as well (frame_images/present.pnm):
    // what the swap made of the resolve, after the blit and the display's colour table.
    if (vk::renderer::detail::g_dumpDraws)
    {
        Submit(true);
        const std::vector<uint8_t> pixels = ReadColourImage(present, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                            g.presentWidth, g.presentHeight, 4);
        const std::string dir = DebugFilesDir() + "/frame_images";
        mkdir(dir.c_str(), 0775);
        if (std::FILE* out = std::fopen((dir + "/present.pnm").c_str(), "wb"))
        {
            std::fprintf(out, "P6\n%u %u\n255\n", g.presentWidth, g.presentHeight);
            for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::fwrite(&pixels[i], 1, 3, out);
            std::fclose(out);
        }
        LOGW("DUMP present image %ux%u from resolve %08X (%ux%u scale %.2f, vk fmt %d): %zu bytes read",
             g.presentWidth, g.presentHeight, frontBuffer, frame.width, frame.height, double(frame.scale),
             int(frame.format), pixels.size());
        // And the resolve it was made from, as it was at the swap.
        const uint32_t sw = HostOf(frame.width, frame.scale), sh = HostOf(frame.height, frame.scale);
        const std::vector<uint8_t> source = ReadColourImage(frame.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, sw, sh, 4);
        if (std::FILE* out = std::fopen((dir + "/present_source.pnm").c_str(), "wb"))
        {
            std::fprintf(out, "P6\n%u %u\n255\n", sw, sh);
            for (size_t i = 0; i + 3 < source.size(); i += 4) std::fwrite(&source[i], 1, 3, out);
            std::fclose(out);
        }
        uint64_t lit = 0;
        for (size_t i = 0; i + 3 < source.size(); i += 4) lit += (source[i] | source[i + 1] | source[i + 2]) != 0;
        LOGW("DUMP   its source at the swap: %ux%u, %llu of %zu pixels not black; ring newest %u of %zu",
             sw, sh, (unsigned long long)lit, source.size() / 4, found->second.newest, std::size(found->second.copies));
        // The same blit again, alone in a command buffer of its own: is it the blit, or the frame around it?
        {
            vk::record::Drain();
            VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(g.setup, &begin);
            vk::util::Barrier(g.setup, frame.image, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vk::util::Barrier(g.setup, present, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdBlitImage(g.setup, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           present, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            vk::util::Barrier(g.setup, frame.image, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            vk::util::Barrier(g.setup, present, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            vkEndCommandBuffer(g.setup);
            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &g.setup;
            {
                std::lock_guard lock(vk::pipeline::QueueMutex());
                vkResetFences(g.device, 1, &g.setupFence);
                vkQueueSubmit(g.queue, 1, &submit, g.setupFence);
            }
            vkWaitForFences(g.device, 1, &g.setupFence, VK_TRUE, UINT64_MAX);
            const std::vector<uint8_t> again = ReadColourImage(present, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                               g.presentWidth, g.presentHeight, 4);
            uint64_t litAgain = 0;
            for (size_t i = 0; i + 3 < again.size(); i += 4) litAgain += (again[i] | again[i + 1] | again[i + 2]) != 0;
            uint64_t litFirst = 0;
            for (size_t i = 0; i + 3 < pixels.size(); i += 4) litFirst += (pixels[i] | pixels[i + 1] | pixels[i + 2]) != 0;
            LOGW("DUMP   present lit: %llu after the frame's blit, %llu after the same blit alone (src %d,%d..%d,%d dst %d,%d..%d,%d)",
                 (unsigned long long)litFirst, (unsigned long long)litAgain,
                 blit.srcOffsets[0].x, blit.srcOffsets[0].y, blit.srcOffsets[1].x, blit.srcOffsets[1].y,
                 blit.dstOffsets[0].x, blit.dstOffsets[0].y, blit.dstOffsets[1].x, blit.dstOffsets[1].y);
        }
        if (!BeginFrame()) return;
    }

    FinishFrame(present);
}

#endif  // PES17_HAVE_VULKAN
