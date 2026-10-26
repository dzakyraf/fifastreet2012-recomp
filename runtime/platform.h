#pragma once
// The few things the runtime asks of the host system that Linux and Windows
// spell differently. Everything else goes through the C++ library or SDL.
#include <cstddef>
#include <cstdint>

namespace platform
{
    // First thing in main, before any other thread starts: Windows sleeps in
    // 15.6 ms steps unless asked for 1 ms, and the title paces itself with
    // short sleeps; and a release build there finds the terminal it was
    // started from, if any.
    void Initialise();
    // Seconds since the process started (platform::Initialise): the clock the window title shows
    // and PES17_INPUT_SCRIPT times its presses by, so a time read off the window can be scripted.
    double SecondsSinceStart();

    // The system's id for the calling thread, as a profiler shows it.
    uint32_t ThreadId();
    bool IsMainThread();
    // Up to fifteen characters are kept on Linux.
    void SetThreadName(const char* name);
    // Background work that must not take time from the frame.
    void LowerThreadPriority();
    // Only the time nothing else wants.
    void IdleThreadPriority();
    // The threads that feed the sound (the mixer, the title's XMA decoding), once per thread:
    // late, they mix silence into it. Android lets an app give its own threads the audio
    // priority; elsewhere a no-op.
    void AudioThreadPriority();

    enum class ThreadAffinityRole {
        PrimeCore,       // Main recompiled game loop (highest frequency core)
        PerformanceCore, // GPU submit / command recorder / rendering
        WorkerCore,      // Background shader warmup / audio / file I/O
        RenderCore,      // GPU ring consumer / presenter: off the prime core the main thread is on
        Default          // OS scheduler default
    };

    // Pin calling thread to the SoC core cluster appropriate for its role.
    void SetThreadAffinity(ThreadAffinityRole role);

    // The calling thread's host stack to stderr. The recompiled functions are
    // named sub_XXXXXXXX, so where symbols resolve this is the guest's stack.
    void PrintBacktrace(const char* why);

    // A detached host thread with a stack of at least this size. The
    // recompiled functions carry the guest's frames on the host stack, and
    // the guest's are large: a title thread that recurses through a few
    // hundred of them wants several megabytes. glibc gives a new thread 8 MB
    // and the question never comes up; bionic gives it 1 MB, and the run
    // ends in a stack overflow somewhere deep in the recompiled code with
    // nothing to show for it. Returns false only if the thread could not be
    // started at all.
    bool StartThread(void (*body)(void*), void* argument, size_t stackBytes,
                     const char* name);

    // A shared library: loaded, or only if something already loaded it.
    void* OpenLibrary(const char* path);
    void* LoadedLibrary(const char* name);
    void* Symbol(void* library, const char* name);
}
