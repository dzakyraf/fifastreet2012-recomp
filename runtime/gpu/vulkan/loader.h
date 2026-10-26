#pragma once
// Vulkan through function pointers, for a build that decides at run time
// which implementation to talk to.
//
// A desktop build links the loader and calls vkSomething directly. Android
// cannot: the driver may be one the player imported as a zip (Mesa's Turnip,
// runtime/android/driver.cpp), which the system's loader knows nothing about,
// and the platform's own libvulkan.so exports only the core entry points --
// everything with a KHR or EXT on the end has to be asked for by name anyway.
//
// So every entry point the runtime calls is a pointer with the same name as
// the function, filled in from vkGetInstanceProcAddr. The call sites do not
// change: `vkCmdDraw(...)` is a call through a pointer instead of a call to
// the loader's stub. The list is generated (tools/gen_vulkan_loader.py) from
// the calls the sources actually make.
//
// Device-level entry points are loaded again from vkGetDeviceProcAddr once
// the device exists. That skips the loader's dispatch trampoline on every
// draw call, which is a measurable share of a frame on a phone where the
// renderer submits thousands of them.
//
// This header is force-included into the Vulkan sources by CMake, so it is
// the first thing every one of them sees.
#if defined(PES17_VULKAN_DYNAMIC) && defined(PES17_HAVE_VULKAN)

#ifndef VK_NO_PROTOTYPES
#   define VK_NO_PROTOTYPES 1
#endif
#if (defined(__ANDROID__) || defined(PES17_ANDROID)) && !defined(VK_USE_PLATFORM_ANDROID_KHR)
#   define VK_USE_PLATFORM_ANDROID_KHR 1
#endif

#include <vulkan/vulkan.h>

extern "C"
{
    // The one that is not loaded from anything else: it comes out of the
    // library, and everything else comes out of it.
    extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

#define PES17_VK_GLOBAL(name)   extern PFN_##name name;
#define PES17_VK_INSTANCE(name) extern PFN_##name name;
#define PES17_VK_DEVICE(name)   extern PFN_##name name;
#include "vulkan_functions.inc"
#undef PES17_VK_GLOBAL
#undef PES17_VK_INSTANCE
#undef PES17_VK_DEVICE
}

#include <string>

namespace vk::loader
{
    // Opens the implementation -- the player's driver where there is one,
    // the system's otherwise -- and loads what can be loaded without an
    // instance. Safe to call more than once; only the first does anything.
    bool Open(std::string& error);
    bool Opened();

    // After vkCreateInstance and after vkCreateDevice. Entry points an
    // extension owns come back null when the extension is not there, which
    // is what the callers already check for.
    void LoadInstance(VkInstance instance);
    void LoadDevice(VkDevice device);
    // The device is about to be destroyed: its entry points go back to the
    // instance's, which stay valid.
    void ForgetDevice();

    // What was opened, for the log ("the system's libvulkan.so", a driver's
    // file name).
    const char* Description();
    // How many of the entry points the runtime wants this implementation has.
    // A driver missing one the renderer needs would otherwise crash on a null
    // pointer somewhere far from here.
    uint32_t Missing();
}

#elif defined(PES17_HAVE_VULKAN)
#include <vulkan/vulkan.h>
#endif  // PES17_VULKAN_DYNAMIC && PES17_HAVE_VULKAN
