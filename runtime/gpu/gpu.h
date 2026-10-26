#pragma once
#include <cstdint>

// The GPU, from the kernel's and D3D9's side.
//
// MW2's D3D9 is statically linked, so it is already recompiled and running: it
// builds real PM4 command streams and publishes a ring write pointer. A thread
// here consumes that stream as the console's command processor does, and the
// draws in it are rendered with Vulkan (vulkan/renderer.h).
namespace gpu
{
    void Initialise();
    void Shutdown();

    // The Vd* kernel imports funnel here.
    void SetRingBuffer(uint32_t basePhysical, uint32_t sizeLog2);
    void SetReadPointerWriteBack(uint32_t addressPhysical, uint32_t blockSizeLog2);
    void SetInterruptCallback(uint32_t callback, uint32_t context);
    // VdSwap: one frame submitted.
    void Swap();
    // Frames the title has submitted (VdSwap calls).
    uint64_t FrameCount();
    // Swap packets the command processor has executed (FrameCount counts the
    // CPU's VdSwap calls).
    uint64_t SwapsExecuted();
    // Resolve write-back (vulkan/renderer_resolve.cpp) calls this; for a while
    // afterwards the title's kicks wait for the ring to be consumed.
    void NoteWriteBack();
    // After D3D kicks the ring: during that window, wait until the command
    // processor has executed what was submitted.
    void WaitAfterKick();

    // True while a command processor is consuming the ring, so work that must
    // happen at a point in the stream can be left to it.
    bool Consuming();

    // After each of the title's presents.
    void Presented();

    // The block D3D9 polls for the command processor's progress through its
    // command-buffer arena: a position at +4 and a sequence counter at +0.
    void SetArenaProgressBlock(uint32_t address);
    uint32_t GetArenaProgressBlock();
    uint32_t GetArenaSequence();
    void ForceArenaProgress(uint32_t sequence, uint32_t position);
    bool CheckArenaWaitDeadlock(uint64_t maxWaitUs, uint32_t& outSeq, uint32_t& outPos);

    // The display's 256-entry colour table, as the scanout hardware holds it:
    // ten bits per channel, blue in the low bits. False until the title has
    // filled it. Read from the presenting thread, written by the ring consumer.
    bool DisplayColourTable(uint32_t out[256]);

    void Report();
}
