// Temporary probes into the game: the save library's device lookup, and the team database load.
#include <ppc_recomp_shared.h>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <cstdlib>
#include "title.h"
#include "guest.h"
#include "log.h"
#include "crash.h"

#define PROBE(addr)                                                                                    \
    GUEST_HOOK(addr)                                                                                    \
    {                                                                                                   \
        static int told = 0;                                                                            \
        const bool show = told++ < 60;                                                                  \
        if (show) LOGI("probe: sub_" #addr "(r3 %08X r4 %08X r5 %08X) lr %08X '%s'", ctx.r3.u32,        \
                       ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr), crash::CurrentThreadName());           \
        GUEST_ORIG(addr)(ctx, base);                                                                    \
        if (show) LOGI("probe: sub_" #addr " -> %08X", ctx.r3.u32);                                     \
    }

// The game's device table lookup (sub_83387180): entries of 296 bytes with the device id at +4; a miss
// is -1, which the save poll (sub_83383DD8) turns into status 2, "your selected storage is no longer available".
GUEST_HOOK(83387180)
{
    const uint32_t table = ctx.r3.u32, key = ctx.r4.u32;
    const uint32_t begin = *GuestPtr<be32>(table), end = *GuestPtr<be32>(table + 4);
    static int told = 0;
    const bool show = told++ < 40;
    std::string ids;
    for (uint32_t at = begin; show && at && at != end && ids.size() < 200; at += 296)
    {
        char t[48];
        std::snprintf(t, sizeof t, " [id %08X +236 %08X]", uint32_t(*GuestPtr<be32>(at + 4)), uint32_t(*GuestPtr<be32>(at + 236)));
        ids += t;
    }
    GUEST_ORIG(83387180)(ctx, base);
    if (show)
        LOGI("probe: sub_83387180 device lookup key %08X in %u entries:%s -> %08X", key,
             begin ? (end - begin) / 296 : 0, ids.c_str(), ctx.r3.u32);
}

// The database loader (sub_826136F8) reads data/db/fifa_street_db.db and its -meta.xml through
// sub_82753EA8(pathString, sizeOut, allocator, 1, 16, 0), which returns the file in memory.
GUEST_HOOK(82753EA8)
{
    const uint32_t sizeOut = ctx.r4.u32;
    const uint32_t str = ctx.r3.u32;
    char name[96] = {};
    // An EA string: try the characters in place, then through a pointer at +0.
    const uint32_t chars = (*GuestPtr<uint8_t>(str) >= 0x20) ? str : uint32_t(*GuestPtr<be32>(str));
    for (int i = 0; chars && i < 95; i++) { const char c = *GuestPtr<char>(chars + i); if (!c) break; name[i] = c; }
    static int told = 0;
    const bool show = !std::strstr(name, "skillmoveai") && told++ < 80;
    GUEST_ORIG(82753EA8)(ctx, base);
    if (!show) return;
    const uint32_t got = ctx.r3.u32, size = sizeOut ? uint32_t(*GuestPtr<be32>(sizeOut)) : 0;
    char head[64] = {};
    for (int i = 0; got && i < 16; i++) std::snprintf(head + i * 3, 4, "%02X ", *GuestPtr<uint8_t>(got + i));
    LOGI("probe: file load '%s' -> %08X, %u bytes: %s", name, got, size, head);
}

// The allocator a thread allocates from (sub_8263C310): a table of (thread handle, allocator) at
// +20 with its count at +16, the default at +32, and the last answer cached at +24/+28. The thread
// handle is the current thread's, duplicated once and kept in a TLS slot (sub_826B5938). A null
// answer made the database load call through a null vtable (lr 8263EF44).
GUEST_HOOK(8263C310)
{
    const uint32_t self = ctx.r3.u32;
    GUEST_ORIG(8263C310)(ctx, base);
    static int told = 0;
    if (ctx.r3.u32 || told++ >= 12) return;
    std::string entries;
    const uint32_t count = *GuestPtr<be32>(self + 16), table = *GuestPtr<be32>(self + 20);
    for (uint32_t i = 0; table && i < count && i < 12; i++)
    {
        char t[32];
        std::snprintf(t, sizeof t, " [%08X %08X]", uint32_t(*GuestPtr<be32>(table + i * 8)), uint32_t(*GuestPtr<be32>(table + i * 8 + 4)));
        entries += t;
    }
    LOGI("probe: allocator lookup on %08X returned null on '%s': cached thread %08X, default %08X, %u entries:%s",
         self, crash::CurrentThreadName(), uint32_t(*GuestPtr<be32>(self + 24)), uint32_t(*GuestPtr<be32>(self + 32)),
         count, entries.c_str());
}

// The camera factory (sub_82950D98(manager, type)): finds or makes the camera of a type (0..6).
GUEST_HOOK(82950D98)
{
    const uint32_t type = ctx.r4.u32;
    GUEST_ORIG(82950D98)(ctx, base);
    static int told = 0;
    if (told++ < 60) LOGI("probe: camera factory type %u -> %08X on '%s'", type, ctx.r3.u32, crash::CurrentThreadName());
}

// Where CameraSystem was allocated, for the camera experiment below.
static std::atomic<uint32_t> g_cameraSystem{ 0 };

