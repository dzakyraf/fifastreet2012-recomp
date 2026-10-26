// File I/O. Guest paths look like "game:\\zone\\common.ff" or
// "\\Device\\Cdrom0\\...", and the console filesystem is case-insensitive, so
// paths are mapped onto the game root and resolved case-insensitively.
//
// The game root is the disc and is read-only. The one writable place is a
// mounted content package -- "save0:" and friends, put there by the content
// layer in content.cpp -- so a path whose device names a mount resolves under
// that mount instead, and may be created, written, truncated and deleted.
#include <atomic>
#include <chrono>
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "objects.h"
#include "../guest.h"
#include "../diagnostics.h"
#include "../env.h"
#include "../log.h"
#include "../crash.h"
#include "../stream_trace.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

#include <unistd.h>
#include <vector>
#include "trace.h"
#include "window_trace.h"

using namespace kernel;
namespace fs = std::filesystem;

namespace
{
    // By the path's own characters: on Windows a narrow path is in the ANSI
    // code page, which cannot spell every folder a player may have.
    std::FILE* Open(const fs::path& path, const char* mode)
    {
#ifdef _WIN32
        const std::wstring wide(mode, mode + std::strlen(mode));
        return _wfopen(path.c_str(), wide.c_str());
#else
        return std::fopen(path.c_str(), mode);
#endif
    }

    // Positions past 2 GB: `long` is 32 bits on Windows.
    void Seek(std::FILE* f, int64_t offset)
    {
#ifdef _WIN32
        _fseeki64(f, offset, SEEK_SET);
#else
        fseeko(f, off_t(offset), SEEK_SET);
#endif
    }
    uint64_t Tell(std::FILE* f)
    {
#ifdef _WIN32
        return uint64_t(_ftelli64(f));
#else
        return uint64_t(ftello(f));
#endif
    }

    struct FileObject : Object
    {
        std::FILE* handle = nullptr;
        fs::path path;
        uint64_t size = 0;
        bool isDirectory = false;
        bool writable = false;
        bool deleteOnClose = false;
        std::mutex lock;

        const char* TypeName() const override { return "file"; }
        ~FileObject() override
        {
            // Writable is only ever true inside a mounted save package -- the
            // disc is read-only and a write anywhere else is discarded before it
            // reaches a handle -- so this is paid on saves and nothing else, and
            // it puts the bytes on storage before the title is told the handle
            // has gone. Pointless for one that is about to be deleted.
            if (handle && writable && !deleteOnClose) kernel::SyncStream(handle);
            if (handle) std::fclose(handle);
            // NT has no delete call of its own: a caller marks the file and the
            // deletion happens when the last handle goes.
            if (deleteOnClose)
            {
                std::error_code ec;
                fs::remove_all(path, ec);
            }
        }
    };

    struct XAnsiString { be16 length; be16 maximumLength; be32 pointer; };
    struct XObjectAttributes { be32 rootDirectory; be32 namePointer; be32 attributes; };
    struct XIoStatusBlock { be32 status; be32 information; };

    std::string ReadObjectName(uint32_t objectAttributes)
    {
        auto* oa = GuestPtr<XObjectAttributes>(objectAttributes);
        if (!oa) return {};
        auto* s = GuestPtr<XAnsiString>(uint32_t(oa->namePointer));
        if (!s) return {};
        uint32_t ptr = s->pointer;
        uint16_t len = s->length;
        if (!ptr || !len) return {};
        return std::string(reinterpret_cast<const char*>(guest::Base() + ptr), len);
    }

    // One path component at a time.
    bool ResolveInsensitive(const fs::path& root, const std::string& relative, fs::path& out)
    {
        fs::path current = root;
        std::string component;
        auto step = [&](const std::string& name) -> bool
        {
            if (name == "..") return false;
            if (name.empty() || name == ".") return true;
            fs::path direct = current / name;
            std::error_code ec;
            if (fs::exists(direct, ec)) { current = direct; return true; }
            if (!fs::is_directory(current, ec)) return false;
            for (auto& e : fs::directory_iterator(current, ec))
            {
                std::string have = e.path().filename().string();
                if (have.size() != name.size()) continue;
                if (std::equal(have.begin(), have.end(), name.begin(),
                               [](char a, char b){ return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); }))
                { current = e.path(); return true; }
            }
            return false;
        };

