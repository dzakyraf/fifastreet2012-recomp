// The policy that moves the scene's render scale while a match runs
// (runtime/gpu/vulkan/dynamic_scale.h). It is pure arithmetic over frame times
// and a thermal status, so every rule is checked here without a phone: a frame
// over budget lowers the scale and one with room to spare raises it, but only
// slowly; heat caps it before the GPU is throttled; a change is followed by a
// pause so the new images' first frames are not read as a verdict; and the scale
// never leaves the player's ceiling or the floor.
#include "gpu/vulkan/dynamic_scale.h"

#include <cstdio>
#include <string>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void Check(bool ok, const std::string& what)
    {
        g_checks++;
        if (!ok)
        {
            g_failures++;
            std::printf("FAIL: %s\n", what.c_str());
        }
    }

    using vk::dynscale::Controller;

    // `windows` windows of `ms` a frame; returns how many changes they made.
    int Run(Controller& c, double ms, int windows)
    {
        int changes = 0;
        for (int i = 0; i < windows * Controller::kWindow; i++)
            if (c.Note(ms)) changes++;
        return changes;
    }

    void Overrun()
    {
        Controller c;
        c.Configure(12, 8, 13.0);
        Check(c.Current() == 12, "starts at the player's scale");
        // 16 ms at 12/16 over a 13 ms budget: the root of 13*0.92/16 is 0.86, so 12 -> 10.
        Run(c, 16.0, 1);
        Check(c.Current() < 12, "over budget lowers the scale");
        Check(c.Current() >= 9, "one decision is at most three sixteenths");
        const int after = c.Current();
        // The next window is ignored (the images are new), the one after is read.
        Run(c, 18.0, Controller::kCooldownWindows);
        Check(c.Current() == after, "a change is followed by a pause");
        Run(c, 30.0, 4);
        Check(c.Current() == 8, "a frame far over the budget walks down to the floor");
        Run(c, 30.0, 4);
        Check(c.Current() == 8, "and stays there: there is nothing below the floor");
    }

    void Recovery()
    {
        Controller c;
        c.Configure(12, 8, 13.0);
        Run(c, 17.0, 20);   // over the budget, but not a sudden jump: a step at a time
        Check(c.Current() == 8, "setup: at the floor");
        // 4 ms is far under budget even three steps up (4 * (9/8)^2 = 5).
        Run(c, 4.0, Controller::kUpWindows - 1);
        Check(c.Current() == 8, "it waits for several calm windows before a step up");
        Run(c, 4.0, 1);
        Check(c.Current() == 9, "then goes up one");
        Run(c, 4.0, 60);
        Check(c.Current() == 12, "and no higher than the player's scale");
    }

    // A scene that costs twice a match (the intro, a cutscene) moves the scale within a few
    // frames and, once it is over, moves it back in one decision.
    void Sudden()
    {
        Controller c;
        c.Configure(12, 8, 13.0);
        for (int i = 0; i < Controller::kFastFrames - 1; i++) Check(!c.Note(28.0), "the sudden rule waits for its frames");
        Check(c.Note(28.0), "and moves the scale at the end of them");
        Check(c.Current() == 8, "to the floor when nothing less fits");
        // Back: one calm window, straight to the ceiling.
        Run(c, 4.0, 1);
        Check(c.Current() == 12, "a calm window after a sudden drop returns to the ceiling in one step");

        Controller w;
        w.Configure(16, 8, 13.0);
        for (int i = 0; i < Controller::kFastFrames; i++) w.Note(30.0);
        Check(w.Current() == 10, "the step that fits, not always the floor");
        // Held for a moment so the first frames of the new images are not read as a verdict.
        for (int i = 0; i < Controller::kFastHold - Controller::kFastFrames; i++) w.Note(30.0);
        Check(w.Current() == 10, "the sudden rule does not act again at once");

        // Not sudden: 1.4 times the budget is the ordinary rule's to handle, over a window.
        Controller n;
        n.Configure(12, 8, 13.0);
        for (int i = 0; i < Controller::kWindow - 1; i++) n.Note(18.0);
        Check(n.Current() == 12, "an overrun short of half again waits for its window");

        // Without the load rule nothing here applies.
        Controller h;
        h.Configure(12, 8, 13.0);
        h.SetRules(true, false);
        for (int i = 0; i < 40; i++) h.Note(40.0);
        Check(h.Current() == 12, "the heat rule alone ignores frame times");
    }

    void NoFlapping()
    {
        Controller c;
        c.Configure(12, 8, 13.0);
        // 11.5 ms is under the budget, but not by enough that a step up would stay under it.
        const int changes = Run(c, 11.5, 40);
        Check(changes == 0, "a frame near the budget changes nothing");
        Check(c.Current() == 12, "and the scale stays");
    }

    void Heat()
    {
        Controller c;
        c.Configure(12, 8, 13.0);
        c.SetThermal(1);
        Check(c.ThermalCeiling() == 12, "light heat is no limit");
        c.SetThermal(2);
        Check(c.ThermalCeiling() == 11, "moderate heat takes a quarter of the room");
        c.SetThermal(3);
        Check(c.ThermalCeiling() == 10, "severe heat takes half of it");
        c.SetThermal(4);
        Check(c.ThermalCeiling() == 8, "critical heat takes all of it");
        c.SetThermal(3);
        // Heat acts on the next frame, not after a window.
        Check(c.Note(5.0), "a status above the scale moves it at once, with time to spare");
        Check(c.Current() == 10, "to the heat's ceiling");
        // The heat's ceiling is not a target: a GPU over budget goes lower still.
        Run(c, 30.0, 10);
        Check(c.Current() == 8, "a GPU with no room goes below the ceiling");
        c.SetThermal(0);
        Run(c, 4.0, 20);
        Check(c.Current() == 12, "and it comes back when the phone has cooled and the GPU has room");

        // A scale that has no room above the floor has nothing to take.
        Controller f;
        f.Configure(8, 8, 13.0);
        f.SetThermal(4);
        Check(f.ThermalCeiling() == 8, "a ceiling at the floor stays");
        // A full 1280x720 scale: eight sixteenths of room.
        Controller w;
        w.Configure(16, 8, 13.0);
        w.SetThermal(2);
        Check(w.ThermalCeiling() == 14, "a quarter of eight");
        w.SetThermal(3);
        Check(w.ThermalCeiling() == 12, "half of eight");
    }

    void Rules()
    {
        // Heat alone: the GPU time says nothing, the status moves the picture and brings it back.
        Controller h;
        h.Configure(12, 8, 13.0);
        h.SetRules(true, false);
        Run(h, 50.0, 5);
        Check(h.Current() == 12, "without the load rule a GPU over budget changes nothing");
        h.SetThermal(3);
        Check(h.Frame() && h.Current() == 10, "heat alone still lowers the picture at once");
        h.SetThermal(0);
        int frames = 0;
        while (h.Current() < 12 && frames < 5000) { h.Frame(); frames++; }
        Check(h.Current() == 12, "and brings it back, a step at a time, when the phone has cooled");
        Check(frames >= 2 * Controller::kWindow, "not faster than a step every couple of seconds");

        // Load alone: the status is ignored.
        Controller l;
        l.Configure(12, 8, 13.0);
        l.SetRules(false, true);
        l.SetThermal(4);
        Check(l.ThermalCeiling() == 12, "without the heat rule the status is no limit");
        Check(!l.Frame() && l.Current() == 12, "and does not move the picture");
        Run(l, 30.0, 8);
        Check(l.Current() == 8, "the load rule still lowers it");

        // Neither: nothing moves.
        Controller n;
        n.Configure(12, 8, 13.0);
        n.SetRules(false, false);
        n.SetThermal(6);
        Run(n, 50.0, 5);
        Check(!n.Frame() && n.Current() == 12, "with both off the scale stays");
    }

    void Bounds()
    {
        Controller c;
        c.Configure(20, 2, 13.0);
        Check(c.Ceiling() == 16, "the ceiling is at most 1280x720");
        Check(c.Floor() == 4, "the floor is at least 4 sixteenths");
        c.Configure(10, 12, 13.0);
        Check(c.Ceiling() == 12 && c.Floor() == 12, "a ceiling under the floor is the floor");
        Run(c, 50.0, 4);
        Check(c.Current() == 12, "a scale pinned by its bounds does not move");
        c.Configure(16, 8, 0.0);
        Check(c.Budget() >= 1.0, "a nonsense budget is kept sane");
    }
}

int main()
{
    Overrun();
    Recovery();
    Sudden();
    NoFlapping();
    Heat();
    Rules();
    Bounds();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
