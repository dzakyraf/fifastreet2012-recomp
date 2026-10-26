// What kernel::WriteFileAtomically and kernel::SyncStream promise.
//
// The promise is about what is on disk at every instant, which is awkward to
// test from inside one process: the failure these guard against is the process
// not reaching its next line. So rather than trying to be killed, these check
// the properties that make being killed survivable.
//
//   * The target is never opened for truncation, so previous contents cannot be
//     destroyed before the replacement is whole. Checked by writing a second
//     version and finding the first intact under "<path>.bak".
//   * A successful write leaves no ".tmp" behind, and a ".tmp" left by an
//     earlier run that died is not mistaken for anything.
//   * Bytes round-trip exactly, including sizes that cross block boundaries and
//     the empty file.
//
// Build with -DPES17_TESTS=ON and run; it needs no game files and writes only
// under a temporary directory of its own.
#include "kernel/durable.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool ok, const std::string& what)
    {
        g_checks++;
        if (ok) return;
        g_failures++;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }

    std::vector<uint8_t> Read(const fs::path& path)
    {
        std::vector<uint8_t> out;
        std::FILE* f = std::fopen(path.string().c_str(), "rb");
        if (!f) return out;
        uint8_t block[4096];
        size_t got;
        while ((got = std::fread(block, 1, sizeof block, f)) > 0)
            out.insert(out.end(), block, block + got);
        std::fclose(f);
        return out;
    }

    std::vector<uint8_t> Pattern(size_t size, uint8_t seed)
    {
        std::vector<uint8_t> out(size);
        for (size_t i = 0; i < size; i++) out[i] = uint8_t(seed + (i * 31));
        return out;
    }

    // The bug this whole file exists for: a save interrupted part-way used to
    // leave the player with nothing, because the target was opened "wb" and so
    // emptied before the first new byte was written.
    void PreviousContentsSurviveAReplacement(const fs::path& dir)
    {
        const fs::path path = dir / "profile.bin";
        fs::path backup = path; backup += ".bak";

        const auto first = Pattern(2000, 0x11);
        Check(kernel::WriteFileAtomically(path, first.data(), first.size()),
              "the first write reports success");
        Check(Read(path) == first, "the first write is readable in full");

        // Large enough that the write cannot be one atomic block at any layer,
        // which is exactly when being interrupted used to be fatal.
        const auto second = Pattern(300 * 1024, 0x77);
        Check(kernel::WriteFileAtomically(path, second.data(), second.size()),
              "the second write reports success");
        Check(Read(path) == second, "the second write replaced the contents");
        Check(Read(backup) == first,
              "the contents it replaced are whole under .bak (the old code lost them)");
    }

    void ASuccessfulWriteLeavesNoTemporary(const fs::path& dir)
    {
        const fs::path path = dir / "clean.bin";
        fs::path temporary = path; temporary += ".tmp";

        const auto bytes = Pattern(64, 0x05);
        Check(kernel::WriteFileAtomically(path, bytes.data(), bytes.size()),
              "the write reports success");
        std::error_code ec;
        Check(!fs::exists(temporary, ec), "no .tmp is left behind");
    }

    // A .tmp on disk means an earlier run died between creating it and the
    // rename. It is not the target and must not be treated as one.
    void ATemporaryFromADeadRunIsIgnored(const fs::path& dir)
    {
        const fs::path path = dir / "stale.bin";
        fs::path temporary = path; temporary += ".tmp";

        const auto good = Pattern(128, 0x22);
        Check(kernel::WriteFileAtomically(path, good.data(), good.size()),
              "the real file is written");

        // Plant rubbish under the temporary name, as a killed run would.
        const auto rubbish = Pattern(999, 0xEE);
        std::FILE* f = std::fopen(temporary.string().c_str(), "wb");
        Check(f != nullptr, "the stale .tmp can be planted");
        if (f) { std::fwrite(rubbish.data(), 1, rubbish.size(), f); std::fclose(f); }

        Check(Read(path) == good, "the real file still reads as itself");

        const auto next = Pattern(128, 0x33);
        Check(kernel::WriteFileAtomically(path, next.data(), next.size()),
              "the next write succeeds over a stale .tmp");
        Check(Read(path) == next, "and lands the new contents");
    }

    void SizesRoundTripExactly(const fs::path& dir)
    {
        // Empty, one byte, and either side of a 4 KB block.
        for (size_t size : { size_t(0), size_t(1), size_t(4095), size_t(4096), size_t(4097) })
        {
            const fs::path path = dir / ("size_" + std::to_string(size) + ".bin");
            const auto bytes = Pattern(size, uint8_t(size));
            Check(kernel::WriteFileAtomically(path, bytes.data(), bytes.size()),
                  "a " + std::to_string(size) + "-byte write succeeds");
            const auto back = Read(path);
            Check(back.size() == size, "a " + std::to_string(size) + "-byte file is that long");
            Check(back == bytes, "a " + std::to_string(size) + "-byte file round-trips");
        }
    }

    void TheDirectoryIsCreatedIfMissing(const fs::path& dir)
    {
        // SaveRoot() may not exist on a first run, and the write has to make it
        // rather than fail.
        const fs::path path = dir / "not" / "there" / "yet" / "profile.bin";
        const auto bytes = Pattern(32, 0x44);
        Check(kernel::WriteFileAtomically(path, bytes.data(), bytes.size()),
              "a write into a missing directory succeeds");
        Check(Read(path) == bytes, "and the file is there");
    }

    void SyncStreamReportsHonestly()
    {
        Check(!kernel::SyncStream(nullptr), "syncing nothing fails rather than claiming success");
    }
}

int main()
{
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "pes17_durable_test";
    fs::remove_all(dir, ec);
    if (!fs::create_directories(dir, ec))
    {
        std::fprintf(stderr, "cannot make %s: %s\n", dir.string().c_str(), ec.message().c_str());
        return 2;
    }

    PreviousContentsSurviveAReplacement(dir);
    ASuccessfulWriteLeavesNoTemporary(dir);
    ATemporaryFromADeadRunIsIgnored(dir);
    SizesRoundTripExactly(dir);
    TheDirectoryIsCreatedIfMissing(dir);
    SyncStreamReportsHonestly();

    fs::remove_all(dir, ec);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
