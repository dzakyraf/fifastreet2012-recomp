#include <ppc_recomp_shared.h>
#include "title.h"
#include "guest_memory.h"
#include "guest.h"
#include "log.h"
#include "stutters.h"
#include "diagnostics.h"
#include "env.h"
#include "kernel/kernel.h"
#include "kernel/modules.h"
#include "gpu/gpu.h"
#include "apu/audio.h"
#include "apu/xma.h"
#include "crash.h"
#include "online/service.h"
#include "engine.h"
#include "pacing_trace.h"
#include "player.h"
#include "watchpoint.h"
#include "install/install.h"
#include "platform.h"
#include "run.h"
#include "report.h"
#include "settings.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

// A run that ends on a deadline and a run that ends on a fault are asking the
// same question, so they print the same reports.
static void ReportAll()
{
    report::Write();
    if constexpr (!diag::kOn) return;
    kernel::ReportUnimplemented();
    kernel::ReportMissingFiles();
    kernel::ReportMemory();
    gpu::Report();
    player::Report();
    engine::ReportPredicateWaits();
    pacing::Write();
    apu::xma::Report();
    apu::audio::Report();
}

void crash::OnFault() { ReportAll(); }

// One shutdown however it is asked for, and on a thread of its own: the caller
// may be the window thread, which stopping the presenter has to join.
void crash::RequestExit(const char* why)
{
    static std::atomic<bool> asked{ false };
    if (asked.exchange(true)) return;
    std::thread([why] {
        LOGI("--------------------------------------------------");
        LOGI("exiting: %s", why);
        crash::StopHangMonitor();
        stutters::Ending();
        gpu::Shutdown();
        ReportAll();
        install::StartNextTitle();
        std::fflush(nullptr);
        _exit(0);
    }).detach();
}

namespace
{
    // The guest never returns, so the reports at the end of main() are unreachable
    // and killing the process loses everything measured. PES17_RUN_SECONDS=<n>
    // reports and exits after n seconds.
    void StartDeadline()
    {
        const unsigned seconds = unsigned(diag::Number("PES17_RUN_SECONDS"));
        if (!seconds) return;
        LOGI("run deadline: reporting and exiting after %u seconds", seconds);
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            // The guest threads are still running over state the reports walk;
            // unwinding them cleanly is not worth it for a measurement.
            crash::RequestExit("the run deadline was reached");
        }).detach();
    }

    // PES17_QUIT_AFTER_ARRIVAL=<n> ends the run n seconds after PES17_WALK_TO gets
    // where it was sent. A walk that takes four minutes and a measurement that
    // takes ten seconds should not be followed by two minutes of standing there.
    void StartArrivalDeadline()
    {
        if (!diag::Text("PES17_QUIT_AFTER_ARRIVAL")) return;
        const unsigned seconds = unsigned(diag::Number("PES17_QUIT_AFTER_ARRIVAL"));
        LOGI("run deadline: reporting and exiting %u seconds after arriving", seconds);
        // A walk that has stalled against a wall is as finished as one that
        // arrived -- it will take no more measurements. Ending there rather than
        // at the watchdog saves the rest of the run, which is most of it when
        // the route snags early.
        const double given = diag::Real("PES17_QUIT_IF_STUCK");
        const double stall = given > 0.0 ? given : 45.0;
        LOGI("run deadline: or %0.f seconds after the walk stops getting nearer", stall);
        std::thread([seconds, stall] {
            bool stuck = false;
            while (!player::Arrived())
            {
                if (player::SecondsSinceProgress() > stall) { stuck = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            crash::RequestExit(stuck ? "the walk stopped getting anywhere"
                                     : "the walk arrived and its measurement is done");
        }).detach();
    }

    std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    }

    // PES17_ASPECT=<width/height> of a screen wider than 16:9: the title's
    // cameras are given that aspect, so they see further to the sides, and the
    // presenter shows the 1280x720 frame stretched to it -- the two cancel, and
    // the pitch keeps its shape. The interface is drawn in the frame's pixels
    // and is stretched with it.
    void Widen()
    {
#if defined(T_DATA_Aspect16x9) && defined(T_DATA_Aspect16x9b)
        const double aspect = env::Real("PES17_ASPECT");
        if (aspect <= 16.0 / 9.0 + 0.01 || aspect > 3.0) return;
        const float value = float(aspect);
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        for (uint32_t address : { T_DATA_Aspect16x9, T_DATA_Aspect16x9b })
        {
            be32* word = GuestPtr<be32>(address);
            if (uint32_t(*word) != 0x3FE38E39u)
            {
                LOGW("widescreen: %08X holds %08X, not 16:9; left alone", address, uint32_t(*word));
                continue;
            }
            *word = bits;
        }
        LOGI("widescreen: cameras at aspect %.4f", aspect);
#else
        if (env::Real("PES17_ASPECT") > 0.0) LOGW("widescreen: the aspect floats are not located for this title");
#endif
    }

}