        for (char c : relative)
        {
            if (c == '\\' || c == '/') { if (!step(component)) return false; component.clear(); }
            else component += c;
        }
        if (!step(component)) return false;
        out = current;
        return true;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower(uint8_t(c)));
        return s;
    }

    // "game:\\zone\\common.ff"        -> device "game",   path "zone\\common.ff"
    // "\\Device\\Cdrom0\\zone\\x.ff" -> device "cdrom0", path "zone\\x.ff"
    void SplitDevice(const std::string& guestPath, std::string& device, std::string& relative)
    {
        std::string p = guestPath;
        if (auto colon = p.find(':'); colon != std::string::npos)
        {
            device = Lower(p.substr(0, colon));
            p = p.substr(colon + 1);
        }
        else if (p.rfind("\\Device\\", 0) == 0 || p.rfind("\\device\\", 0) == 0)
        {
            size_t slash = p.find('\\', 8);
            device = Lower(p.substr(8, slash == std::string::npos ? slash : slash - 8));
            p = (slash == std::string::npos) ? std::string() : p.substr(slash + 1);
        }
        while (!p.empty() && (p.front() == '\\' || p.front() == '/')) p.erase(p.begin());
        relative = p;
    }

    // The mount table the content layer fills in. Everything not in it is the
    // disc, which is read-only.
    std::mutex g_mountLock;
    std::map<std::string, fs::path> g_mounts;

    fs::path DeviceRoot(const std::string& device, bool& writable)
    {
        std::lock_guard g(g_mountLock);
        auto it = g_mounts.find(device);
        if (it == g_mounts.end()) { writable = false; return GameRoot(); }
        writable = true;
        return it->second;
    }

    // Where a name that is not there yet would go. Only ever used under a mount,
    // so there is no case-insensitive match to make: this name is the one being
    // created.
    fs::path HostPath(const fs::path& root, const std::string& relative)
    {
        fs::path out = root;
        std::string component;
        for (char c : relative)
        {
            if (c == '\\' || c == '/')
            {
                if (component == "..") component.clear();
                else if (!component.empty()) { out /= component; component.clear(); }
            }
            else component += c;
        }
        if (component != ".." && !component.empty()) out /= component;
        return out;
    }

    std::mutex g_missingLock;
    std::map<std::string, uint64_t> g_missing;

    void NoteMissing(const std::string& guestPath)
    {
        // Logged in every build: a port's first question is which files the
        // title asks for and does not find.
        // An empty name means the open carried no ANSI_STRING at all, which is a
        // different bug from a genuinely absent file.
        const std::string key = guestPath.empty() ? "<no name in OBJECT_ATTRIBUTES>" : guestPath;
        std::lock_guard g(g_missingLock);
        auto [it, inserted] = g_missing.try_emplace(key, 0);
        it->second++;
        if (inserted) LOGK("file not found: %s", key.c_str());
    }

    bool TraceFiles()
    {
        static const bool on = diag::Flag("PES17_TRACE_FILES");
        return on;
    }

    // NT create dispositions, and what the caller is told happened.
    constexpr uint32_t FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2,
                       FILE_OPEN_IF = 3, FILE_OVERWRITE = 4, FILE_OVERWRITE_IF = 5;
    constexpr uint32_t FILE_SUPERSEDED = 0, FILE_OPENED = 1, FILE_CREATED = 2,
                       FILE_OVERWRITTEN = 3;
    constexpr uint32_t FILE_DIRECTORY_FILE = 0x00000001;

    bool WantsToWrite(uint32_t desiredAccess)
    {
        constexpr uint32_t GENERIC_WRITE = 0x40000000, GENERIC_ALL = 0x10000000;
        constexpr uint32_t FILE_WRITE_DATA = 0x0002, FILE_APPEND_DATA = 0x0004;
        return (desiredAccess & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    }

    uint32_t OpenFileCommon(const std::string& guestPath, uint32_t desiredAccess,
                            uint32_t disposition, uint32_t options,
                            uint32_t& handleOut, uint32_t& information)
    {
        std::string device, relative;
        SplitDevice(guestPath, device, relative);
        bool writable = false;
        const fs::path root = DeviceRoot(device, writable);

        fs::path resolved;
        bool existed = false;
        if (!writable && !relative.empty())
        {
            static const fs::path modDir = [] {
                if (const char* envMod = env::Text("PES17_MOD_DIR"))
                    return fs::path(envMod);
                return GameRoot() / "mods";
            }();
            std::error_code ec;
            if (fs::is_directory(modDir, ec))
                existed = ResolveInsensitive(modDir, relative, resolved);
        }
        if (!existed)
            existed = !relative.empty() && ResolveInsensitive(root, relative, resolved);
        if (!existed)
        {
            // A name that is not there is only an error until something is allowed
            // to make it: the disc cannot be written to, and a disposition that
            // only opens does not create.
            if (relative.empty() || !writable ||
                disposition == FILE_OPEN || disposition == FILE_OVERWRITE)
            {
                NoteMissing(guestPath);
                return X_STATUS_OBJECT_NAME_NOT_FOUND;
            }
            resolved = HostPath(root, relative);
        }
        else if (disposition == FILE_CREATE)
        {
            return X_STATUS_OBJECT_NAME_COLLISION;
        }

        auto f = std::make_shared<FileObject>();
        f->path = resolved;
        std::error_code ec;
        if ((options & FILE_DIRECTORY_FILE) || (existed && fs::is_directory(resolved, ec)))
        {
            if (!existed && !fs::create_directories(resolved, ec))
                return X_STATUS_OBJECT_NAME_NOT_FOUND;
            f->isDirectory = true;
            information = existed ? FILE_OPENED : FILE_CREATED;
        }
        else
        {
            // Truncating dispositions and a file that is not there yet both mean
            // starting from empty; anything else keeps what is on disk. Only ever
            // under a mount: the disc is read-only, and a disposition asking to
            // overwrite one of its files must not be taken at its word.
            const bool truncate = writable &&
                                  (!existed || disposition == FILE_SUPERSEDE ||
                                   disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF);
            f->writable = writable && (truncate || WantsToWrite(desiredAccess));
            if (!existed) fs::create_directories(resolved.parent_path(), ec);

            const char* mode = truncate ? "w+b" : (f->writable ? "r+b" : "rb");
            f->handle = Open(resolved, mode);
            if (!f->handle) { NoteMissing(guestPath); return X_STATUS_OBJECT_NAME_NOT_FOUND; }
            f->size = truncate ? 0 : fs::file_size(resolved, ec);
            information = !existed ? FILE_CREATED
                        : truncate ? (disposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN)
                                   : FILE_OPENED;
        }
        handleOut = InsertHandle(f);
        static std::atomic<uint32_t> opens{ 0 };
        if (TraceFiles() || opens++ < 200)
            LOGK("open %s -> %s (%s)", guestPath.c_str(), resolved.string().c_str(),
                 f->writable ? "writable" : "read-only");
        return X_STATUS_SUCCESS;
    }
}

