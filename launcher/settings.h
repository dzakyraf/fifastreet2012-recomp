#pragma once
// What the player sets in the launcher for the game: kept in
// .env beside the launcher, a line each of NAME=value. The game reads the file as
// it starts (runtime/settings.cpp), however it is started, so a change is
// for the next start. The file can hold any of the game's switches; the
// launcher changes its own and keeps the rest.
#include <string>

namespace settings
{
    void Load();

    // PES17_SCALE: how many times the console's 1280x720 the game draws at.
    constexpr int kMaxScale = 3;
    int Scale();
    void SetScale(int scale);

    // PES17_FPS_LIMIT: how many frames a second the game draws at most; 60 is
    // the console's own pacing and 0 no limit. HasFpsLimit is false until one
    // is kept, and FpsLimit is 60 then, as the game takes it.
    constexpr int kConsoleFps = 60;
    bool HasFpsLimit();
    int FpsLimit();
    void SetFpsLimit(int limit);

    // PES17_FOV: the game's field of view, as its own setting counts it -- the
    // width of a 4:3 picture, 65 on the console.
    constexpr int kConsoleFov = 65, kWidestFov = 120, kFovStep = 5;
    int Fov();
    void SetFov(int fov);

    // PES17_LAN_ACCEPT: 1 joins LAN lobby invitations without asking.
    bool LanAccept();
    void SetLanAccept(bool accept);

    // PES17_NAME: the name other network players see (15 characters). Empty
    // means the profile's name.
    std::string LanName();
    void SetLanName(const std::string& name);

    // PES17_LAN_JOIN: joins that player's lobby as soon as it is seen.
    // Empty means off; the server browser and invitations still work.
    std::string LanJoin();
    void SetLanJoin(const std::string& name);

    // PES17_LAN_IFACE: announce that adapter (name or address substring,
    // e.g. "WiFi" or "192.168.1") instead of the first connection up.
    // Empty means automatic. For machines on a VPN and a LAN at once.
    std::string LanIface();
    void SetLanIface(const std::string& value);

    // PES17_LAUNCHER_SOUNDS: 0 keeps the launcher's menus silent. Written by hand.
    bool Sounds();
}
