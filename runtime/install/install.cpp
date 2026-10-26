#include "install.h"
#include "crypto.h"
#include "xex.h"
#include "spawn.h"
#include "../log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef PES17_USE_SDL
#include <SDL3/SDL.h>
#endif
#ifdef PES17_ANDROID
#include "../android/android.h"
#include "../../launcher/disc.h"
#include "../../launcher/package.h"
#ifdef PES17_HAVE_XEX_PATCHER
#include <xex_patcher.h>
#endif
// A TU6 build configured without the update's hashes (no pes17/update/ and no
// -DPES17_UPDATE_SHA256_*): ApplyTu6 refuses at run time rather than trusting
// any bytes, and this keeps it compiling.
#ifndef PES17_UPDATE_SHA256_PATCH_MP
#define PES17_UPDATE_SHA256_PATCH_MP ""
#endif
#ifndef PES17_UPDATE_SHA256_DLC1_UI_MP
#define PES17_UPDATE_SHA256_DLC1_UI_MP ""
#endif
#ifndef PES17_UPDATE_SHA256_DLC2_UI_MP
#define PES17_UPDATE_SHA256_DLC2_UI_MP ""
#endif
#endif

namespace fs = std::filesystem;

// The executable this build was recompiled from, by its SHA-256, which CMake
// reads from pes17/ at configure time. Any other -- another region, another
// version -- has other code at other addresses, so it is not run.
namespace
{
#ifdef PES17_TITLE_MP
    constexpr const char* kXex = "default_mp.xex";
    constexpr const char* kSha256 = PES17_XEX_SHA256_MP;
#else
    constexpr const char* kXex = "default.xex";
    constexpr const char* kSha256 = PES17_XEX_SHA256_SP;
#endif
    constexpr const char* kGameFolder = "game";
    std::filesystem::path g_gameFolder = "game";
#ifdef _WIN32
    constexpr const char* kLauncher = "pes17-launcher.exe";
#else
    constexpr const char* kLauncher = "pes17-launcher";
#endif