void kernel::MountDevice(const std::string& name, const fs::path& root)
{
    std::lock_guard g(g_mountLock);
    g_mounts[Lower(name)] = root;
}

void kernel::UnmountDevice(const std::string& name)
{
    std::lock_guard g(g_mountLock);
    g_mounts.erase(Lower(name));
}

void kernel::ReportMissingFiles()
{
    if constexpr (!diag::kOn) return;
    std::lock_guard g(g_missingLock);
    if (g_missing.empty()) return;
    LOGI("files the guest asked for and did not get (%zu distinct):", g_missing.size());
    size_t n = 0;
    for (auto& [path, count] : g_missing)
    {
        LOGI("  %8llu  %s", (unsigned long long)count, path.c_str());
        if (++n >= 40) { LOGI("  ... and %zu more", g_missing.size() - n); break; }
    }
}

PPC_FUNC(__imp__NtCreateFile)
{
    auto* handleOut = GuestPtr<be32>(ctx.r3.u32);
    std::string name = ReadObjectName(ctx.r5.u32);
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r6.u32);
    const uint32_t options = kernel::StackArgument(ctx, 8);   // CreateOptions

    uint32_t handle = 0, information = 0;
    uint32_t status = OpenFileCommon(name, ctx.r4.u32, ctx.r10.u32, options, handle, information);
    if (handleOut && status == X_STATUS_SUCCESS) *handleOut = handle;
    if (iosb) { iosb->status = status; iosb->information = (status == X_STATUS_SUCCESS) ? information : 0u; }
    ctx.r3.u64 = status;
}

