#pragma once
#include <cstdio>
#include <filesystem>

// Durable writes. Everything this runtime saves is small, written rarely and
// irreplaceable, and both hosts it runs on can take the process away mid-write:
// Android's low memory killer does it by design, and a lost Vulkan device exits
// outright (gpu/vulkan/renderer.cpp, OnDeviceLost). See durable.cpp for why
// neither fflush nor opening the target for writing is enough.
//
// Deliberately free of any dependency on the guest or the recompiled image, so
// the checks in tests/durable_test.cpp can build against it on its own.
namespace kernel
{
    // Pushes a stream's bytes to storage. fflush alone only moves them into the
    // kernel's page cache, which is written back whenever the kernel chooses.
    bool SyncStream(std::FILE* f);

    // Writes `path` so that losing the process part-way cannot destroy what was
    // there before: a temporary beside it, synced, then renamed over it, with
    // the previous contents kept as "<path>.bak".
    bool WriteFileAtomically(const std::filesystem::path& path, const void* bytes, size_t size);
}
