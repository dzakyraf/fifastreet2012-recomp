#pragma once
#include <cstdint>
#include <string>

// "What writes this memory?" for the ARM build, where the x86 watchpoint's
// single step is not available. `adb shell "echo 01410000 10 > .../files/watch_phys"`
// (hex physical address, hex page count) makes those pages read-only for the
// next run; each write fault prints the host call stack (guest functions show
// as sub_XXXXXXXX), lets the write through, and the pages are made read-only
// again a moment later. Diagnostics only: the first reports are all it gives.
namespace debugwatch
{
    void ArmFromFile(const std::string& path);
    bool HandleFault(const void* address);
}
