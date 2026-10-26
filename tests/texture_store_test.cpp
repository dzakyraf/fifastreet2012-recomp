// The store of converted textures (runtime/gpu/vulkan/texture_store.cpp): what one
// session saves, the next reads back byte for byte; a record a crash cut short is
// dropped rather than trusted; other source bytes are not answered with it; and a
// file made by another encoder version is started again.
//
// The store opens once a process, so each session is a run of this program of its
// own: with no argument it is the driver, and runs itself as `write`, `read` and
// `stale`, one session each.
#include "gpu/vulkan/texture_store.h"
#include "gpu/texture.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        if (ok) return;
        g_failures++;
        std::printf("FAIL: %s\n", what);
    }

    // The same texture every run: a 64x64 BC1 source with two levels, and a
    // made-up converted form of it (the store keeps whatever it is handed).
    gpu::TextureData Source()
    {
        gpu::TextureData data;
        data.ok = true;
        data.width = data.height = 64;
        data.blocksWide = data.blocksHigh = 16;
        data.bytesPerBlock = 8;
        gpu::TextureData::Level base{ 64, 64, 1, 16, 16, 1, 0, 16 * 16 * 8 };
        gpu::TextureData::Level next{ 32, 32, 1, 8, 8, 1, base.size, 8 * 8 * 8 };
        data.levels = { base, next };
        data.bytes.resize(base.size + next.size);
        uint32_t state = 12345;
        for (uint8_t& b : data.bytes) b = uint8_t((state = state * 1664525u + 1013904223u) >> 24);
        return data;
    }

    gpu::TextureData Converted(const gpu::TextureData& source)
    {
        gpu::TextureData out = source;
        out.bytesPerBlock = 16;
        out.levels[0].size *= 2;
        out.levels[1].offset = out.levels[0].size;
        out.levels[1].size *= 2;
        out.bytes.resize(source.bytes.size() * 2);
        for (size_t i = 0; i < out.bytes.size(); i++) out.bytes[i] = uint8_t(i * 7 + 3);
        return out;
    }

    constexpr uint32_t kFormat = 18;     // DXT1
    constexpr int kVkFormat = 157;       // VK_FORMAT_ASTC_4x4_UNORM_BLOCK

    bool WaitFor(bool (*ready)())
    {
        for (int i = 0; i < 500; i++)
        {
            if (ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool Same(const gpu::TextureData& a, const gpu::TextureData& b)
    {
        if (a.bytes != b.bytes || a.levels.size() != b.levels.size() || a.bytesPerBlock != b.bytesPerBlock)
            return false;
        for (size_t i = 0; i < a.levels.size(); i++)
            if (a.levels[i].offset != b.levels[i].offset || a.levels[i].size != b.levels[i].size ||
                a.levels[i].blocksWide != b.levels[i].blocksWide)
                return false;
        return a.width == b.width && a.height == b.height && a.minLevel == b.minLevel;
    }

    long SizeOf(const char* path)
    {
        std::FILE* f = std::fopen(path, "rb");
        if (!f) return -1;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fclose(f);
        return size;
    }

    int Session(const std::string& phase)
    {
        Check(WaitFor([] { return vk::texstore::Open(); }), "the store opens");
        const gpu::TextureData source = Source();
        const gpu::TextureData converted = Converted(source);
        gpu::TextureData out;
        int vkFormat = 0;
        uint64_t hash = 0;
        if (phase == "write")
        {
            Check(!vk::texstore::Load(source, kFormat, out, vkFormat, hash), "an empty store has nothing");
            Check(hash != 0, "a miss hands back the source's hash");
            vk::texstore::Save(source, kFormat, hash, converted, kVkFormat);
            Check(WaitFor([] { return vk::texstore::Counts().saved == 1; }), "the record is written");
            Check(vk::texstore::Load(source, kFormat, out, vkFormat, hash), "found again in the same session");
            Check(Same(out, converted) && vkFormat == kVkFormat, "and read back whole");
        }
        else if (phase == "read")
        {
            Check(vk::texstore::Counts().records == 1, "the torn record is dropped, the whole one kept");
            Check(vk::texstore::Load(source, kFormat, out, vkFormat, hash), "found in the next session");
            Check(Same(out, converted) && vkFormat == kVkFormat, "byte for byte, with its levels");
            gpu::TextureData other = source;
            other.bytes[100] ^= 1;
            Check(!vk::texstore::Load(other, kFormat, out, vkFormat, hash), "one byte off is another texture");
            Check(!vk::texstore::Load(source, 20, out, vkFormat, hash), "another format is another texture");
        }
        else if (phase == "stale")
        {
            Check(vk::texstore::Counts().records == 0, "another encoder's file is started again");
            Check(!vk::texstore::Load(source, kFormat, out, vkFormat, hash), "and holds nothing");
        }
        return g_failures ? 1 : 0;
    }

    int Run(const std::string& self, const char* phase, const char* path)
    {
        const std::string command = "PES17_TEXTURE_STORE='" + std::string(path) + "' '" + self + "' " + phase;
        const int status = std::system(command.c_str());
        if (status != 0) std::printf("FAIL: the %s session\n", phase);
        return status != 0;
    }
}

int main(int argc, char** argv)
{
    if (argc > 1) return Session(argv[1]);

    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                             "/pes17_texture_store_test.bin";
    std::remove(path.c_str());
    int failed = Run(argv[0], "write", path.c_str());

    // A crash part-way through the next record: its head is there, its bytes are not.
    const long whole = SizeOf(path.c_str());
    if (std::FILE* f = std::fopen(path.c_str(), "ab"))
    {
        std::vector<uint8_t> torn(96, 0xAB);
        std::memcpy(torn.data(), "TXR1", 4);
        std::fwrite(torn.data(), 1, torn.size(), f);
        std::fclose(f);
    }
    failed += Run(argv[0], "read", path.c_str());
    Check(SizeOf(path.c_str()) == whole, "the file ends after the last whole record again");

    // The header's encoder version (bytes 12..15) from some other build.
    if (std::FILE* f = std::fopen(path.c_str(), "r+b"))
    {
        std::fseek(f, 12, SEEK_SET);
        const uint32_t other = 0xFFFF;
        std::fwrite(&other, sizeof other, 1, f);
        std::fclose(f);
    }
    failed += Run(argv[0], "stale", path.c_str());
    std::remove(path.c_str());

    if (failed || g_failures)
    {
        std::printf("texture store: FAILED\n");
        return 1;
    }
    std::printf("texture store: all checks passed\n");
    return 0;
}
