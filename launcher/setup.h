#pragma once
// Installing the game: the files from the player's disc into the game folder,
// and title update 6 applied to the two executables, which is the version
// the game executables are built from. (A build of the disc version,
// PES17_VERSION_TU0, installs the disc as it is.)
//
// The game folder is game/ beside the launcher, unless the player chose
// another folder with INSTALL LOCATION: then it is game/ under that folder,
// named by install_location.txt beside the launcher (install::InstallLocation
// in runtime/install/install.h, which the game reads too). Saves stay beside
// the launcher either way.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace setup
{
    enum class State
    {
        NotInstalled,
        NeedsUpdate,    // the disc's executables are in game/: the update is all that is missing
        Installed,
    };
    // What game/ holds, by the executables in it.
    State Detect();

    struct Progress
    {
        std::mutex lock;
        int step = 0, steps = 0;        // 1-based, of how many
        std::string title;              // what the step does
        std::string detail;             // the file being copied, when there is one
        uint64_t done = 0, total = 0;   // bytes, when the step has an amount
        std::atomic<bool> cancel{ false };
    };

    enum class Result
    {
        Done,
        Cancelled,
        Failed,
        NoUpdate,       // the update could not be downloaded: the player supplies the file
    };

    // Installs from `disc` (an image or an extracted folder), or brings the
    // install already in game/ up to date when `disc` is empty. `update` is the
    // title update's package or a folder with its files; empty downloads it.
    // `error` is set for Failed and NoUpdate.
    Result Run(const std::filesystem::path& disc, const std::filesystem::path& update, Progress& progress,
               std::string& error);

    // Where the update is downloaded from, to tell a player who has to fetch it.
    const char* UpdateUrl();
    // True in a build that installs an update at all.
    bool UsesUpdate();
    // The game folder: game/ beside the launcher, or game/ under the chosen
    // install location.
    std::filesystem::path GameFolder();
    // The install location itself, or empty for beside the launcher.
    std::filesystem::path InstallLocation();
    // Names the folder the game files go in. Must be absolute; it is created
    // if missing and must be writable, with room checked at install time.
    // Applies to installs from now on: files already installed stay where
    // they are. Empty clears the choice (back to beside the launcher).
    // `error` is set on failure.
    bool SetInstallLocation(const std::filesystem::path& folder, std::string& error);
    // That folder and its free space, as a sentence for the menu.
    std::string InstallLocationText();
}