    // Paths as UTF-8, which SDL and the log take, whatever the system's code page.
    std::string Utf8(const fs::path& path)
    {
        const auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    fs::path ExecutableFolder()
    {
#ifdef _WIN32
        wchar_t path[MAX_PATH * 4];
        const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
        return fs::path(std::wstring(path, length)).parent_path();
#else
        std::error_code ec;
        return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
    }

    void ShowError(const std::string& text)
    {
        LOGE("%s", text.c_str());
        std::fprintf(stderr, "%s\n", text.c_str());
#ifdef PES17_USE_SDL
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Modern Warfare 2", text.c_str(), nullptr);
#endif
#ifdef PES17_ANDROID
        android::SetStatus(text.c_str());
#endif
    }

    // The launcher beside this executable, left running on its own.
    bool StartLauncher()
    {
#ifdef PES17_USE_SDL
        std::error_code ec;
        const fs::path launcher = fs::absolute(kLauncher, ec);
        if (!fs::is_regular_file(launcher, ec)) return false;
        const std::string path = Utf8(launcher);
        const char* const args[] = { path.c_str(), nullptr };
        SDL_Process* process = spawn::Start(args);
        if (!process) { LOGE("cannot start %s: %s", path.c_str(), SDL_GetError()); return false; }
        SDL_DestroyProcess(process);    // forgets it; the launcher keeps running
        return true;
#else
        return false;
#endif
    }

    // The program of each of the title's executables, beside this one.
    const char* ProgramFor(const std::string& xex)
    {
#ifdef _WIN32
        if (xex == "default.xex") return "pes17-sp.exe";
        if (xex == "default_mp.xex") return "pes17-mp.exe";
#else
        if (xex == "default.xex") return "pes17-sp";
        if (xex == "default_mp.xex") return "pes17-mp";
#endif
        return nullptr;
    }

    constexpr const char* kLaunchDataVariable = "PES17_LAUNCH_DATA";
    bool g_playerStart = false;
    std::string g_nextTitle;

    enum class Installed { Yes, No, Unreadable };

    // The title's image from game/, if the executable there is the one this
    // build was made from.
    Installed LoadInstalled(std::vector<uint8_t>& image, std::string& error)
    {
        const fs::path path = fs::path(g_gameFolder) / kXex;
        std::vector<uint8_t> file;
        {
            std::ifstream in(path, std::ios::binary);
            file.assign(std::istreambuf_iterator<char>(in), {});
        }
        if (file.empty()) return Installed::No;
        install::crypto::Sha256 hash;
        hash.Update(file.data(), file.size());
        if (hash.FinalHex() != kSha256) return Installed::No;
        if (!install::xex::Image(file, image, error))
        {
            error = Utf8(path) + ": " + error;
            return Installed::Unreadable;
        }
        return Installed::Yes;
    }

#ifdef PES17_ANDROID
    constexpr uint32_t kTitleId = 0x4541097B;   // FIFA Street, from the XEX execution info

    struct Title { const char* xex; const char* sha256; const char* disc; };
    constexpr Title kTitles[] = {
        { "default.xex", PES17_XEX_SHA256_SP, PES17_DISC_SHA256_SP },
    };

    std::string Sha256(const std::vector<uint8_t>& bytes)
    {
        install::crypto::Sha256 hash;
        hash.Update(bytes.data(), bytes.size());
        return hash.FinalHex();
    }

    std::string WrongDisc(const std::string& name, const std::vector<uint8_t>& file)
    {
        install::xex::Info info;
        if (!install::xex::ReadInfo(file, info))
            return name + " is not an Xbox 360 executable.";
        char text[256];
        if (info.titleId != kTitleId)
            std::snprintf(text, sizeof(text), "This is not PES 2017 (title %08X).", info.titleId);
        else
            std::snprintf(text, sizeof(text),
                          "This is PES 2017, but not the version this build was recompiled from "
                          "(Europe, executable version %u). Install from the European disc image "
                          "or its extracted folder.", info.version);
        return text;
    }

    bool IsGameFile(const std::string& name)
    {
        std::string lower = name;
        for (char& c : lower) c = char(std::tolower(uint8_t(c)));
        auto endsWith = [&](const char* suffix) {
            const size_t n = std::strlen(suffix);
            return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
        };
        // FIFA Street: the EA BIG archives and their headers, and the game DLL.
        // The data/ and dlc/ folders are not copied by this flat installer yet.
        return endsWith(".big") || endsWith(".bh") || lower == "nxeart" || lower == "fifadllzf.xex.dll";
    }

    struct Progress
    {
        std::mutex lock;
        std::string file;
        uint64_t done = 0, total = 0;
        std::atomic<bool> cancel{ false };
    };

    bool Install(const fs::path& from, Progress& progress, std::string& error)
    {
        auto source = install::Source::Open(from, error);
        if (!source) return false;

        std::vector<const install::Source::File*> files;
        for (const Title& title : kTitles)
        {
            const auto* file = source->Find(title.xex);
            if (!file)
            {
                if (std::string(title.xex) == kXex)
                {
                    error = std::string("There is no ") + title.xex + " in " + Utf8(from) +
                            ": it is not a FIFA Street disc or folder (no default.xex).";
                    return false;
                }
                continue;
            }
            std::vector<uint8_t> bytes;
            if (!source->ReadAll(*file, bytes))
            {
                error = std::string("Could not read ") + title.xex + " from " + Utf8(from);
                return false;
            }
            // A TU6 build installs from the disc's executables (the update is
            // applied afterwards by ApplyTu6); a TU0 build installs what it
            // was recompiled from.
#ifdef PES17_VERSION_TU0
            if (Sha256(bytes) != title.sha256) { error = WrongDisc(title.xex, bytes); return false; }
#else
            if (Sha256(bytes) != title.disc) { error = WrongDisc(title.xex, bytes); return false; }
#endif
        }
        for (const auto& file : source->Files())
            if (IsGameFile(file.name)) files.push_back(&file);
        for (const Title& title : kTitles)
            if (const auto* file = source->Find(title.xex)) files.push_back(file);

        const fs::path folder = g_gameFolder;
        std::error_code ec;
        fs::create_directories(folder, ec);
        if (ec) { error = "Cannot create " + Utf8(fs::absolute(folder)) + ": " + ec.message(); return false; }

        uint64_t needed = 0;
        std::vector<const install::Source::File*> copy;
        const fs::path canonicalFolder = folder.lexically_normal();
        for (const auto* file : files)
        {
            if (file->name.empty() || file->name == "." || file->name == ".." ||
                file->name.find('/') != std::string::npos || file->name.find('\\') != std::string::npos)
                continue;
            const fs::path to = (folder / install::FromUtf8(file->name)).lexically_normal();
            if (to.parent_path() != canonicalFolder) continue;

            if (fs::is_regular_file(to, ec) && fs::file_size(to, ec) == file->size)
            {
                if (file->size == 0) continue;
                bool match = true;
                std::ifstream in(to, std::ios::binary);
                std::vector<char> bufDisk(64 << 10), bufSrc(64 << 10);
                for (uint64_t at = 0; at < file->size; at += bufDisk.size())
                {
                    const size_t checkLen = size_t(std::min<uint64_t>(bufDisk.size(), file->size - at));
                    if (!in.read(bufDisk.data(), checkLen) || size_t(in.gcount()) != checkLen ||
                        !source->Read(*file, at, bufSrc.data(), checkLen) ||
                        std::memcmp(bufDisk.data(), bufSrc.data(), checkLen) != 0)
                    {
                        match = false;
                        break;
                    }
                }
                if (match) continue;
            }
            copy.push_back(file);
            needed += file->size;
        }
        {
            std::lock_guard lock(progress.lock);
            progress.total = needed;
        }
        const auto space = fs::space(folder, ec);
        if (!ec && space.available < needed)
        {
            char text[160];
            std::snprintf(text, sizeof(text), "Not enough disk space: %.1f GB needed, %.1f GB free.",
                          needed / 1e9, space.available / 1e9);
            error = text;
            return false;
        }

        std::vector<uint8_t> buffer(8 << 20);
        for (const auto* file : copy)
        {
            {
                std::lock_guard lock(progress.lock);
                progress.file = file->name;
            }
            const fs::path to = folder / install::FromUtf8(file->name), partial = fs::path(to) += ".part";

            struct PartCleaner
            {
                fs::path target;
                bool disarmed = false;
                ~PartCleaner()
                {
                    if (!disarmed && !target.empty())
                    {
                        std::error_code ec;
                        fs::remove(target, ec);
                    }
                }
            } cleaner{ partial };

            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            if (!out) { error = "Cannot write " + Utf8(fs::absolute(partial)); return false; }
            for (uint64_t at = 0; at < file->size;)
            {
                if (progress.cancel) { out.close(); error = "cancelled"; return false; }
                const size_t take = size_t(std::min<uint64_t>(buffer.size(), file->size - at));
                if (!source->Read(*file, at, buffer.data(), take))
                {
                    out.close();
                    error = "Could not read " + file->name + " from " + Utf8(from) + ".";
                    return false;
                }
                out.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(take));
                if (!out)
                {
                    out.close();
                    error = "Could not write " + Utf8(fs::absolute(partial)) + " (disk full?).";
                    return false;
                }
                at += take;
                std::lock_guard lock(progress.lock);
                progress.done += take;
            }
            out.close();
            fs::rename(partial, to, ec);
            if (ec) { error = "Cannot rename " + Utf8(partial) + ": " + ec.message(); return false; }
            cleaner.disarmed = true;
        }
        return true;
    }