PPC_FUNC(__imp__NtOpenFile)
{
    auto* handleOut = GuestPtr<be32>(ctx.r3.u32);
    std::string name = ReadObjectName(ctx.r5.u32);
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r6.u32);

    uint32_t handle = 0, information = 0;
    uint32_t status = OpenFileCommon(name, ctx.r4.u32, FILE_OPEN, ctx.r8.u32, handle, information);
    if (handleOut && status == X_STATUS_SUCCESS) *handleOut = handle;
    if (iosb) { iosb->status = status; iosb->information = (status == X_STATUS_SUCCESS) ? information : 0u; }
    ctx.r3.u64 = status;
}

PPC_FUNC(__imp__NtReadFile)
{
    auto file = std::dynamic_pointer_cast<FileObject>(LookupHandle(ctx.r3.u32));
    auto event = LookupHandleAs<Event>(ctx.r4.u32);
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r7.u32);
    uint32_t buffer = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t offsetPtr = ctx.r10.u32;

    if (!file || !file->handle || !buffer)
    {
        if (iosb) { iosb->status = X_STATUS_INVALID_PARAMETER; iosb->information = 0; }
        if (event) event->Set();
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    int64_t offset = offsetPtr ? int64_t(uint64_t(*GuestPtr<be64>(offsetPtr))) : -1;
    uint32_t apcRoutine = ctx.r5.u32 & ~3u;

    if (event) event->Reset();

    // Deferred to APC-delivery time when the caller asked for a completion
    // routine, so the buffer is filled at the moment the title is waiting for it
    // rather than while it is still decompressing out of the other half of its
    // double buffer.
    auto transfer = [file, event, buffer, length, offset, iosb]
    {
        std::lock_guard g(file->lock);
        // C requires a positioning call between a write and a read on the same
        // update stream; seeking to where it already is counts as one.
        if (offset >= 0) Seek(file->handle, offset);
        else if (file->writable) std::fseek(file->handle, 0, SEEK_CUR);
        // Into a buffer of our own and copied across, not read straight into
        // guest memory: a page the GPU's shadow watches is read-only, and the
        // kernel fails a read into it where a copy faults and carries on.
        thread_local std::vector<uint8_t> bounce;
        if (bounce.size() < length) bounce.resize(length);
        const long long position = (long long)std::ftell(file->handle);
        const auto readFrom = std::chrono::steady_clock::now();
        const size_t got = std::fread(bounce.data(), 1, length, file->handle);
        {
            // Throughput every two seconds: reads, MB, and the time spent waiting
            // in fread -- whether a cutscene's stall is the disc.
            static std::atomic<uint64_t> waitUs{ 0 }, windowBytes{ 0 }, windowReads{ 0 };
            static std::atomic<int64_t> windowStart{ 0 };
            const auto now = std::chrono::steady_clock::now();
            waitUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - readFrom).count());
            windowBytes += got; windowReads++;
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            int64_t start = windowStart.load();
            if (!start) windowStart.compare_exchange_strong(start, nowMs);
            else if (nowMs - start >= 2000 && windowStart.compare_exchange_strong(start, nowMs))
                LOGI("io: %llu reads, %.1f MB, %.0f ms in fread over %.1f s",
                     (unsigned long long)windowReads.exchange(0), windowBytes.exchange(0) / 1048576.0,
                     waitUs.exchange(0) / 1000.0, (nowMs - start) / 1000.0);
        }
        std::memcpy(guest::Base() + buffer, bounce.data(), got);
        stream_trace::Read(file->path.filename().string().c_str(), length, uint32_t(got),
                           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readFrom).count(),
                           buffer);
        uint32_t status = (got == 0 && length != 0) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
        if (iosb) { iosb->status = status; iosb->information = uint32_t(got); }
        if (event) event->Set();
        static std::atomic<uint32_t> reads{ 0 };
        static std::atomic<uint64_t> bytes{ 0 };
        const uint32_t n = reads++;
        const uint64_t total = bytes += got;
        // The first reads, then one in 500 with the running total: enough to see
        // whether loading moves without flooding the log.
        if (n % 100 == 0 && n)
            LOGI("files: %u reads, %.1f MB so far", n, total / 1048576.0);
        if (WindowTraceOn()) WindowTrace("read", crash::CurrentThreadName(), uint32_t(length), uint32_t(got), file->path.filename().string().c_str());
        static std::atomic<int64_t> lastReadUs{ 0 };
        const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t sincePrevious = nowUs - lastReadUs.exchange(nowUs);
        if (TraceFiles() || n < 60 || n % 5 == 0)
            LOGK("read %s: %u bytes at %lld (file position %lld) -> %zu into %08X (%s) on '%s', %lld ms after the last read",
                 file->path.filename().string().c_str(), length, (long long)offset, position, got, buffer,
                 status ? "eof" : "ok", crash::CurrentThreadName(), (long long)(sincePrevious / 1000));
    };

    if (!apcRoutine)
    {
        transfer();
        ctx.r3.u64 = iosb ? uint32_t(iosb->status) : X_STATUS_SUCCESS;
        return;
    }

    // The outcome lands in the status block when the APC runs. Bit 0 of the
    // routine is NT's APC-mode flag.
    if (iosb) { iosb->status = X_STATUS_PENDING; iosb->information = 0; }
    kernel::QueueUserApc(apcRoutine, ctx.r6.u32, ctx.r7.u32, std::move(transfer));
    ctx.r3.u64 = X_STATUS_PENDING;
}

