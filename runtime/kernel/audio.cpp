// XAudio and XMA -- the kernel's side of sound. The mixer itself is XAudio2,
// statically linked into the title and so already recompiled; what it needs
// from the kernel is a render driver to pace it and hand its frames to, and
// hardware decoder contexts for its XMA voices. Both live in runtime/apu.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "physical.h"
#include "../guest.h"
#include "../log.h"
#include "../apu/audio.h"
#include "../apu/xma.h"
#include <atomic>
#include <cstdlib>
#include "trace.h"

namespace
{
    constexpr uint32_t X_E_INVALIDARG = 0x80070057;
}

// XAudioRegisterRenderDriverClient(const struct { callback; context; }* client, uint32_t* driverOut)
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    auto* client = GuestPtr<be32>(ctx.r3.u32);
    auto* driverOut = GuestPtr<be32>(ctx.r4.u32);
    if (!client || !client[0]) { ctx.r3.u64 = X_E_INVALIDARG; return; }
    uint32_t handle = apu::audio::RegisterClient(client[0], client[1]);
    if (!handle) { ctx.r3.u64 = X_E_INVALIDARG; return; }
    if (driverOut) *driverOut = handle;
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    apu::audio::UnregisterClient(ctx.r3.u32);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XAudioSubmitRenderDriverFrame(driver, const float samples[6][256])
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    apu::audio::SubmitFrame(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// The dashboard's per-category volumes; there is no dashboard, so full.
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)
{
    if (auto* out = GuestPtr<be<float>>(ctx.r4.u32)) *out = 1.0f;
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// The mixer only fetches a category's volume when this says it changed, and
// its object starts out zeroed, so the first query has to report every
// category changed or every voice is scaled by zero for the whole run.
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask)
{
    static std::atomic<uint32_t> queries{ 0 };
    if (auto* out = GuestPtr<be32>(ctx.r4.u32)) *out = queries++ == 0 ? 0xFFFFFFFFu : 0;
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XMACreateContext(void** contextOut)
PPC_FUNC(__imp__XMACreateContext)
{
    uint32_t context = apu::xma::AllocateContext();
    if (auto* out = GuestPtr<be32>(ctx.r3.u32)) *out = context;
    ctx.r3.u64 = context ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY;
}

PPC_FUNC(__imp__XMAReleaseContext)
{
    apu::xma::ReleaseContext(ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// The rest of the context calls, as Xenia's xboxkrnl_audio_xma.cc has them. Stubbed, every voice
// stood still: the buffers never read as consumed, the output never moved, and the audio threads
// polled them millions of times a minute -- and the match intro, which waits on its sound, never
// faded in.
namespace
{
    uint32_t Physical(uint32_t address) { return address ? kernel::ToPhysical(address) : 0; }
    using apu::xma::Field;
}

// XMAInitializeContext(context, const XMA_CONTEXT_INIT*)
PPC_FUNC(__imp__XMAInitializeContext)
{
    const uint32_t init = ctx.r4.u32;
    if (!init) { ctx.r3.u64 = X_E_INVALIDARG; return; }
    auto word = [&](uint32_t offset) { return uint32_t(*GuestPtr<be32>(init + offset)); };
    apu::xma::ContextInit c{};
    c.input0 = Physical(word(0));   c.input0Packets = word(4);
    c.input1 = Physical(word(8));   c.input1Packets = word(12);
    c.readOffset = word(16);
    c.output = Physical(word(20));  c.outputBlocks = word(24);
    c.subframeDecodeCount = word(32);
    c.channels = word(36);          c.sampleRate = word(40);
    c.loopStart = word(44);         c.loopEnd = word(48);
    c.loopCount = *GuestPtr<uint8_t>(init + 52);
    c.loopSubframeEnd = *GuestPtr<uint8_t>(init + 53);
    c.loopSubframeSkip = *GuestPtr<uint8_t>(init + 54);
    apu::xma::InitialiseContext(ctx.r3.u32, c);
    ctx.r3.u64 = 0;
}

// (context, buffer, packetCount)
PPC_FUNC(__imp__XMASetInputBuffer0)
{
    apu::xma::SetInputBuffer(ctx.r3.u32, 0, Physical(ctx.r4.u32), ctx.r5.u32);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XMASetInputBuffer1)
{
    apu::xma::SetInputBuffer(ctx.r3.u32, 1, Physical(ctx.r4.u32), ctx.r5.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMAIsInputBuffer0Valid)  { ctx.r3.u64 = apu::xma::Get(ctx.r3.u32, Field::InputBuffer0Valid); }
PPC_FUNC(__imp__XMAIsInputBuffer1Valid)  { ctx.r3.u64 = apu::xma::Get(ctx.r3.u32, Field::InputBuffer1Valid); }
PPC_FUNC(__imp__XMAIsOutputBufferValid)  { ctx.r3.u64 = apu::xma::Get(ctx.r3.u32, Field::OutputBufferValid); }
PPC_FUNC(__imp__XMAGetOutputBufferReadOffset)  { ctx.r3.u64 = apu::xma::Get(ctx.r3.u32, Field::OutputBufferReadOffset); }
PPC_FUNC(__imp__XMAGetOutputBufferWriteOffset) { ctx.r3.u64 = apu::xma::Get(ctx.r3.u32, Field::OutputBufferWriteOffset); }

PPC_FUNC(__imp__XMASetInputBuffer0Valid) { apu::xma::Set(ctx.r3.u32, Field::InputBuffer0Valid, 1); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XMASetInputBuffer1Valid) { apu::xma::Set(ctx.r3.u32, Field::InputBuffer1Valid, 1); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XMASetOutputBufferValid) { apu::xma::Set(ctx.r3.u32, Field::OutputBufferValid, 1); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XMASetInputBufferReadOffset)
{
    apu::xma::Set(ctx.r3.u32, Field::InputBufferReadOffset, ctx.r4.u32);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XMASetOutputBufferReadOffset)
{
    apu::xma::Set(ctx.r3.u32, Field::OutputBufferReadOffset, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// Enabling is a kick: the context decodes now, on this thread.
PPC_FUNC(__imp__XMAEnableContext) { apu::xma::EnableContext(ctx.r3.u32); ctx.r3.u64 = 0; }
// (context, wait)
PPC_FUNC(__imp__XMADisableContext) { apu::xma::DisableContext(ctx.r3.u32); ctx.r3.u64 = 0; }