int pes17::Run(int argc, char** argv)
{
    platform::Initialise();

    // Before any switch is read, the log's among them.
    const std::string kept = settings::Load(install::Folder(argc, argv) / ".env");

    // PES17_LOG_FILE=<path> sends the log there instead of the terminal. Every
    // line this runtime writes goes to stderr, so a plain `> file` redirection
    // catches nothing.
    // Its folder is made first: a run started with a new folder named lost its
    // whole log to the terminal.
#ifndef PES17_ANDROID
    if (const char* logPath = env::Text("PES17_LOG_FILE"))
    {
        std::error_code ignored;
        if (const auto folder = std::filesystem::path(logPath).parent_path(); !folder.empty())
            std::filesystem::create_directories(folder, ignored);
        // A title the other one started (from its menus) carries on in the
        // log of the run it belongs to.
        if (!std::freopen(logPath, env::Flag("PES17_LOG_APPEND") ? "a" : "w", stderr))
            std::fprintf(stdout, "could not open %s for the log\n", logPath);
        // A line at a time, as stderr is to a terminal: the file is read while
        // the run goes on, and a line held back is a line a watcher never sees.
        // Windows has no line buffering (and takes the request for an invalid
        // one), so there every write goes straight through.
        else
#ifdef _WIN32
            std::setvbuf(stderr, nullptr, _IONBF, 0);
#else
            std::setvbuf(stderr, nullptr, _IOLBF, 0);
#endif
    }
#endif  // PES17_ANDROID

    LOGI("--- %s recompilation runtime ---", PES17_TITLE_NAME);
    if (!kept.empty()) LOGI("settings: %s", kept.c_str());

    // A player starts the executable with no arguments (runtime/install/); a
    // development run names the image, a flat PE or the XEX, and the game folder.
    install::Launch launch;
    if (install::PlayerStart(argc, argv))
    {
        int exitCode = 0;
        if (!install::Prepare(argc, argv, launch, exitCode)) return exitCode;
    }
    else
    {
        if (!install::LoadImageFile(argv[1], launch.image)) return 1;
        launch.gameRoot = (argc > 2) ? argv[2] : "pes/game";
    }

    crash::Install();
    // Logged in before any other thread runs: Steam reads the app id from the
    // environment, which the service sets.
    online::Get();

    // The executable's XEX itself, for its header: what LoadImageFile or the
    // installer decoded is only the image.
    const std::filesystem::path xexPath = install::PlayerStart(argc, argv)
        ? std::filesystem::path(launch.gameRoot) / "default.xex" : std::filesystem::path(argv[1]);
    const std::vector<uint8_t> xexFile = ReadFile(xexPath);

    if (!guest::Initialise()) return 1;
    // default.xex now; fifadllzf.xex.dll when it asks for it (XexLoadImage, kernel/misc.cpp).
    if (!kernel::modules::Map(kernel::modules::kExecutable, xexFile, launch.image))
    {
        LOGE("could not map %s", xexPath.string().c_str());
        return 1;
    }
    launch.image = {};
    Widen();
    watchpoint::Install();

    crash::RegisterThread("main thread");
    platform::SetThreadAffinity(platform::ThreadAffinityRole::PrimeCore);
    player::Record();
    kernel::Initialise(launch.gameRoot);
    gpu::Initialise();
    apu::xma::Initialise();

    // PPC grows down; leave a zeroed backchain at the top.
    constexpr uint32_t kStackSize = 1u << 20;
    uint32_t stackBase = kernel::AllocateGuest(kStackSize, 0x1000);
    if (!stackBase) { LOGE("could not allocate guest stack"); return 1; }
    uint32_t stackTop = (stackBase + kStackSize - 0x100) & ~0xFu;

    // r13 is the Xbox 360 thread pointer (KPCR). The recompiled code makes ~1100
    // r13-relative accesses and reads its own thread id out of
    // KPCR.CurrentThread -> KTHREAD.ThreadId, so the block must be populated.
    uint32_t pcr = kernel::CreateThreadBlock(1);
    if (!pcr) { LOGE("could not allocate the thread block"); return 1; }

    PPCContext ctx{};
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.r1.u32 = stackTop;
    ctx.r13.u32 = pcr;
    ctx.fpscr.loadFromHost();

    uint32_t entry = T_ENTRY_POINT;   // the XEX header's ENTRY_POINT, from title.h
    PPCFunc* fn = kernel::GuestFunction(entry);
    if (!fn) { LOGE("no recompiled function at entry point %08X", entry); return 1; }

    LOGI("thread block (r13) at %08X", pcr);
    StartDeadline();
    StartArrivalDeadline();
    LOGI("entering guest at %08X (stack %08X..%08X)", entry, stackBase, stackTop);
    LOGI("--------------------------------------------------");
    fn(ctx, guest::Base());
    LOGI("--------------------------------------------------");
    LOGI("guest returned, r3 = %08X", ctx.r3.u32);

    gpu::Shutdown();
    ReportAll();
    return 0;
}

