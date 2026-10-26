#include "screenshot.h"
#include "../../diagnostics.h"
#include "../../log.h"
#include "../../platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#ifdef PES17_ANDROID
#include "../../android/android.h"
#endif

namespace
{
    // Where the touch-a-file debug switches live, as renderer_resolve.cpp has it.
    std::string FilesDir()
    {
#ifdef PES17_ANDROID
        return android::GetPaths().external;
#else
        const char* dir = std::getenv("PES17_FILES");
        return dir ? dir : ".";
#endif
    }

    std::mutex g_lock;
    std::string g_requested;          // a Request() not yet taken
    std::vector<double> g_scriptAt;   // "<t>:shot" entries, in order
    size_t g_scriptNext = 0;
    double g_every = 0, g_lastEvery = 0, g_lastPoll = 0;
    std::once_flag g_once;

    void ReadSwitches()
    {
        g_every = diag::Real("PES17_SHOT_EVERY", 0.0);
        if (const char* text = diag::Text("PES17_INPUT_SCRIPT"))
            for (const char* at = text; *at; )
            {
                const char* comma = std::strchr(at, ',');
                const std::string entry(at, comma ? comma - at : std::strlen(at));
                const size_t colon = entry.find(':');
                if (colon != std::string::npos && entry.compare(colon + 1, std::string::npos, "shot") == 0)
                    g_scriptAt.push_back(std::atof(entry.substr(0, colon).c_str()));
                if (!comma) break;
                at = comma + 1;
            }
        std::sort(g_scriptAt.begin(), g_scriptAt.end());
        if (!g_scriptAt.empty() || g_every > 0)
            LOGI("screenshot: %zu scripted, every %.1f s, into %s/shots", g_scriptAt.size(), g_every,
                 FilesDir().c_str());
    }

    // PNG, uncompressed: deflate's stored blocks need no compressor, and a
    // debugging picture is read once and deleted.
    uint32_t Crc(const uint8_t* data, size_t size, uint32_t crc = 0)
    {
        static uint32_t table[256];
        static std::once_flag made;
        std::call_once(made, [] {
            for (uint32_t n = 0; n < 256; n++)
            {
                uint32_t c = n;
                for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[n] = c;
            }
        });
        crc = ~crc;
        for (size_t i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    void Put32(std::vector<uint8_t>& out, uint32_t v)
    {
        out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16));
        out.push_back(uint8_t(v >> 8));  out.push_back(uint8_t(v));
    }

    void Chunk(std::FILE* f, const char* type, const std::vector<uint8_t>& body)
    {
        std::vector<uint8_t> head;
        Put32(head, uint32_t(body.size()));
        head.insert(head.end(), type, type + 4);
        std::fwrite(head.data(), 1, head.size(), f);
        if (!body.empty()) std::fwrite(body.data(), 1, body.size(), f);
        std::vector<uint8_t> tail;
        Put32(tail, Crc(body.data(), body.size(), Crc(head.data() + 4, 4)));
        std::fwrite(tail.data(), 1, tail.size(), f);
    }

    // `rgb` is three bytes a pixel, rows top to bottom.
    bool WritePng(const std::string& path, const std::vector<uint8_t>& rgb, uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> raw;
        raw.reserve(size_t(width * 3 + 1) * height);
        for (uint32_t y = 0; y < height; y++)
        {
            raw.push_back(0);   // no filter
            raw.insert(raw.end(), rgb.begin() + size_t(y) * width * 3, rgb.begin() + size_t(y + 1) * width * 3);
        }
        std::vector<uint8_t> z{ 0x78, 0x01 };
        z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
        uint32_t a = 1, b = 0;
        for (size_t at = 0; at < raw.size() || at == 0; )
        {
            const size_t n = std::min<size_t>(65535, raw.size() - at);
            const bool last = at + n == raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
            z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
            z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
            for (size_t i = at; i < at + n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
            at += n;
            if (last) break;
        }
        Put32(z, (b << 16) | a);

        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        static const uint8_t kSignature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        std::fwrite(kSignature, 1, 8, f);
        std::vector<uint8_t> header;
        Put32(header, width); Put32(header, height);
        header.insert(header.end(), { 8, 2, 0, 0, 0 });   // 8 bits, RGB
        Chunk(f, "IHDR", header);
        Chunk(f, "IDAT", z);
        Chunk(f, "IEND", {});
        return std::fclose(f) == 0;
    }
}

void vk::screenshot::Request(const char* why)
{
    std::lock_guard lock(g_lock);
    g_requested = why;
}

bool vk::screenshot::Due(std::string& why)
{
    if constexpr (!diag::kOn) return false;
    std::call_once(g_once, ReadSwitches);
    const double now = platform::SecondsSinceStart();
    std::lock_guard lock(g_lock);
    if (!g_requested.empty()) { why = std::move(g_requested); g_requested.clear(); return true; }
    if (g_scriptNext < g_scriptAt.size() && now >= g_scriptAt[g_scriptNext])
    {
        while (g_scriptNext < g_scriptAt.size() && now >= g_scriptAt[g_scriptNext]) g_scriptNext++;
        why = "script";
        return true;
    }
    if (g_every > 0 && now - g_lastEvery >= g_every) { g_lastEvery = now; why = "every"; return true; }
    if (now - g_lastPoll >= 0.5)
    {
        g_lastPoll = now;
        const std::string trigger = FilesDir() + "/screenshot";
        if (std::FILE* f = std::fopen(trigger.c_str(), "r"))
        {
            std::fclose(f);
            std::remove(trigger.c_str());
            why = "file";
            return true;
        }
    }
    return false;
}

void vk::screenshot::Save(std::vector<uint8_t> pixels, uint32_t width, uint32_t height, bool bgra, std::string why)
{
    const double now = platform::SecondsSinceStart();
    std::thread([pixels = std::move(pixels), width, height, bgra, why = std::move(why), now] {
        // A Retina or 4K window halves until it is no wider than 1920: the
        // picture is for reading text, not for counting pixels.
        uint32_t step = 1;
        while (width / step > 1920) step *= 2;
        const uint32_t w = width / step, h = height / step;
        std::vector<uint8_t> rgb(size_t(w) * h * 3);
        const int r = bgra ? 2 : 0, bl = bgra ? 0 : 2;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                uint32_t sum[3]{};
                for (uint32_t dy = 0; dy < step; dy++)
                    for (uint32_t dx = 0; dx < step; dx++)
                    {
                        const uint8_t* p = &pixels[(size_t(y * step + dy) * width + x * step + dx) * 4];
                        sum[0] += p[r]; sum[1] += p[1]; sum[2] += p[bl];
                    }
                uint8_t* out = &rgb[(size_t(y) * w + x) * 3];
                for (int c = 0; c < 3; c++) out[c] = uint8_t(sum[c] / (step * step));
            }

        const std::string dir = FilesDir() + "/shots";
        ::mkdir(dir.c_str(), 0755);
        char name[96];
        std::snprintf(name, sizeof name, "/shot_%06.1fs_%s.png", now, why.c_str());
        const std::string path = dir + name;
        if (WritePng(path, rgb, w, h)) LOGI("screenshot: %s (%ux%u)", path.c_str(), w, h);
        else LOGW("screenshot: could not write %s", path.c_str());
    }).detach();
}
