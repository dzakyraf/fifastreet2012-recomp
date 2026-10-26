#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace
{
    constexpr const char* kFile = ".env";
    // Every line, so the ones a player wrote by hand are written back.
    std::map<std::string, std::string> g_kept;

    void Save()
    {
        std::ofstream file(kFile, std::ios::trunc);
        for (const auto& [name, value] : g_kept) file << name << '=' << value << '\n';
    }
}

void settings::Load()
{
    std::ifstream file(kFile);
    for (std::string line; std::getline(file, line);)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string name = line.substr(0, equals), value = line.substr(equals + 1);
        g_kept[name] = value;
    }
}

int settings::Scale()
{
    const auto kept = g_kept.find("PES17_SCALE");
    return std::clamp(kept == g_kept.end() ? 1 : std::atoi(kept->second.c_str()), 1, kMaxScale);
}

void settings::SetScale(int scale)
{
    g_kept["PES17_SCALE"] = std::to_string(std::clamp(scale, 1, kMaxScale));
    Save();
}

bool settings::HasFpsLimit() { return g_kept.count("PES17_FPS_LIMIT") != 0; }

int settings::FpsLimit()
{
    const auto kept = g_kept.find("PES17_FPS_LIMIT");
    return kept == g_kept.end() ? kConsoleFps : std::clamp(std::atoi(kept->second.c_str()), 0, 1000);
}

void settings::SetFpsLimit(int limit)
{
    g_kept["PES17_FPS_LIMIT"] = std::to_string(limit);
    Save();
}

int settings::Fov()
{
    const auto kept = g_kept.find("PES17_FOV");
    return kept == g_kept.end() ? kConsoleFov : std::clamp(std::atoi(kept->second.c_str()), kConsoleFov, kWidestFov);
}

void settings::SetFov(int fov)
{
    g_kept["PES17_FOV"] = std::to_string(std::clamp(fov, kConsoleFov, kWidestFov));
    Save();
}

bool settings::LanAccept()
{
    const auto kept = g_kept.find("PES17_LAN_ACCEPT");
    return kept != g_kept.end() && kept->second == "1";
}

void settings::SetLanAccept(bool accept)
{
    if (accept) g_kept["PES17_LAN_ACCEPT"] = "1";
    else g_kept.erase("PES17_LAN_ACCEPT");
    Save();
}

std::string settings::LanName()
{
    const auto kept = g_kept.find("PES17_NAME");
    return kept == g_kept.end() ? std::string() : kept->second.substr(0, 15);
}

void settings::SetLanName(const std::string& name)
{
    if (name.empty()) g_kept.erase("PES17_NAME");
    else g_kept["PES17_NAME"] = name.substr(0, 15);
    Save();
}

std::string settings::LanJoin()
{
    const auto kept = g_kept.find("PES17_LAN_JOIN");
    return kept == g_kept.end() ? std::string() : kept->second;
}

void settings::SetLanJoin(const std::string& name)
{
    if (name.empty()) g_kept.erase("PES17_LAN_JOIN");
    else g_kept["PES17_LAN_JOIN"] = name;
    Save();
}

std::string settings::LanIface()
{
    const auto kept = g_kept.find("PES17_LAN_IFACE");
    return kept == g_kept.end() ? std::string() : kept->second;
}

void settings::SetLanIface(const std::string& value)
{
    if (value.empty()) g_kept.erase("PES17_LAN_IFACE");
    else g_kept["PES17_LAN_IFACE"] = value;
    Save();
}

bool settings::Sounds()
{
    const auto kept = g_kept.find("PES17_LAUNCHER_SOUNDS");
    return kept == g_kept.end() || kept->second != "0";
}