    std::mutex g_installLock;
    Progress* g_installing = nullptr;
#endif
}

#ifdef PES17_ANDROID
namespace install
{
    namespace
    {
        std::string TuSha256(const std::vector<uint8_t>& bytes)
        {
            install::crypto::Sha256 hash;
            hash.Update(bytes.data(), bytes.size());
            return hash.FinalHex();
        }

        // Through a temporary name, so a file present is a file written whole.
        bool TuWriteWhole(const fs::path& to, const std::vector<uint8_t>& bytes, std::string& error)
        {
            const fs::path partial = fs::path(to) += ".part";
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            out.close();
            std::error_code ec;
            if (out) fs::rename(partial, to, ec);
            if (!out || ec) { error = "Cannot write " + install::Utf8(fs::absolute(to)) + " (disk full?)."; return false; }
            return true;
        }
    }

    // Applies downloaded title update 6 to an installed disc (TU6 builds):
    // patches both executables in memory exactly as the PC launcher's install
    // does (launcher/setup.cpp), then lays the update's fastfiles beside the
    // disc's. Executables go last: both present and patched is what an update
    // that finished looks like.
    bool ApplyTu6(const fs::path& package, std::string& error)
    {
#ifdef PES17_VERSION_TU0
        (void)package;
        error = "This build is the disc version; it needs no update.";
        return false;
#else
#ifndef PES17_HAVE_XEX_PATCHER
        (void)package;
        error = "This APK was built without the title-update patcher; install a CI-built TU6 APK.";
        return false;
#else
        static const char* kUpdateHashes[][2] = {
            { "patch_mp.ff", PES17_UPDATE_SHA256_PATCH_MP },
            { "dlc1_ui_mp.ff", PES17_UPDATE_SHA256_DLC1_UI_MP },
            { "dlc2_ui_mp.ff", PES17_UPDATE_SHA256_DLC2_UI_MP },
        };
        for (const auto& [name, hash] : kUpdateHashes)
            if (!*hash)
            {
                error = "This build does not know title update 6's files; install a CI-built TU6 APK.";
                return false;
            }
        std::vector<uint8_t> packed;
        {
            std::ifstream in(package, std::ios::binary);
            if (!in) { error = "Cannot read the downloaded update."; return false; }
            packed.assign(std::istreambuf_iterator<char>(in), {});
        }
        std::map<std::string, std::vector<uint8_t>> files;
        std::string why;
        if (!install::ReadPackage(packed, files, why)) { error = "The downloaded update: " + why + "."; return false; }
        for (const auto& [name, hash] : kUpdateHashes)
        {
            const auto it = files.find(name);
            if (it == files.end() || TuSha256(it->second) != hash)
            {
                error = std::string("This is not title update 6: its ") + name + " is missing or another version's.";
                return false;
            }
        }
        const char* kPatches[][3] = {
            { "default.xex", "default.xexp", PES17_XEX_SHA256_SP },
            { "default_mp.xex", "default_mp.xexp", PES17_XEX_SHA256_MP },
        };
        const fs::path folder = install::GameFolder();
        std::vector<uint8_t> patched[2];
        for (int i = 0; i < 2; i++)
        {
            const auto px = files.find(kPatches[i][1]);
            if (px == files.end() || px->second.empty())
            {
                error = std::string("This is not title update 6: its ") + kPatches[i][1] + " is missing.";
                return false;
            }
            std::vector<uint8_t> base;
            {
                std::ifstream in(folder / kPatches[i][0], std::ios::binary);
                if (!in) { error = "Install the disc first: its executables are not here yet."; return false; }
                base.assign(std::istreambuf_iterator<char>(in), {});
            }
            const auto result = XexPatcher::apply(base.data(), base.size(), px->second.data(),
                                                  px->second.size(), patched[i], false);
            if (result != XexPatcher::Result::Success || TuSha256(patched[i]) != kPatches[i][2])
            {
                error = std::string("This is not title update 6: its ") + kPatches[i][1] +
                        " does not make the expected executable.";
                return false;
            }
        }
        for (const auto& [name, hash] : kUpdateHashes)
            if (!TuWriteWhole(folder / name, files[name], error)) return false;
        for (int i = 0; i < 2; i++)
            if (!TuWriteWhole(folder / kPatches[i][0], patched[i], error)) return false;
        LOGI("install: title update 6 applied");
        return true;
#endif
#endif
    }
}
#endif

