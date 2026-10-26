#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Pictures of the window, as PNG files, for a run nobody is watching.
//
// A scripted run (PES17_INPUT_SCRIPT) reaches a screen and the log says what the
// title called, but not what it drew: a dialog's text, a menu's highlight, a
// blank where a crest should be. A picture taken at a time in the script answers
// that without a person at the window. The frame is the swapchain image as it
// is presented, sign-in overlay and letterbox included.
//
// Ways to ask for one, all landing in <files>/shots/ (PES17_FILES, or the app's
// files folder on Android) as shot_<seconds>s_<why>.png:
//   - "45:shot" in PES17_INPUT_SCRIPT, on the same clock as the presses;
//   - PES17_SHOT_EVERY=<seconds>, one every so often;
//   - `touch <files>/screenshot`, polled about twice a second and removed;
//   - F2 in the window.
namespace vk::screenshot
{
    // Ask for the next presented frame; `why` goes in the file name and the log.
    void Request(const char* why);

    // The presenter asks once a loop whether a picture is due, and `why` says
    // which of the above wanted it.
    bool Due(std::string& why);

    // `pixels` is the frame, tightly packed, four bytes a pixel, blue first when
    // `bgra`. Written on a thread of its own so the window does not hitch.
    void Save(std::vector<uint8_t> pixels, uint32_t width, uint32_t height, bool bgra, std::string why);
}