// The tagged allocator (sub_826C4EE0(size, poolName, name, ...)): the pool is found by the name's
// first three letters. The cameras' allocations came back null.
GUEST_HOOK(826C4EE0)
{
    const uint32_t size = ctx.r3.u32, pool = ctx.r4.u32, name = ctx.r5.u32;
    char poolName[24] = {}, objectName[40] = {};
    for (int i = 0; pool && i < 23; i++) { const char c = *GuestPtr<char>(pool + i); if (!c) break; poolName[i] = c; }
    for (int i = 0; name && i < 39; i++) { const char c = *GuestPtr<char>(name + i); if (!c || c < 32) break; objectName[i] = c; }
    GUEST_ORIG(826C4EE0)(ctx, base);
    static int failed = 0, told = 0;
    const bool camera = std::strstr(objectName, "amera") != nullptr;
    if (!std::strcmp(objectName, "CameraSystem")) g_cameraSystem = ctx.r3.u32;
    if ((!ctx.r3.u32 && failed++ < 40) || (camera && told++ < 400))
        LOGI("probe: alloc %u bytes from pool '%s' for '%s' -> %08X on '%s'", size, poolName, objectName,
             ctx.r3.u32, crash::CurrentThreadName());
}

// CameraTask's second virtual (vtable 82369968): counted, to see whether the gameplay camera runs.
GUEST_HOOK(829A9C40)
{
    static std::atomic<uint32_t> calls{ 0 };
    const uint32_t n = ++calls;
    if (n <= 3 || n % 150 == 0)
    {
        const uint32_t t = ctx.r3.u32;
        LOGI("probe: CameraTask update %u (this %08X): +3325 %u +3336 %d +3190 %u +2740 %08X +2744 %08X", n, t,
             *GuestPtr<uint8_t>(t + 3325), int32_t(uint32_t(*GuestPtr<be32>(t + 3336))), *GuestPtr<uint8_t>(t + 3190),
             uint32_t(*GuestPtr<be32>(t + 2740)), uint32_t(*GuestPtr<be32>(t + 2744)));
    }
    GUEST_ORIG(829A9C40)(ctx, base);
    // PES17_CAMERA_COPY=1 (experiment): the second camera record over the first, every update.
    static const bool copy = std::getenv("PES17_CAMERA_COPY") != nullptr;
    if (copy && g_cameraSystem)
    {
        uint8_t* system = GuestPtr<uint8_t>(g_cameraSystem.load());
        std::memmove(system + 0x30, system + 0x90, 0x60);
    }
}

// Presentation::PresSetGameCamera's type id (sub_82940750): who asks for it, and when.
GUEST_HOOK(82940750)
{
    const uint32_t lr = uint32_t(ctx.lr);
    GUEST_ORIG(82940750)(ctx, base);
    static std::mutex lock;
    static std::map<uint32_t, uint32_t> seen;
    std::lock_guard held(lock);
    const uint32_t n = ++seen[lr];
    if (n <= 2 || n % 1000 == 0)
        LOGI("probe: PresSetGameCamera id asked from %08X (time %u) on '%s'", lr, n, crash::CurrentThreadName());
}

// The other functions naming Presentation::PresSetGameCamera: does any of them run?
#define COUNT_PROBE(addr)                                                                               \
    GUEST_HOOK(addr)                                                                                    \
    {                                                                                                   \
        static std::atomic<uint32_t> calls{ 0 };                                                        \
        const uint32_t n = ++calls;                                                                     \
        if (n <= 3 || n % 1000 == 0)                                                                    \
            LOGI("probe: sub_" #addr " call %u (r3 %08X r4 %08X) lr %08X on '%s'", n, ctx.r3.u32,       \
                 ctx.r4.u32, uint32_t(ctx.lr), crash::CurrentThreadName());                             \
        GUEST_ORIG(addr)(ctx, base);                                                                    \
    }

// Event type-id getters: who asks for an event's id (subscribers once, senders when they send).
#define EVENT_ID_PROBE(addr, name)                                                                      \
    GUEST_HOOK(addr)                                                                                    \
    {                                                                                                   \
        const uint32_t lr = uint32_t(ctx.lr);                                                           \
        GUEST_ORIG(addr)(ctx, base);                                                                    \
        static std::mutex lock;                                                                         \
        static std::map<uint32_t, uint32_t> seen;                                                       \
        std::lock_guard held(lock);                                                                     \
        const uint32_t n = ++seen[lr];                                                                  \
        if (n <= 2 || n % 1000 == 0)                                                                    \
            LOGI("probe: " name " id asked from %08X (time %u) on '%s'", lr, n, crash::CurrentThreadName()); \
    }
EVENT_ID_PROBE(829406A0, "PresNISLoadedEvent")
EVENT_ID_PROBE(82940648, "PresSkipNISEvent")

// Global shader parameter registration (sub_82653800(lib, group, type, name, 1) -> handle): the
// handle of gCameraPos, to find what fills it every frame.
GUEST_HOOK(82653800)
{
    char name[48] = {};
    for (int i = 0; ctx.r6.u32 && i < 47; i++) { const char c = *GuestPtr<char>(ctx.r6.u32 + i); if (!c) break; name[i] = c; }
    const uint32_t type = ctx.r5.u32;
    GUEST_ORIG(82653800)(ctx, base);
    if (std::strstr(name, "amera") || std::strstr(name, "View") || std::strstr(name, "view"))
    {
        const uint32_t h = ctx.r3.u32;
        LOGI("probe: shader param '%s' type %u -> %08X [%08X %08X %08X %08X]", name, type, h,
             h ? uint32_t(*GuestPtr<be32>(h)) : 0, h ? uint32_t(*GuestPtr<be32>(h + 4)) : 0,
             h ? uint32_t(*GuestPtr<be32>(h + 8)) : 0, h ? uint32_t(*GuestPtr<be32>(h + 12)) : 0);
    }
}
EVENT_ID_PROBE(828C4E88, "NISSequenceComplete")
EVENT_ID_PROBE(828CAB70, "NISLoadedEvent")
