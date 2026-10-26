// Saved games.
//
// The title's save system is two steps. Memcard_* asks XContentCreate to mount
// a content package as a lettered drive -- "save0:" -- and then opens
// "save0:\savegame.svg" with the ordinary file API and writes it like any other
// file. Reporting no device is what puts the save-device prompt in front of the
// player before every level.
//
// A package on the console is an STFS container, a signed archive with the save
// inside it. Nothing here writes that format: a package is a directory under
// `saves/`, and mounting it is just an entry in the file layer's device table.
// The title never looks inside the container -- it only asks XAM to mount one
// and then reads and writes files through the drive letter -- so a directory is
// enough for it to save, load and delete.
#include <array>
#include <vector>
#include <cstdio>
#include <ppc_recomp_shared.h>
#include "title.h"
#include "kernel.h"
#include "objects.h"
#include "../guest.h"
#include "../log.h"
#include "../env.h"

#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include "trace.h"

using namespace kernel;
namespace fs = std::filesystem;

namespace
{
    // XCONTENT_DATA, as the title builds it: the device it chose, what kind of
    // content this is, a display name for the dashboard and the name on disk.
    constexpr uint32_t kContentDeviceId   = 0;
    constexpr uint32_t kContentType       = 4;
    constexpr uint32_t kContentFileName   = 264;
    constexpr uint32_t kContentFileMax    = 42;
    constexpr uint32_t kXContentDataSize  = 0x134;

    // XCONTENTFLAG_*: the low nibble of the flags is a create disposition, the
    // same idea as NT's.
    constexpr uint32_t kCreateNew = 1, kCreateAlways = 2, kOpenExisting = 3,
                       kOpenAlways = 4, kTruncate = 5;

    std::string GuestString(uint32_t address, size_t limit)
    {
        if (!address) return {};
        const char* p = reinterpret_cast<const char*>(guest::Base() + address);
        size_t n = 0;
        while (n < limit && p[n]) n++;
        return std::string(p, n);
    }

    // The name goes on the host filesystem, so it may only be a name.
    std::string SafeName(const std::string& in)
    {
        std::string out;
        for (char c : in)
        {
            const bool ok = std::isalnum(uint8_t(c)) || c == '_' || c == '-' || c == '.';
            out += ok ? c : '_';
        }
        while (!out.empty() && out.front() == '.') out.erase(out.begin());
        return out;
    }


    // Memcard_InitializeSystem keeps the device each controller saved to in a
    // table of one word per controller, and everything else asks that table
    // first: a zero there means "nobody has chosen yet", which is the state the
    // save-device prompt exists to get out of. On the console the choice is
    // remembered in the player's profile, so it is made once ever; here the
    // table starts empty on every launch, so the prompt would come back on every
    // launch. There is only one device to choose, so choose it.
    constexpr uint32_t kDeviceTable = T_DATA_DeviceTable;
    constexpr uint32_t kControllers = 4;

    bool ChooseDeviceForPlayer()
    {
        static const bool on = !env::Flag("PES17_NO_AUTO_SAVE_DEVICE");
        return on;
    }

    // A clearing disposition means the title wants the package empty to write a
    // new save into. Deleting the old one outright is what the console does,
    // but it opens a window as long as the title's own write in which the
    // player has no save at all -- and on Android the low memory killer can
    // take the process away inside it, which costs a campaign. The old package
    // moves aside instead, one generation deep, so some whole save is always on
    // disk under one name or the other.
    //
    // Nothing reads the backup automatically: the save is the title's own
    // format and there is no way from here to tell a truncated one from a
    // short one. Recovery is renaming "<name>.bak" back over "<name>".
    void RotatePackage(const fs::path& package)
    {
        std::error_code ec;
        fs::path backup = package;
        backup += ".bak";

        fs::remove_all(backup, ec);
        ec.clear();
        fs::rename(package, backup, ec);
        if (!ec)
        {
            LOGK("content: kept the previous %s as %s", package.filename().string().c_str(),
                 backup.filename().string().c_str());
            return;
        }

        // Could not move it aside, and the title still needs the name empty.
        LOGW("content: cannot keep a backup of %s (%s); emptying it in place",
             package.string().c_str(), ec.message().c_str());
        ec.clear();
        fs::remove_all(package, ec);
    }
}

const fs::path& kernel::SaveRoot()
{
    // Beside the runtime, not inside the game root: the game root is the disc.
    static const fs::path root = fs::path("saves");
    return root;
}

// Memcard_InitializeSystem. The table is clear when it returns, so this is the
// moment to answer the question the prompt would ask.
#ifdef T_Memcard_InitializeSystem
GUEST_HOOK(T_Memcard_InitializeSystem)
{
    GUEST_ORIG(T_Memcard_InitializeSystem)(ctx, base);
    if (!ChooseDeviceForPlayer() || !kDeviceTable) return;
    for (uint32_t i = 0; i < kControllers; i++)
    {
        auto* chosen = GuestPtr<be32>(kDeviceTable + i * 4);
        if (chosen && uint32_t(*chosen) == 0) *chosen = kSaveDeviceId;
    }
    LOGK("content: the hard disk is the save device for every controller");
}
#endif

