#include "mods.h"
#include "disc.h"
#include "../runtime/install/install.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
    bool IsGameFile(const std::string& name)
    {
        std::string lower = name;
        for (char& c : lower) c = char(std::tolower(uint8_t(c)));
        auto endsWith = [&](const char* suffix) {
            const size_t n = std::strlen(suffix);
            return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
        };
        return endsWith(".ff") || endsWith(".pak") || endsWith(".bik");
    }

    bool IsExecutable(const std::string& name)
    {
        std::string lower = name;
        for (char& c : lower) c = char(std::tolower(uint8_t(c)));
        auto endsWith = [&](const char* suffix) {
            const size_t n = std::strlen(suffix);
            return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
        };
        return endsWith(".xex") || endsWith(".exe") || endsWith(".dll") || endsWith(".so");
    }
}

mods::Result mods::Install(const fs::path& folder, const fs::path& game)
{
    Result result;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return result;
    fs::create_directories(game, ec);
    std::vector<fs::directory_entry> entries;
    for (const auto& entry : fs::directory_iterator(folder, ec)) entries.push_back(entry);
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename() < b.path().filename();
    });
    char buffer[1 << 20];
    for (const auto& entry : entries)
    {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = install::Utf8(entry.path().filename());
        const fs::path to = game / entry.path().filename();
        if (!IsGameFile(name))
        {
            result.skipped.push_back({name, IsExecutable(name) ? "executables are never taken from a mod"
                                                              : "needs the CoD Xe plugin, which this port does not run"});
            continue;
        }
        if (fs::is_regular_file(to, ec) && fs::file_size(to, ec) == fs::file_size(entry.path(), ec)) continue;
        const fs::path partial = fs::path(to) += ".part";
        std::ifstream in(entry.path(), std::ios::binary);
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        bool ok = bool(in) && bool(out);
        while (ok && in)
        {
            in.read(buffer, sizeof buffer);
            out.write(buffer, in.gcount());
            ok = bool(out);
        }
        out.close();
        if (ok) fs::rename(partial, to, ec);
        if (!ok || ec)
        {
            fs::remove(partial, ec);
            result.skipped.push_back({name, "could not be written (disk full?)"});
            continue;
        }
        result.copied.push_back(name);
    }
    return result;
}

std::string mods::Report(const Result& result)
{
    std::string text;
    if (result.copied.empty() && result.skipped.empty())
        return "That folder holds nothing to install: no .ff, .pak or .bik files.";
    if (!result.copied.empty())
    {
        char head[64];
        std::snprintf(head, sizeof head, "Installed %zu file(s) into the game folder:\n", result.copied.size());
        text += head;
        for (const std::string& name : result.copied) text += name + "\n";
    }
    if (!result.skipped.empty())
    {
        if (!text.empty()) text += "\n";
        text += "Skipped:\n";
        for (const Skipped& skipped : result.skipped)
            text += skipped.name + " (" + skipped.reason + ")\n";
    }
    return text;
}
