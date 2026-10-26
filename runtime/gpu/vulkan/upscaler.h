#pragma once
// The window's upscaler: AMD FidelityFX Super Resolution 1 -- EASU, an edge-adaptive upscale, then
// RCAS, a contrast-adaptive sharpen -- from the renderer's 1280x720 frame to the size the window shows
// it at. A bilinear blit (what the presenter did before) leaves a phone's 1920x1080 picture soft.
//
// Off by default (it costs about 5 fps on a throttled phone): PES17_UPSCALER=fsr or `touch files/upscaler_on` turns it on,
// `touch files/upscaler_off` forces it off; PES17_FSR_SHARPNESS is RCAS's
// sharpness reduction in stops, 0 the sharpest (default 0.2, as Xenia).
#include <cstdint>
#include "loader.h"

namespace vk::upscale
{
    // Whether to upscale this frame. Cheap; asked once a present.
    bool Enabled();

    // Records the two passes into `commands`. `source` is in TRANSFER_SRC layout and is left in it.
    // `slot` is the presenter's frame-in-flight index: each slot has its own images and descriptor
    // sets, which are only rewritten once that slot's previous submission has finished. Returns an
    // image of width x height holding the result in TRANSFER_SRC layout, or VK_NULL_HANDLE when the
    // upscaler cannot run (the caller then blits as before).
    VkImage Record(VkDevice device, VkPhysicalDevice physical, VkCommandBuffer commands, uint32_t slot,
                   VkImage source, uint32_t sourceWidth, uint32_t sourceHeight,
                   uint32_t width, uint32_t height);

    // Everything it made. The device must be idle.
    void Destroy(VkDevice device);
}
