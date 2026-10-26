#pragma once
// A 3-second window (from 70 s after start) in which every wait, signal, read
// and sleep is logged with a microsecond timestamp, for reconstructing who waits
// for whom. Off the rest of the time; cheap to test.
#include <chrono>
#include <cstdint>
namespace kernel
{
    bool WindowTraceOn();
    void WindowTrace(const char* kind, const char* thread, uint32_t a, uint32_t b, const char* note);
}
