// XamInputGetKeystrokeEx: the pad as a stream of key events.
//
// PES 2017 does not poll XInputGetState; its pad library calls this from several
// threads and builds its own held/pressed state from the events. Answered "empty"
// (as it was while no chatpad was the only question), the title saw a controller
// that never did anything. The events are made as ReXGlue's input driver makes
// them (itself following the console): one event per call, KEYUP before KEYDOWN,
// sticks and triggers as virtual keys, and KEYDOWN|REPEAT while a key is held.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "../guest.h"
#include "../log.h"

#ifdef PES17_ANDROID
#include "../android/android.h"
#endif

#include <array>
#include <chrono>
#include <mutex>

namespace
{
    constexpr uint32_t X_ERROR_EMPTY = 0x000010D2;

    // XINPUT_KEYSTROKE: u16 virtual key, u16 unicode, u16 flags, u8 user, u8 hid.
    constexpr uint16_t kKeyDown = 0x0001, kKeyUp = 0x0002, kKeyRepeat = 0x0004;

    // Bit i of the key field -> virtual key. 0-15 are XINPUT_GAMEPAD's button bits,
    // 16-17 the triggers, 18-25 the left stick's eight directions, 26-33 the right's.
    constexpr uint16_t kVk[34] = {
        0x5810, 0x5811, 0x5812, 0x5813, 0x5814, 0x5815, 0x5816, 0x5817,   // dpad, start, back, L3, R3
        0x5805, 0x5804, 0,      0,      0x5800, 0x5801, 0x5802, 0x5803,   // LB, RB, -, -, A, B, X, Y
        0x5806, 0x5807,                                                    // triggers
        0x5820, 0x5821, 0x5822, 0x5823, 0x5824, 0x5825, 0x5826, 0x5827,   // left stick
        0x5830, 0x5831, 0x5832, 0x5833, 0x5834, 0x5835, 0x5836, 0x5837,   // right stick
    };

    constexpr int kTriggerThreshold = 30;       // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
    constexpr int kThumbThreshold = 7849;       // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
    constexpr auto kRepeatDelay = std::chrono::milliseconds(400);
    constexpr auto kRepeatRate = std::chrono::milliseconds(100);

    uint64_t Stick(int16_t x, int16_t y, int shift)
    {
        bool u = y > kThumbThreshold, d = y < -kThumbThreshold;
        bool r = x > kThumbThreshold, l = x < -kThumbThreshold;
        uint64_t f = 0;
        if (u && l) { u = l = false; f |= 1ull << (4 + shift); }
        if (u && r) { u = r = false; f |= 1ull << (5 + shift); }
        if (d && r) { d = r = false; f |= 1ull << (6 + shift); }
        if (d && l) { d = l = false; f |= 1ull << (7 + shift); }
        if (u) f |= 1ull << (0 + shift);
        if (d) f |= 1ull << (1 + shift);
        if (r) f |= 1ull << (2 + shift);
        if (l) f |= 1ull << (3 + shift);
        return f;
    }

    struct Keys
    {
        uint64_t last = 0;
        int repeating = -1;                         // bit being repeated
        bool waiting = false;                       // before the first repeat
        std::chrono::steady_clock::time_point since;
    };
    std::mutex g_lock;
    std::array<Keys, 4> g_keys;

    void Fill(uint32_t out, uint16_t vk, uint16_t flags, uint32_t user)
    {
        auto* p = GuestPtr<uint8_t>(out);
        p[0] = uint8_t(vk >> 8); p[1] = uint8_t(vk);       // virtual key
        p[2] = 0; p[3] = 0;                                 // unicode
        p[4] = uint8_t(flags >> 8); p[5] = uint8_t(flags);  // flags
        p[6] = uint8_t(user); p[7] = 0;                     // user, hid code
    }
}

// (UserIndex*, Flags, XINPUT_KEYSTROKE*). The user is 0xFF for "any".
PPC_FUNC(__imp__XamInputGetKeystrokeEx)
{
#ifdef PES17_ANDROID
    const uint32_t userPtr = ctx.r3.u32, out = ctx.r5.u32;
    const uint32_t requested = userPtr ? uint32_t(*GuestPtr<be32>(userPtr)) : 0xFF;
    if (!out) { ctx.r3.u64 = X_ERROR_EMPTY; return; }
    const bool any = requested == 0xFF;
    std::lock_guard held(g_lock);
    const auto now = std::chrono::steady_clock::now();
    for (uint32_t user = any ? 0 : requested; user < (any ? 4u : requested + 1); user++)
    {
        android::input::Pad pad;
        if (!android::input::Poll(user, pad))
        {
            if (!any) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
            continue;
        }
        const uint64_t now_keys = uint64_t(pad.buttons)
            | (pad.leftTrigger > kTriggerThreshold ? 1ull << 16 : 0)
            | (pad.rightTrigger > kTriggerThreshold ? 1ull << 17 : 0)
            | Stick(pad.leftX, pad.leftY, 18) | Stick(pad.rightX, pad.rightY, 26);
        Keys& keys = g_keys[user & 3];

        if (keys.repeating >= 0)
        {
            if (keys.waiting && now - keys.since >= kRepeatDelay) { keys.waiting = false; keys.since = now; }
            else if (!keys.waiting && now - keys.since >= kRepeatRate)
            {
                keys.since = now;
                Fill(out, kVk[keys.repeating], kKeyDown | kKeyRepeat, user);
                ctx.r3.u64 = X_ERROR_SUCCESS;
                return;
            }
        }

        const uint64_t changed = now_keys ^ keys.last;
        if (!changed) continue;
        // Releases first, then presses, so a stick moving between directions
        // ends one before it starts the next.
        for (int pass = 0; pass < 2; pass++)
            for (int bit = 0; bit < 34; bit++)
            {
                const uint64_t mask = 1ull << bit;
                if (!(changed & mask) || !kVk[bit]) continue;
                const bool pressed = (now_keys & mask) != 0;
                if (pass == 0 && !pressed)
                {
                    keys.last &= ~mask;
                    if (keys.repeating == bit) keys.repeating = -1;
                    Fill(out, kVk[bit], kKeyUp, user);
                    ctx.r3.u64 = X_ERROR_SUCCESS;
                    return;
                }
                if (pass == 1 && pressed)
                {
                    keys.last |= mask;
                    keys.repeating = bit; keys.waiting = true; keys.since = now;
                    Fill(out, kVk[bit], kKeyDown, user);
                    ctx.r3.u64 = X_ERROR_SUCCESS;
                    return;
                }
            }
        keys.last = now_keys;   // only buttons with no virtual key changed
    }
    ctx.r3.u64 = X_ERROR_EMPTY;
#else
    ctx.r3.u64 = X_ERROR_EMPTY;
#endif
}