bool install::PlayerStart(int argc, char**)
{
    return argc == 1;
}

fs::path install::Folder(int argc, char** argv)
{
    return PlayerStart(argc, argv) ? ExecutableFolder() : fs::path();
}

bool install::Prepare(int, char**, Launch& launch, int& exitCode)
{
    exitCode = 1;
    g_playerStart = true;
    // Everything a player's copy keeps -- game/, saves/ -- is beside the executable.
    std::error_code ec;
    if (const fs::path folder = ExecutableFolder(); !folder.empty()) fs::current_path(folder, ec);
    // ...unless the launcher put the game files on another drive (INSTALL
    // LOCATION): same file it wrote, beside this program too.
    if (const fs::path at = InstallLocation(ExecutableFolder()); !at.empty())
        SetGameFolder(at / kGameFolder);

    std::string error;
    switch (LoadInstalled(launch.image, error))
    {
    case Installed::Yes:
        launch.gameRoot = g_gameFolder;
        return true;
    case Installed::Unreadable:
        ShowError(error);
        return false;
    case Installed::No:
        break;
    }
    // Not installed, or installed for another version of this build: both are
    // the launcher's to put right.
    if (StartLauncher()) { exitCode = 0; return false; }
    ShowError(std::string("The game is not installed here, and ") + kLauncher +
              " is not beside this program to install it.");
    return false;
}

bool install::LoadImageFile(const fs::path& path, std::vector<uint8_t>& image)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { LOGE("cannot open image %s", Utf8(path).c_str()); return false; }
    std::vector<uint8_t> file(std::istreambuf_iterator<char>(in), {});
    if (file.size() >= 4 && std::memcmp(file.data(), "XEX2", 4) == 0)
    {
        std::string error;
        if (!xex::Image(file, image, error)) { LOGE("%s: %s", Utf8(path).c_str(), error.c_str()); return false; }
        return true;
    }
    image = std::move(file);
    return true;
}