PPC_FUNC(__imp__NtWriteFile)
{
    auto file = std::dynamic_pointer_cast<FileObject>(LookupHandle(ctx.r3.u32));
    auto event = LookupHandleAs<Event>(ctx.r4.u32);
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r7.u32);
    uint32_t buffer = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t offsetPtr = ctx.r10.u32;

    // A write to anything but a mounted package is a write to the disc -- the
    // title's own logs go there. Accept and discard those, as before.
    if (!file || !file->handle || !file->writable || !buffer)
    {
        if (iosb) { iosb->status = X_STATUS_SUCCESS; iosb->information = length; }
        if (event) event->Set();
        ctx.r3.u64 = X_STATUS_SUCCESS;
        return;
    }

    std::lock_guard g(file->lock);
    const int64_t offset = offsetPtr ? int64_t(uint64_t(*GuestPtr<be64>(offsetPtr))) : -1;
    if (offset >= 0) Seek(file->handle, offset);
    else std::fseek(file->handle, 0, SEEK_CUR);          // the same rule, the other way round
    const size_t put = std::fwrite(guest::Base() + buffer, 1, length, file->handle);
    std::fflush(file->handle);
    const uint64_t end = Tell(file->handle);
    if (end > file->size) file->size = end;

    const uint32_t status = (put == length) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
    if (iosb) { iosb->status = status; iosb->information = uint32_t(put); }
    if (event) event->Set();
    if (TraceFiles())
        LOGK("write %s: %u bytes at %lld -> %zu", file->path.filename().string().c_str(),
             length, (long long)offset, put);
    ctx.r3.u64 = status;
}

