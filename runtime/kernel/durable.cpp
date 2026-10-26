// Making a write survive losing the process.
//
// Two things have to be true for a save to be safe, and stdio gives neither on
// its own.
//
// The first is that the bytes reach storage. fflush moves them out of the
// process's buffer and no further -- the kernel holds them in its page cache and
// writes them back when it likes -- so a kill, a reset or a flat battery
// between the flush and the writeback loses them. fsync is the call that waits
// for storage.
//
// The second is that a file is never half-replaced. Opening the real file with
// "wb" truncates it before the first byte of the new contents is written, so the
// window between the open and the close holds no whole file at all. Writing a
// temporary and renaming it over the target closes that window: rename is
// atomic, so every instant has one name or the other holding something whole.
//
// This matters more here than on the console it is standing in for. Android's
// low memory killer takes the process away by design, a lost Vulkan device
// exits the process outright (renderer.cpp, OnDeviceLost), and the manifest
// sets allowBackup="false", so there is nothing to restore from if a save goes.
#include "durable.h"
#include "../log.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
    // By the path's own characters, as the file layer does it: on Windows a
    // narrow path is in the ANSI code page, which cannot spell every folder a
    // player may have.
    std::FILE* Open(const fs::path& path, const char* mode)
    {
#ifdef _WIN32
        const std::wstring wide(mode, mode + std::strlen(mode));
        return _wfopen(path.c_str(), wide.c_str());
#else
        return std::fopen(path.c_str(), mode);
#endif
    }

    // A rename is only as durable as the directory entry it made, and that
    // entry is itself in the page cache until the directory is synced. Without
    // this a crash can leave the old name in place with nothing under it.
    // POSIX only: Windows offers no directory handle to sync, and its rename
    // is already ordered against the file's own data.
    void SyncDirectory(const fs::path& directory)
    {
#ifndef _WIN32
        const fs::path where = directory.empty() ? fs::path(".") : directory;
        const int fd = ::open(where.c_str(), O_RDONLY);
        if (fd < 0) return;
        ::fsync(fd);
        ::close(fd);
#else
        (void)directory;
#endif
    }
}

bool kernel::SyncStream(std::FILE* f)
{
    if (!f) return false;
    if (std::fflush(f) != 0) return false;
#ifdef _WIN32
    return ::_commit(::_fileno(f)) == 0;
#else
    return ::fsync(::fileno(f)) == 0;
#endif
}

bool kernel::WriteFileAtomically(const fs::path& path, const void* bytes, size_t size)
{
    std::error_code ec;
    const fs::path directory = path.parent_path();
    if (!directory.empty()) fs::create_directories(directory, ec);

    fs::path temporary = path; temporary += ".tmp";
    fs::path backup    = path; backup    += ".bak";

    // All of it into the temporary, and synced before it is given the real
    // name, so the name can never be seen with a short file under it.
    std::FILE* f = Open(temporary, "wb");
    if (!f)
    {
        LOGE("save: cannot create %s", temporary.string().c_str());
        return false;
    }
    const bool written = size == 0 || std::fwrite(bytes, 1, size, f) == size;
    const bool synced  = written && kernel::SyncStream(f);
    std::fclose(f);
    if (!synced)
    {
        LOGE("save: %s was not written in full; %s is left as it was",
             temporary.string().c_str(), path.string().c_str());
        fs::remove(temporary, ec);
        return false;
    }

    // The previous contents move aside rather than being overwritten. That
    // leaves no instant without a whole file: until the rename below lands the
    // backup is the good copy, and a reader that finds the real name missing or
    // unreadable falls back to it (LoadLocked in profile.cpp).
    const bool had = fs::exists(path, ec);
    ec.clear();
    if (had)
    {
        fs::remove(backup, ec);
        ec.clear();
        fs::rename(path, backup, ec);
        if (ec)
        {
            LOGW("save: cannot keep a backup of %s (%s)", path.string().c_str(),
                 ec.message().c_str());
            ec.clear();
        }
    }

    fs::rename(temporary, path, ec);
    if (ec)
    {
        LOGE("save: cannot put %s in place (%s)", path.string().c_str(), ec.message().c_str());
        // Leave the old one under its own name rather than nothing at all.
        std::error_code restore;
        if (had && fs::exists(backup, restore)) fs::rename(backup, path, restore);
        fs::remove(temporary, restore);
        return false;
    }

    SyncDirectory(directory);
    return true;
}
