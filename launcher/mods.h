#pragma once
// Mods: extra game files (fastfiles, image archives, movies) copied into the
// game folder. The engine reads that one flat directory, so a same-version
// .ff just loads -- maps, weapons and modes from packs like the CoD Xe
// fastfiles work this way.
//
// Only engine data is copied. Scripts, plugin files and executables are
// reported as skipped: loose .gsc and the CoD Xe plugin need a script
// runtime this port does not have, and executables are never taken from a
// mod. The install behind this screen is untouched otherwise.
#include <filesystem>
#include <string>
#include <vector>

namespace mods
{
    struct Skipped
    {
        std::string name, reason;
    };
    struct Result
    {
        std::vector<std::string> copied;
        std::vector<Skipped> skipped;
    };

    // Copies what the game loads out of `folder` (not recursive) into the
    // game folder, leaving files already there, whole, alone.
    Result Install(const std::filesystem::path& folder, const std::filesystem::path& game);

    // The outcome as sentences for the menu.
    std::string Report(const Result& result);
}