PPC_FUNC(__imp__NtQueryInformationFile)
{
    auto file = std::dynamic_pointer_cast<FileObject>(LookupHandle(ctx.r3.u32));
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r4.u32);
    uint32_t buffer = ctx.r5.u32;
    uint32_t infoClass = ctx.r7.u32;

    if (!file) { ctx.r3.u64 = X_STATUS_INVALID_PARAMETER; return; }

    constexpr uint32_t FileStandardInformation = 5;
    constexpr uint32_t FilePositionInformation = 14;
    constexpr uint32_t FileNetworkOpenInformation = 34;

    if (infoClass == FileStandardInformation && buffer)
    {
        auto* p = GuestPtr<be64>(buffer);
        p[0] = file->size;   // AllocationSize
        p[1] = file->size;   // EndOfFile
        auto* extra = GuestPtr<be32>(buffer + 16);
        extra[0] = 1;                                  // NumberOfLinks
        extra[1] = file->isDirectory ? 0x01000000u : 0;// DeletePending / Directory
    }
    else if (infoClass == FilePositionInformation && buffer)
    {
        std::lock_guard g(file->lock);
        *GuestPtr<be64>(buffer) = file->handle ? Tell(file->handle) : 0ull;
    }
    else if (infoClass == FileNetworkOpenInformation && buffer)
    {
        auto* p = GuestPtr<be64>(buffer);
        for (int i = 0; i < 4; i++) p[i] = 0;   // timestamps
        p[4] = file->size;                      // AllocationSize
        p[5] = file->size;                      // EndOfFile
    }

    if (iosb) { iosb->status = X_STATUS_SUCCESS; iosb->information = 0; }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtQueryFullAttributesFile)
{
    std::string name = ReadObjectName(ctx.r3.u32);
    std::string device, relative;
    SplitDevice(name, device, relative);
    bool writable = false;
    const fs::path root = DeviceRoot(device, writable);
    fs::path resolved;
    if (relative.empty() || !ResolveInsensitive(root, relative, resolved))
    {
        NoteMissing(name);
        ctx.r3.u64 = X_STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    std::error_code ec;
    bool dir = fs::is_directory(resolved, ec);
    uint64_t size = dir ? 0 : fs::file_size(resolved, ec);
    if (auto* p = GuestPtr<be64>(ctx.r4.u32))
    {
        for (int i = 0; i < 4; i++) p[i] = 0;
        p[4] = size;
        p[5] = size;
        *GuestPtr<be32>(ctx.r4.u32 + 48) = dir ? 0x10u : 0x80u;  // DIRECTORY / NORMAL
    }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtSetInformationFile)
{
    auto file = std::dynamic_pointer_cast<FileObject>(LookupHandle(ctx.r3.u32));
    auto* iosb = GuestPtr<XIoStatusBlock>(ctx.r4.u32);
    uint32_t buffer = ctx.r5.u32;
    uint32_t infoClass = ctx.r7.u32;

    constexpr uint32_t FileRenameInformation = 10;
    constexpr uint32_t FileDispositionInformation = 13;
    constexpr uint32_t FilePositionInformation = 14;
    constexpr uint32_t FileEndOfFileInformation = 20;

    if (file && buffer && infoClass == FileRenameInformation)
    {
        // { replaceIfExists, rootDirectory, ANSI_STRING newName }: how a save is
        // finished -- written as data.tmp, then renamed over data.bin.
        const bool replace = *GuestPtr<be32>(buffer) != 0;
        const uint32_t length = *GuestPtr<be16>(buffer + 8), data = *GuestPtr<be32>(buffer + 12);
        const std::string name(GuestPtr<const char>(data), length);
        std::string device, relative;
        SplitDevice(name, device, relative);
        bool writable = false;
        const fs::path root = DeviceRoot(device, writable);
        uint32_t status = X_STATUS_SUCCESS;
        if (!writable || relative.empty()) status = 0xC0000022u;
        else
        {
            const fs::path target = HostPath(root, relative);
            std::error_code ec;
            std::lock_guard g(file->lock);
            if (fs::exists(target, ec) && !replace) status = X_STATUS_OBJECT_NAME_COLLISION;
            else
            {
                if (file->handle) std::fflush(file->handle);
                fs::rename(file->path, target, ec);
                if (ec) status = 0xC0000022u;
                else { LOGK("file: renamed %s to %s", file->path.string().c_str(), target.string().c_str()); file->path = target; }
            }
        }
        if (iosb) { iosb->status = status; iosb->information = 0; }
        ctx.r3.u64 = status;
        return;
    }

    if (file && buffer)
    {
        std::lock_guard g(file->lock);
        if (infoClass == FilePositionInformation && file->handle)
        {
            Seek(file->handle, int64_t(uint64_t(*GuestPtr<be64>(buffer))));
        }
        else if (infoClass == FileEndOfFileInformation && file->handle && file->writable)
        {
            const uint64_t end = *GuestPtr<be64>(buffer);
            std::fflush(file->handle);
            if (ftruncate(fileno(file->handle), off_t(end)) == 0) file->size = end;
        }
        else if (infoClass == FileDispositionInformation)
        {
            // A single byte: non-zero means delete when the last handle closes.
            file->deleteOnClose = file->writable && *GuestPtr<uint8_t>(buffer) != 0;
        }
    }

    if (iosb) { iosb->status = X_STATUS_SUCCESS; iosb->information = 0; }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtFlushBuffersFile)
{
    if (auto file = std::dynamic_pointer_cast<FileObject>(LookupHandle(ctx.r3.u32)))
    {
        std::lock_guard g(file->lock);
        // The title asking for its save to be on disk, and the only chance it
        // gives to make that true. fflush on its own hands the bytes to the
        // kernel and returns; the kernel writes them back when it chooses, so a
        // kill in between loses a save the title was told had landed.
        if (file->handle) kernel::SyncStream(file->handle);
    }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtReadFileScatter) { ctx.r3.u64 = X_STATUS_NOT_IMPLEMENTED; }