void install::SetGameFolder(const fs::path& folder)
{
    g_gameFolder = folder;
    LOGI("install: the game folder is %s", Utf8(fs::absolute(folder)).c_str());
}

const fs::path& install::GameFolder() { return g_gameFolder; }

bool install::Installed()
{
    std::error_code ec;
    return fs::is_regular_file(g_gameFolder / kXex, ec);
}

void install::CancelInstall()
{
#ifdef PES17_ANDROID
    std::lock_guard lock(g_installLock);
    if (g_installing) g_installing->cancel = true;
#endif
}

bool install::InstallFrom(const fs::path& from, std::string& error, Report report, void* user)
{
#ifdef PES17_ANDROID
    Progress progress;
    {
        std::lock_guard lock(g_installLock);
        if (g_installing) { error = "An install is already running."; return false; }
        g_installing = &progress;
    }

    std::atomic<bool> finished{ false };
    std::thread reporter;
    if (report)
        reporter = std::thread([&] {
            while (!finished.load(std::memory_order_acquire))
            {
                {
                    std::lock_guard lock(progress.lock);
                    report(progress.file.c_str(), progress.done, progress.total, user);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            std::lock_guard lock(progress.lock);
            report(progress.file.c_str(), progress.done, progress.total, user);
        });

    const bool ok = Install(from, progress, error);
    finished.store(true, std::memory_order_release);
    if (reporter.joinable()) reporter.join();
    {
        std::lock_guard lock(g_installLock);
        g_installing = nullptr;
    }
    if (!ok && error.empty()) error = "The game could not be installed.";
    if (ok) LOGI("install: done, into %s", Utf8(fs::absolute(g_gameFolder)).c_str());
    else    LOGE("install: %s", error.c_str());
    return ok;
#else
    error = "Not implemented on desktop";
    return false;
#endif
}

std::vector<uint8_t>& install::LaunchData()
{
    static std::vector<uint8_t> data = [] {
        std::vector<uint8_t> bytes;
        const char* hex = std::getenv(kLaunchDataVariable);
        for (; hex && hex[0] && hex[1]; hex += 2)
        {
            unsigned value = 0;
            if (std::sscanf(hex, "%2x", &value) != 1) return std::vector<uint8_t>{};
            bytes.push_back(uint8_t(value));
        }
        return bytes;
    }();
    return data;
}

void install::SetNextTitle(const std::string& xex) { g_nextTitle = xex; }

void install::StartNextTitle()
{
    if (g_nextTitle.empty()) return;
    const char* program = ProgramFor(g_nextTitle);
    if (!program) { LOGE("the title asked for %s, which is not one of its executables", g_nextTitle.c_str()); return; }
    // A development run was given its image and game folder by hand, and
    // nothing says where the other title's are.
    if (!g_playerStart)
    {
        LOGW("the title asked for %s: start that build yourself, a development run does not", g_nextTitle.c_str());
        return;
    }
#ifdef PES17_USE_SDL
    std::error_code ec;
    const fs::path file = fs::absolute(program, ec);
    if (!fs::is_regular_file(file, ec)) { ShowError(std::string(program) + " is not beside this program."); return; }
    std::string hex;
    for (const uint8_t byte : LaunchData())
    {
        char text[3];
        std::snprintf(text, sizeof(text), "%02x", byte);
        hex += text;
    }
    SDL_Environment* environment = SDL_GetEnvironment();
    if (hex.empty()) SDL_UnsetEnvironmentVariable(environment, kLaunchDataVariable);
    else SDL_SetEnvironmentVariable(environment, kLaunchDataVariable, hex.c_str(), true);
    // It writes on in this run's log, where there is one (main.cpp).
    SDL_SetEnvironmentVariable(environment, "PES17_LOG_APPEND", "1", true);
    const std::string path = Utf8(file);
    const char* const args[] = { path.c_str(), nullptr };
    SDL_Process* process = spawn::Start(args, environment);
    if (!process) { ShowError("cannot start " + path + ": " + SDL_GetError()); return; }
    SDL_DestroyProcess(process);
    LOGI("started %s", path.c_str());
#else
    LOGE("this build cannot start %s", program);
#endif
}