// A desktop build starts here; Android starts the same run from jni.cpp.
#ifndef PES17_ANDROID
#include "gpu/debug_watch.h"
namespace
{
    // The touch-a-file switches in $PES17_FILES (dump_guest, report_memory, check_heap), polled as
    // jni.cpp polls them on a phone. watch_phys itself needs the phone's fault handler.
    void PollFileSwitches()
    {
        if (const char* dir = std::getenv("PES17_FILES")) debugwatch::ArmFromFile(std::string(dir) + "/watch_phys");
    }
}
#ifdef __APPLE__
// Cocoa wants the window on the process's main thread, so the guest runs on a thread of its own
// (with room for the recompiled code's deep host stack) and the main thread serves the presenter.
#include <pthread.h>
#include "gpu/vulkan/presenter.h"
namespace
{
    struct GuestArgs { int argc; char** argv; int code; std::atomic<bool> done{ false }; };
    void* GuestThread(void* p)
    {
        auto* a = static_cast<GuestArgs*>(p);
        a->code = pes17::Run(a->argc, a->argv);
        a->done = true;
        return nullptr;
    }
}
int main(int argc, char** argv)
{
    PollFileSwitches();
    GuestArgs args{ argc, argv, 0 };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, size_t(256) << 20);
    pthread_t guest;
    if (pthread_create(&guest, &attr, GuestThread, &args) != 0) return pes17::Run(argc, argv);
    vk::RunHostedPresenter(args.done);
    pthread_join(guest, nullptr);
    return args.code;
}
#else
int main(int argc, char** argv)
{
    PollFileSwitches();
    return pes17::Run(argc, argv);
}
#endif
#endif
