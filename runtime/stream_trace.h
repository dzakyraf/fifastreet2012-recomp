#pragma once
#include <cstdint>

// How long the title's own streaming takes to turn a file read into a texture
// on screen -- the face and kit popping (PES17_STREAM_TRACE=1 or
// files/stream_trace). Three kinds of `stream:` line, all stamped in ms since
// the first one:
//   read  -- a guest file read: thread, core, file, bytes, time in fread
//   tex   -- a texture first uploaded or re-staged after a rewrite (64x64 and up)
//   sched -- every 500 ms, each busy thread's ms running and ms runnable but
//            waiting for a core (sampled from /proc/self/task/*/stat)
// Off, every call is one load and a branch.
namespace stream_trace
{
    bool On();
    void Read(const char* file, uint32_t bytes, uint32_t got, double freadMs, uint32_t buffer);
    void Texture(const char* kind, uint32_t address, uint32_t width, uint32_t height, uint32_t format);
}