// Which storage devices exist. The title asks before it offers to save, and a
// "no" here is the save-device prompt.
PPC_FUNC(__imp__XamContentGetDeviceState)
{
    const uint32_t deviceId = ctx.r3.u32;
    const uint32_t result = (deviceId == kSaveDeviceId) ? X_ERROR_SUCCESS
                                                        : X_ERROR_DEVICE_NOT_CONNECTED;
    CompleteOverlapped(ctx.r4.u32, result);
    ctx.r3.u64 = result;
}

// XDEVICE_DATA { id, type, totalBytes, freeBytes, friendlyName[28] }. The title
// compares the free space with the size it is about to write and refuses to
// save if it does not fit, so the numbers have to be real enough to pass.
PPC_FUNC(__imp__XamContentGetDeviceData)
{
    const uint32_t deviceId = ctx.r3.u32;
    if (deviceId != kSaveDeviceId) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }

    if (auto* data = GuestPtr<be32>(ctx.r4.u32))
    {
        constexpr uint64_t kTotal = 16ull * 1024 * 1024 * 1024;
        constexpr uint64_t kFree  =  8ull * 1024 * 1024 * 1024;
        std::memset(data, 0, 80);
        data[0] = kSaveDeviceId;
        data[1] = 1;                                   // XCONTENTDEVICETYPE_HDD
        *GuestPtr<be64>(ctx.r4.u32 + 8)  = kTotal;
        *GuestPtr<be64>(ctx.r4.u32 + 16) = kFree;
        auto* name = GuestPtr<be16>(ctx.r4.u32 + 24);  // 28 wide characters
        const char* friendly = "Hard Drive";
        for (int i = 0; friendly[i] && i < 27; i++) name[i] = uint16_t(friendly[i]);
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// Mount a package as a drive. The disposition in the low nibble of the flags
// says whether it may be created, and the caller is told which of the two
// happened so it knows whether there is a save in there to read.
static uint32_t ContentCreate(uint32_t rootName, uint32_t contentData, uint32_t flags,
                              uint32_t dispositionOut, uint32_t licenseOut, uint32_t overlapped)
{
    const std::string drive = GuestString(rootName, 32);
    const std::string file  = SafeName(GuestString(contentData + kContentFileName, kContentFileMax));
    if (drive.empty() || file.empty() || !contentData)
    {
        CompleteOverlapped(overlapped, X_ERROR_INVALID_PARAMETER);
        return X_ERROR_INVALID_PARAMETER;
    }

    LOGK("content: XamContentCreateEx root '%s' file '%s' flags %08X (disposition %u) device %u type %08X, license out %08X, overlapped %08X",
         drive.c_str(), file.c_str(), flags, flags & 0xF, unsigned(*GuestPtr<be32>(contentData + kContentDeviceId)),
         unsigned(*GuestPtr<be32>(contentData + kContentType)), licenseOut, overlapped);
    const uint32_t type = *GuestPtr<be32>(contentData + kContentType);
    const uint32_t device = *GuestPtr<be32>(contentData + kContentDeviceId);
    const fs::path package = SaveRoot() / file;
    std::error_code ec;
    const bool existed = fs::is_directory(package, ec);

    // "Create always" and "truncate" both mean the package the title gets is a
    // new empty one. Keeping the old contents instead is not a smaller version
    // of that: the title saves by creating its file with CREATE_NEW, so a
    // leftover file from the last save collides and the save fails.
    const uint32_t disposition = flags & 0xF;
    const bool clear = disposition == kCreateAlways || disposition == kTruncate;

    uint32_t result = X_ERROR_SUCCESS;
    switch (disposition)
    {
    case kCreateNew:    if (existed) result = X_ERROR_ALREADY_EXISTS; break;
    case kOpenExisting:
    case kTruncate:     if (!existed) result = X_ERROR_PATH_NOT_FOUND; break;
    case kCreateAlways:
    case kOpenAlways:
    default:            break;
    }
    if (!result && clear && existed) RotatePackage(package);
    if (!result && !fs::create_directories(package, ec) && !fs::is_directory(package, ec))
        result = X_ERROR_PATH_NOT_FOUND;

    if (!result)
    {
        // The title finds its saves again by enumerating packages, so what it passed
        // in (the display name and type the dashboard would list) is kept with them.
        const fs::path record = package / ".xcontent";
        if (!fs::exists(record, ec))
            if (std::FILE* out = std::fopen(record.string().c_str(), "wb"))
            {
                std::fwrite(guest::Base() + contentData, 1, kXContentDataSize, out);
                std::fclose(out);
            }
        MountDevice(drive, package);
        LOGK("content: mounted %s: on %s (device %u, type %u, %s)", drive.c_str(),
             package.string().c_str(), device, type,
             !existed ? "new" : clear ? "emptied" : "existing");
    }
    // Asked to open a package nobody has saved yet: the first look for a new
    // player's stats, or for a save before the first.
    else if (result == X_ERROR_PATH_NOT_FOUND && !existed)
        LOGK("content: there is no %s yet (%s: asked to open it)", package.string().c_str(), drive.c_str());
    else
    {
        LOGK("content: refused %s: on %s -- error %u", drive.c_str(),
             package.string().c_str(), result);
    }

    // XCONTENT_NEW / XCONTENT_EXISTING, and a licence mask of nothing, which is
    // what a package made on this console rather than bought carries.
    if (auto* d = GuestPtr<be32>(dispositionOut)) *d = existed ? 2u : 1u;
    if (auto* l = GuestPtr<be32>(licenseOut)) *l = 0;
    CompleteOverlapped(overlapped, result);
    return result;
}

PPC_FUNC(__imp__XamContentCreateEx)
{
    // (user, rootName, contentData, flags, dispositionOut, licenseOut, cacheSize, contentSize, overlapped)
    ctx.r3.u64 = ContentCreate(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                               kernel::StackArgument(ctx, 8));
}

// xam.xex ordinal 625, which FIFA Street's save library reaches through XexGetProcedureAddress
// (sub_83573848): XamContentCreateEx without the user -- the account is in the record instead. The
// record is an XCONTENT_DATA followed by the XUID (+0x138) and the title id (+0x140), so the leading
// 0x134 bytes read the same. Without it the game concludes it cannot make a save and reports
// "Unable to save Settings File".
// (rootName, contentDataEx, flags, dispositionOut, licenseOut, cacheSize, contentSize, overlapped)
void kernel::XamContentCreateTitle(PPCContext& ctx, uint8_t*)
{
    ctx.r3.u64 = ContentCreate(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r10.u32);
}

PPC_FUNC(__imp__XamContentClose)
{
    const std::string drive = GuestString(ctx.r3.u32, 32);
    if (!drive.empty()) UnmountDevice(drive);
    CompleteOverlapped(ctx.r4.u32, X_ERROR_SUCCESS);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

static uint32_t ContentDelete(uint32_t contentData, uint32_t overlapped)
{
    const std::string file = SafeName(GuestString(contentData + kContentFileName, kContentFileMax));
    uint32_t result = X_ERROR_FILE_NOT_FOUND;
    if (!file.empty())
    {
        std::error_code ec;
        const fs::path package = SaveRoot() / file;
        if (fs::is_directory(package, ec))
        {
            fs::remove_all(package, ec);
            result = ec ? X_ERROR_PATH_NOT_FOUND : X_ERROR_SUCCESS;
            // The player asked for this save to be gone, so the copy
            // RotatePackage kept goes with it -- leaving it would be a save
            // that is still on disk after being deleted.
            if (!result)
            {
                std::error_code also;
                fs::path backup = package;
                backup += ".bak";
                fs::remove_all(backup, also);
                LOGK("content: deleted %s", package.string().c_str());
            }
        }
    }
    if (result) LOGK("content: delete '%s' -> error %u", file.c_str(), result);
    CompleteOverlapped(overlapped, result);
    return result;
}

PPC_FUNC(__imp__XamContentDelete)
{
    // (user, contentData, overlapped)
    ctx.r3.u64 = ContentDelete(ctx.r4.u32, ctx.r5.u32);
}

// xam.xex ordinal 624, XamContentDeleteInternal in Xenia's export table: XamContentDelete without the
// user, for the record 625 creates (the XUID and title id follow the XCONTENT_DATA). FIFA Street's
// save library reaches it the way it reaches 625 (sub_83573990) after writing a save, and reports
// "Unable to save Settings File" when it is missing.
// (contentDataEx, overlapped)
void kernel::XamContentDeleteTitle(PPCContext& ctx, uint8_t*)
{
    ctx.r3.u64 = ContentDelete(ctx.r3.u32, ctx.r4.u32);
}


// The packages on the save device as XCONTENT_DATA records, for XamContentCreateEnumerator.
namespace kernel { std::vector<std::array<uint8_t, 0x134>> ListContent(uint32_t device, uint32_t type); }
std::vector<std::array<uint8_t, 0x134>> kernel::ListContent(uint32_t device, uint32_t type)
{
    std::vector<std::array<uint8_t, 0x134>> items;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(SaveRoot(), ec))
    {
        if (!entry.is_directory(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() > 41 || name.find('.') != std::string::npos) continue;     // backups
        std::array<uint8_t, 0x134> record{};
        bool have = false;
        if (std::FILE* in = std::fopen((entry.path() / ".xcontent").string().c_str(), "rb"))
        {
            have = std::fread(record.data(), 1, record.size(), in) == record.size();
            std::fclose(in);
        }
        if (!have)
        {
            *reinterpret_cast<be32*>(record.data() + kContentDeviceId) = kSaveDeviceId;
            *reinterpret_cast<be32*>(record.data() + kContentType) = 1;
            for (size_t i = 0; i < name.size(); i++)
            {
                record[8 + 2 * i + 1] = uint8_t(name[i]);               // display name, UTF-16BE
                record[kContentFileName + i] = uint8_t(name[i]);
            }
        }
        const uint32_t recordType = *reinterpret_cast<be32*>(record.data() + kContentType);
        if (type && recordType != type) continue;
        (void)device;
        items.push_back(record);
    }
    return items;
}
