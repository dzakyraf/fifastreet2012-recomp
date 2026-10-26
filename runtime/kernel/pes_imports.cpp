// Kernel and XAM imports PES 2017 reaches that MW2 never did. Argument order
// and results follow the console's ABI (r3-r10, then 8-byte stack slots from
// r1+0x54) as documented by Xenia's kernel; each was found missing by the
// "unimplemented:" lines a device run logs.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "modules.h"
#include "objects.h"
#include "yield_backoff.h"
#include "../guest.h"
#include "../log.h"
#include "../crash.h"
#include "../platform.h"
#ifdef PES17_ANDROID
#include "../android/android.h"
#endif
#include <chrono>
#include <condition_variable>
#include <memory>
#include <thread>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sched.h>
#include <string>
#include <vector>
#include "trace.h"

using namespace kernel;

namespace
{
    constexpr uint32_t X_ERROR_CANCELLED   = 0x000004C7;
    constexpr uint32_t X_ERROR_IO_INCOMPLETE = 0x000003E4;
    constexpr uint32_t X_ERROR_EMPTY       = 0x000010D2;
    constexpr uint32_t X_STATUS_NO_MORE_FILES = 0x80000006;
    constexpr uint32_t X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;

    uint32_t Load32(uint32_t a) { return *GuestPtr<be32>(a); }
    void Store32(uint32_t a, uint32_t v) { *GuestPtr<be32>(a) = v; }
    uint16_t Load16(uint32_t a) { const uint8_t* p = guest::Base() + a; return uint16_t(p[0] << 8 | p[1]); }
    void Store16(uint32_t a, uint16_t v) { uint8_t* p = guest::Base() + a; p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
    uint64_t Load64(uint32_t a) { return uint64_t(Load32(a)) << 32 | Load32(a + 4); }

    std::string GuestString(uint32_t a, size_t max = 4096)
    {
        std::string s;
        if (!a) return s;
        const char* p = reinterpret_cast<const char*>(guest::Base() + a);
        while (s.size() < max && p[s.size()]) s.push_back(p[s.size()]);
        return s;
    }

    std::string GuestWideString(uint32_t a, size_t max = 4096)
    {
        std::string s;
        if (!a) return s;
        for (size_t i = 0; i < max; i++)
        {
            const uint16_t c = Load16(a + uint32_t(i * 2));
            if (!c) break;
            s.push_back(c < 0x80 ? char(c) : '?');
        }
        return s;
    }

    // ---- the XEX header, which the title reads fields of --------------------
    // The executable's, as kernel::modules mapped it (modules.cpp).
    uint32_t XexHeader() { return modules::Header(modules::kExecutable); }

    // An optional header's value (key & 0xFF == 0) or its guest address.
    uint32_t XexField(uint32_t key) { return modules::Field(modules::kExecutable, key); }

    // ---- printf, which the console's kernel exports ---------------------------
    // Arguments come one 8-byte slot at a time: r3-r10 first, then the caller's
    // stack, or a va_list, which here is a pointer to those slots. A double is
    // passed as the bits of its slot, as the console's variadic calls do.
    struct Args
    {
        const PPCContext* ctx = nullptr;
        uint32_t next = 0;          // index of the next argument in the full list
        uint32_t valist = 0;        // when non-zero, slots are read from here

        uint64_t Slot()
        {
            if (valist) { const uint64_t v = Load64(valist); valist += 8; return v; }
            const uint32_t n = next++;
            // PPCContext does not keep r3-r10 adjacent (r3 comes first), so name them.
            switch (n)
            {
            case 0: return ctx->r3.u64;  case 1: return ctx->r4.u64;
            case 2: return ctx->r5.u64;  case 3: return ctx->r6.u64;
            case 4: return ctx->r7.u64;  case 5: return ctx->r8.u64;
            case 6: return ctx->r9.u64;  case 7: return ctx->r10.u64;
            }
            return Load64(ctx->r1.u32 + 0x14 + n * 8);
        }
    };

    std::string Format(const char* fmt, Args& args)
    {
        std::string out;
        for (const char* p = fmt; *p; p++)
        {
            if (*p != '%') { out.push_back(*p); continue; }
            const char* start = p++;
            if (*p == '%') { out.push_back('%'); continue; }
            std::string spec = "%";
            while (*p && std::strchr("-+ #0", *p)) spec.push_back(*p++);
            if (*p == '*') { spec += std::to_string(int32_t(args.Slot())); p++; }
            else while (std::isdigit(uint8_t(*p))) spec.push_back(*p++);
            if (*p == '.')
            {
                spec.push_back(*p++);
                if (*p == '*') { spec += std::to_string(int32_t(args.Slot())); p++; }
                else while (std::isdigit(uint8_t(*p))) spec.push_back(*p++);
            }
            bool wide64 = false, wideChar = false, half = false;
            for (;;)
            {
                if (*p == 'l' && p[1] == 'l') { wide64 = true; p += 2; }
                else if (*p == 'I' && p[1] == '6' && p[2] == '4') { wide64 = true; p += 3; }
                else if (*p == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
                else if (*p == 'l' || *p == 'w') { wideChar = true; p++; }
                else if (*p == 'h') { half = true; p++; }
                else if (*p == 'z' || *p == 't' || *p == 'j' || *p == 'L') { p++; }
                else break;
            }
            const char conv = *p;
            if (!conv) { out.append(start); break; }
            char buffer[512];
            switch (conv)
            {
            case 'd': case 'i':
            {
                const uint64_t v = args.Slot();
                const long long s = wide64 ? (long long)int64_t(v) : half ? (long long)int16_t(v) : (long long)int32_t(v);
                std::snprintf(buffer, sizeof buffer, (spec + "lld").c_str(), s);
                out += buffer; break;
            }
            case 'u': case 'x': case 'X': case 'o':
            {
                const uint64_t v = args.Slot();
                const unsigned long long u = wide64 ? v : half ? uint16_t(v) : uint32_t(v);
                std::snprintf(buffer, sizeof buffer, (spec + "ll" + conv).c_str(), u);
                out += buffer; break;
            }
            case 'p':
                std::snprintf(buffer, sizeof buffer, "%08X", uint32_t(args.Slot()));
                out += buffer; break;
            case 'c': case 'C':
                std::snprintf(buffer, sizeof buffer, (spec + "c").c_str(), int(uint8_t(args.Slot())));
                out += buffer; break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            {
                const uint64_t bits = args.Slot();
                double d; std::memcpy(&d, &bits, 8);
                std::snprintf(buffer, sizeof buffer, (spec + conv).c_str(), d);
                out += buffer; break;
            }
            case 's': case 'S':
            {
                const uint32_t a = uint32_t(args.Slot());
                // MSVC: %s is narrow, %S/%ls wide.
                const std::string s = !a ? std::string("(null)")
                                         : (conv == 'S' || wideChar) ? GuestWideString(a) : GuestString(a);
                std::snprintf(buffer, sizeof buffer, (spec + "s").c_str(), s.c_str());
                out += buffer; break;
            }
            case 'n': args.Slot(); break;
            default: out.append(start, p + 1); break;
            }
        }
        return out;
    }

    // MSVC _snprintf: no terminator and -1 when it does not fit.
    int32_t WriteBounded(uint32_t dest, uint32_t count, const std::string& s, bool bounded)
    {
        char* d = reinterpret_cast<char*>(guest::Base() + dest);
        if (!bounded) { std::memcpy(d, s.c_str(), s.size() + 1); return int32_t(s.size()); }
        if (s.size() < count) { std::memcpy(d, s.c_str(), s.size() + 1); return int32_t(s.size()); }
        if (count) std::memcpy(d, s.data(), count);
        return -1;
    }

    // ---- time ------------------------------------------------------------------
    constexpr int64_t kEpochDelta100ns = 116444736000000000LL;   // 1601 -> 1970
}

// ---- semaphores and the combined signal-and-wait -----------------------------

PPC_FUNC(__imp__NtCreateSemaphore)
{
    // (Handle*, ObjectAttributes, InitialCount, MaximumCount)
    auto semaphore = std::make_shared<Semaphore>();
    semaphore->count = int32_t(ctx.r5.u32);
    semaphore->limit = int32_t(ctx.r6.u32) > 0 ? int32_t(ctx.r6.u32) : 0x7FFFFFFF;
    const uint32_t handle = InsertHandle(semaphore, crash::CurrentThreadName(), uint32_t(ctx.lr));
    if (auto* out = GuestPtr<be32>(ctx.r3.u32)) *out = handle;
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtReleaseSemaphore)
{
    // (Handle, ReleaseCount, PreviousCount*)
    auto semaphore = LookupHandleAs<Semaphore>(ctx.r3.u32);
    if (!semaphore) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    const int32_t delta = int32_t(ctx.r4.u32);
    {
        std::lock_guard held(DispatcherLock());
        if (int64_t(semaphore->count) + delta > semaphore->limit)
        { ctx.r3.u64 = X_STATUS_SEMAPHORE_LIMIT_EXCEEDED; return; }
    }
    const int32_t previous = semaphore->Release(delta, uint32_t(ctx.lr));
    if (auto* out = GuestPtr<be32>(ctx.r5.u32)) *out = uint32_t(previous);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__NtSignalAndWaitForSingleObjectEx)
{
    // (SignalHandle, WaitHandle, Alertable, Timeout*): signal one object, then
    // wait on the other, as one call.
    auto signal = LookupHandle(ctx.r3.u32);
    if (!signal) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    if (auto* event = dynamic_cast<Event*>(signal.get())) event->Set(crash::CurrentThreadName(), uint32_t(ctx.lr));
    else if (auto* semaphore = dynamic_cast<Semaphore*>(signal.get())) semaphore->Release(1);
    else if (auto* mutant = dynamic_cast<Mutant*>(signal.get())) mutant->Release();
    else { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }

    auto waitOn = LookupHandle(ctx.r4.u32);
    auto* dispatcher = dynamic_cast<Dispatcher*>(waitOn.get());
    if (!dispatcher) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    int64_t storage;
    const int64_t* timeout = nullptr;
    if (ctx.r6.u32) { storage = int64_t(Load64(ctx.r6.u32)); timeout = &storage; }
    Dispatcher* list[1] = { dispatcher };
    ctx.r3.u64 = Wait(list, 1, false, timeout);
}

// CAPTURE during a hang: the threads that spin in a yield loop say, once each, where
// in the guest they are (the caller chain from r1), so a busy-wait can be named.
static std::atomic<int> g_yieldStacks{ 0 };
void kernel::DumpYieldStacks(int perThread) { g_yieldStacks.store(perThread, std::memory_order_relaxed); }

PPC_FUNC(__imp__NtYieldExecution)
{
    if (g_yieldStacks.load(std::memory_order_relaxed) > 0)
    {
        thread_local int shown = 0;
        thread_local int generation = 0;
        const int now = g_yieldStacks.load(std::memory_order_relaxed);
        if (generation != now) { generation = now; shown = 0; }
        if (shown++ < 3)
        {
            char line[300]; int at = std::snprintf(line, sizeof line, "%08X", uint32_t(ctx.lr));
            uint32_t frame = ctx.r1.u32;
            for (int i = 0; i < 10 && frame; i++)
            {
                auto* ptr = GuestPtr<be32>(frame);
                if (!ptr) break;
                const uint32_t next = uint32_t(*ptr);
                if (!next) break;
                at += std::snprintf(line + at, sizeof line - at, " <- %08X", uint32_t(*GuestPtr<be32>(next - 8)));
                frame = next;
            }
            LOGW("spin: %s (r3 %08X r4 %08X r5 %08X) yields from %s", crash::CurrentThreadName(),
                 ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, line);
        }
    }
    kernel::YieldBackoff(); ctx.r3.u64 = X_STATUS_SUCCESS;
}

// ---- DPCs: queued by interrupt handlers, run here straight away ----------------

PPC_FUNC(__imp__KeInitializeDpc)
{
    // (Dpc, DeferredRoutine, DeferredContext) into the KDPC: type 19 at +0,
    // routine at +0x0C, context at +0x10.
    const uint32_t dpc = ctx.r3.u32;
    std::memset(guest::Base() + dpc, 0, 0x1C);
    Store16(dpc, 19);
    Store32(dpc + 0x0C, ctx.r4.u32);
    Store32(dpc + 0x10, ctx.r5.u32);
}

PPC_FUNC(__imp__KeInsertQueueDpc)
{
    // (Dpc, SystemArgument1, SystemArgument2). The routine runs on the calling
    // thread -- usually the runtime's interrupt thread, which is where the
    // console would run it once the interrupt returns.
    const uint32_t dpc = ctx.r3.u32;
    const uint32_t routine = Load32(dpc + 0x0C);
    Store32(dpc + 0x14, ctx.r4.u32);
    Store32(dpc + 0x18, ctx.r5.u32);
    if (routine) CallGuest(ctx, routine, dpc, Load32(dpc + 0x10), ctx.r4.u32, ctx.r5.u32);
    ctx.r3.u64 = 1;
}

// ---- exceptions ----------------------------------------------------------------

PPC_FUNC(__imp__RtlRaiseException)
{
    // EXCEPTION_RECORD: code +0, flags +4, NumberParameters +0x10, information
    // from +0x14. 0x406D1388 is MSVC's SetThreadName, which a debugger catches;
    // with none attached it is meant to be ignored.
    const uint32_t record = ctx.r3.u32;
    const uint32_t code = record ? Load32(record) : 0;
    if (code == 0x406D1388)
    {
        const uint32_t name = Load32(record + 0x14 + 4);
        LOGK("thread name: '%s' is %s", GuestString(name, 64).c_str(), crash::CurrentThreadName());
        return;
    }
    LOGW("RtlRaiseException: code %08X flags %08X at %08X (called from %08X by '%s') -- ignored",
         code, record ? Load32(record + 4) : 0, record ? Load32(record + 0x0C) : 0,
         uint32_t(ctx.lr), crash::CurrentThreadName());
}

PPC_FUNC(__imp__RtlCaptureContext) { (void)ctx; }

// ---- strings --------------------------------------------------------------------

PPC_FUNC(__imp___snprintf)
{
    Args args{ &ctx, 3 };
    const std::string s = Format(GuestString(ctx.r5.u32).c_str(), args);
    ctx.r3.u64 = uint64_t(int64_t(WriteBounded(ctx.r3.u32, ctx.r4.u32, s, true)));
}

PPC_FUNC(__imp__sprintf)
{
    Args args{ &ctx, 2 };
    const std::string s = Format(GuestString(ctx.r4.u32).c_str(), args);
    ctx.r3.u64 = uint64_t(int64_t(WriteBounded(ctx.r3.u32, 0, s, false)));
}

PPC_FUNC(__imp___vsnprintf)
{
    Args args{ &ctx, 0, ctx.r6.u32 };
    const std::string s = Format(GuestString(ctx.r5.u32).c_str(), args);
    ctx.r3.u64 = uint64_t(int64_t(WriteBounded(ctx.r3.u32, ctx.r4.u32, s, true)));
}

PPC_FUNC(__imp__RtlInitUnicodeString)
{
    // UNICODE_STRING { u16 Length, u16 MaximumLength, u32 Buffer }
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint32_t chars = 0;
    if (src) while (Load16(src + chars * 2)) chars++;
    Store16(dst, uint16_t(chars * 2));
    Store16(dst + 2, uint16_t(src ? chars * 2 + 2 : 0));
    Store32(dst + 4, src);
}

PPC_FUNC(__imp__RtlUnicodeStringToAnsiString)
{
    // (AnsiString*, UnicodeString*, AllocateDestination)
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    const uint16_t bytes = Load16(src);
    const uint32_t buffer = Load32(src + 4);
    const uint32_t chars = bytes / 2;
    uint32_t out = Load32(dst + 4);
    if (ctx.r5.u32) { out = AllocateGuest(chars + 1, 4); Store32(dst + 4, out); Store16(dst + 2, uint16_t(chars + 1)); }
    if (!out) { ctx.r3.u64 = X_STATUS_NO_MEMORY; return; }
    const uint32_t room = ctx.r5.u32 ? chars + 1 : Load16(dst + 2);
    const uint32_t n = room ? std::min(chars, room - 1) : 0;
    for (uint32_t i = 0; i < n; i++)
    {
        const uint16_t c = Load16(buffer + i * 2);
        guest::Base()[out + i] = c < 0x80 ? uint8_t(c) : uint8_t('?');
    }
    if (room) guest::Base()[out + n] = 0;
    Store16(dst, uint16_t(n));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__RtlFreeAnsiString)
{
    if (const uint32_t buffer = Load32(ctx.r3.u32 + 4)) FreeGuest(buffer);
    Store32(ctx.r3.u32 + 4, 0);
    Store16(ctx.r3.u32, 0);
    Store16(ctx.r3.u32 + 2, 0);
}

PPC_FUNC(__imp__RtlUpcaseUnicodeChar)
{
    const uint16_t c = uint16_t(ctx.r3.u32);
    ctx.r3.u64 = (c >= 'a' && c <= 'z') ? c - 32 : c;
}

PPC_FUNC(__imp__RtlCompareStringN)
{
    // (String1, Length1, String2, Length2, CaseInSensitive): narrow strings.
    const uint32_t a = ctx.r3.u32, b = ctx.r5.u32;
    const uint32_t la = ctx.r4.u32, lb = ctx.r6.u32;
    const bool fold = ctx.r7.u32 != 0;
    const uint32_t n = std::min(la, lb);
    for (uint32_t i = 0; i < n; i++)
    {
        int ca = guest::Base()[a + i], cb = guest::Base()[b + i];
        if (fold) { ca = std::toupper(ca); cb = std::toupper(cb); }
        if (ca != cb) { ctx.r3.u64 = uint64_t(int64_t(ca - cb)); return; }
    }
    ctx.r3.u64 = uint64_t(int64_t(int32_t(la) - int32_t(lb)));
}

// ---- time fields ----------------------------------------------------------------

PPC_FUNC(__imp__RtlTimeToTimeFields)
{
    // (Time* 100ns since 1601, TIME_FIELDS*): Year, Month, Day, Hour, Minute,
    // Second, Milliseconds, Weekday as big-endian u16.
    const int64_t t = int64_t(Load64(ctx.r3.u32));
    const int64_t ms = (t - kEpochDelta100ns) / 10000;
    std::time_t seconds = std::time_t(ms / 1000);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    const uint32_t f = ctx.r4.u32;
    Store16(f + 0, uint16_t(tm.tm_year + 1900));
    Store16(f + 2, uint16_t(tm.tm_mon + 1));
    Store16(f + 4, uint16_t(tm.tm_mday));
    Store16(f + 6, uint16_t(tm.tm_hour));
    Store16(f + 8, uint16_t(tm.tm_min));
    Store16(f + 10, uint16_t(tm.tm_sec));
    Store16(f + 12, uint16_t(((ms % 1000) + 1000) % 1000));
    Store16(f + 14, uint16_t(tm.tm_wday));
}

PPC_FUNC(__imp__RtlTimeFieldsToTime)
{
    // (TIME_FIELDS*, Time*): returns TRUE.
    const uint32_t f = ctx.r3.u32;
    std::tm tm{};
    tm.tm_year = Load16(f) - 1900; tm.tm_mon = Load16(f + 2) - 1; tm.tm_mday = Load16(f + 4);
    tm.tm_hour = Load16(f + 6); tm.tm_min = Load16(f + 8); tm.tm_sec = Load16(f + 10);
    const int64_t seconds = int64_t(timegm(&tm));
    const int64_t t = seconds * 10000000LL + int64_t(Load16(f + 12)) * 10000LL + kEpochDelta100ns;
    *GuestPtr<be32>(ctx.r4.u32) = uint32_t(uint64_t(t) >> 32);
    *GuestPtr<be32>(ctx.r4.u32 + 4) = uint32_t(t);
    ctx.r3.u64 = 1;
}

// ---- memory and lists -------------------------------------------------------------

PPC_FUNC(__imp__ExAllocatePool) { ctx.r3.u64 = AllocateGuest(ctx.r3.u32, ctx.r3.u32 >= 4096 ? 4096 : 16); }
PPC_FUNC(__imp__ExAllocatePoolTypeWithTag) { ctx.r3.u64 = AllocateGuest(ctx.r3.u32, ctx.r3.u32 >= 4096 ? 4096 : 16); }
PPC_FUNC(__imp__ExFreePool) { FreeGuest(ctx.r3.u32); }

PPC_FUNC(__imp__InterlockedPopEntrySList)
{
    // SLIST_HEADER { u32 Next, u16 Depth, u16 Sequence }, pushed by the title's
    // own lwarx/stwcx. code: one 64-bit compare-and-swap keeps the two agreed.
    auto* header = reinterpret_cast<std::atomic<uint64_t>*>(guest::Base() + ctx.r3.u32);
    uint64_t old = header->load(std::memory_order_acquire);
    for (;;)
    {
        const uint64_t value = __builtin_bswap64(old);
        const uint32_t entry = uint32_t(value >> 32);
        if (!entry) { ctx.r3.u64 = 0; return; }
        const uint32_t next = Load32(entry);
        const uint16_t depth = uint16_t(value >> 16), sequence = uint16_t(value);
        const uint64_t updated = uint64_t(next) << 32 | uint64_t(uint16_t(depth - 1)) << 16 | uint16_t(sequence + 1);
        if (header->compare_exchange_weak(old, __builtin_bswap64(updated), std::memory_order_acq_rel))
        { ctx.r3.u64 = entry; return; }
    }
}

PPC_FUNC(__imp__ObReferenceObject) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__ObIsTitleObject) { ctx.r3.u64 = 1; }

// ---- threads --------------------------------------------------------------------

PPC_FUNC(__imp__KeQueryBasePriorityThread) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeSetDisableBoostThread)   { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeSetCurrentProcessType)   { }

// ---- the executable's own header -------------------------------------------------

PPC_FUNC(__imp__RtlImageXexHeaderField)
{
    // (XexHeaderBase, Key): whichever module's header the title passes -- the
    // DLL reads its own -- falling back to the executable's.
    for (int i = 0; i < 2; i++)
        if (modules::Loaded(i) && modules::Header(i) == ctx.r3.u32)
        { ctx.r3.u64 = modules::Field(i, ctx.r4.u32); return; }
    ctx.r3.u64 = XexField(ctx.r4.u32);
}

PPC_FUNC(__imp__XamGetExecutionId)
{
    // (Info**): a block the title reads its title id, version and disc from.
    static uint32_t block = 0;
    if (!block)
    {
        block = AllocateGuest(24, 8);
        if (const uint32_t info = XexField(0x00040006))
            std::memcpy(guest::Base() + block, guest::Base() + info, 24);
    }
    Store32(ctx.r3.u32, block);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// ---- language, region and UI -------------------------------------------------------

PPC_FUNC(__imp__XamGetLanguage)  { ctx.r3.u64 = 1; }        // English, as XGetLanguage
PPC_FUNC(__imp__XamGetLocaleEx)  { ctx.r3.u64 = 6; }        // Great Britain: the European disc
PPC_FUNC(__imp__XGetGameRegion)  { ctx.r3.u64 = 0x02FE; }   // PAL: the region of this disc
PPC_FUNC(__imp__XamGetOverlappedResult)
{
    // (Overlapped*, Result*, Wait): InternalLow is the result, InternalHigh the length.
    const uint32_t overlapped = ctx.r3.u32;
    const uint32_t result = Load32(overlapped);
    if (ctx.r4.u32) Store32(ctx.r4.u32, Load32(overlapped + 4));
    ctx.r3.u64 = result == X_ERROR_IO_PENDING ? X_ERROR_IO_INCOMPLETE : result;
}

PPC_FUNC(__imp__XamShowMessageBoxUI)
{
    // (User, Title, Text, ButtonCount, Buttons, FocusButton, Flags, Result*,
    // Overlapped): nobody can answer, so the first button is pressed -- and the
    // text is logged, because a title says what is wrong in these.
    LOGW("message box: \"%s\": \"%s\"", GuestWideString(ctx.r4.u32, 256).c_str(),
         GuestWideString(ctx.r5.u32, 1024).c_str());
    if (ctx.r10.u32) Store32(ctx.r10.u32, 0);
    const uint32_t overlapped = uint32_t(Load64(ctx.r1.u32 + 0x14 + 8 * 8));
    if (overlapped) { CompleteOverlapped(overlapped, X_ERROR_SUCCESS); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = X_ERROR_SUCCESS;
}

namespace
{
    // The one text dialog the phone shows, and the call it answers. The title may ask again while it is
    // open; the newest call takes over the answer and the older one is cancelled, so the answer never
    // goes to a request the title has already given up on.
    constexpr uint32_t kXnSysUi = 0x00000009;      // XN_SYS_UI: a system dialog opened (1) or closed (0)
    struct KeyboardRequest { uint32_t buffer = 0, length = 0, overlapped = 0; std::shared_ptr<void> waiter; };
    std::mutex g_keyboardLock;
    bool g_keyboardOpen = false;
    KeyboardRequest g_keyboard;
    struct KeyboardWait { std::mutex m; std::condition_variable cv; bool done = false, ok = false; };

    void Utf8ToGuestUtf16(const std::string& utf8, uint32_t buffer, uint32_t length)
    {
        std::vector<uint16_t> out;
        for (size_t i = 0; i < utf8.size();)
        {
            const uint8_t c = uint8_t(utf8[i]);
            uint32_t cp = c; size_t n = 1;
            if (c >= 0xF0 && i + 3 < utf8.size()) { cp = ((c & 7) << 18) | ((utf8[i+1] & 0x3F) << 12) | ((utf8[i+2] & 0x3F) << 6) | (utf8[i+3] & 0x3F); n = 4; }
            else if (c >= 0xE0 && i + 2 < utf8.size()) { cp = ((c & 15) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F); n = 3; }
            else if (c >= 0xC0 && i + 1 < utf8.size()) { cp = ((c & 31) << 6) | (utf8[i+1] & 0x3F); n = 2; }
            i += n;
            if (cp >= 0x10000) { cp -= 0x10000; out.push_back(uint16_t(0xD800 + (cp >> 10))); out.push_back(uint16_t(0xDC00 + (cp & 0x3FF))); }
            else out.push_back(uint16_t(cp));
        }
        if (out.size() > length - 1) out.resize(length - 1);
        for (size_t i = 0; i < out.size(); i++) Store16(buffer + uint32_t(i * 2), out[i]);
        Store16(buffer + uint32_t(out.size() * 2), 0);
    }

    void Answer(const KeyboardRequest& request, bool ok, const std::string& utf8, bool dialogClosed = true)
    {
        if (ok && request.buffer && request.length) Utf8ToGuestUtf16(utf8, request.buffer, request.length);
        LOGW("keyboard: %s \"%s\" -> buffer %08X, overlapped %08X", ok ? "answered" : "cancelled", utf8.c_str(),
             request.buffer, request.overlapped);
        if (request.overlapped)
        {
            if (ok) CompleteOverlapped(request.overlapped, X_ERROR_SUCCESS);
            else CompleteOverlappedEx(request.overlapped, X_ERROR_SUCCESS, X_ERROR_CANCELLED, 0);   // as Xenia: the call succeeded, the user cancelled
        }
        if (auto wait = std::static_pointer_cast<KeyboardWait>(request.waiter))
        {
            std::lock_guard lock(wait->m);
            wait->ok = ok; wait->done = true; wait->cv.notify_all();
        }
        // The title only takes the answer while it believes the system UI is still up (the handler of its
        // completion checks the flags XN_SYS_UI sets), so "closed" follows the completion, a moment later.
        if (dialogClosed)
            std::thread([] { std::this_thread::sleep_for(std::chrono::milliseconds(500)); PostNotification(kXnSysUi, 0); }).detach();
    }
}

PPC_FUNC(__imp__XamShowKeyboardUI)
{
    // (User, Flags, Default, Title, Description, Buffer, Length, Overlapped). On the phone the app shows a
    // text dialog; the answer is written into Buffer as big-endian UTF-16 and the overlapped completed.
    const uint32_t defaultText = ctx.r5.u32, title = ctx.r6.u32, description = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32, overlapped = ctx.r10.u32;
#ifdef PES17_ANDROID
    KeyboardRequest request{ buffer, length, overlapped, nullptr };
    std::shared_ptr<KeyboardWait> wait;
    if (!overlapped) { wait = std::make_shared<KeyboardWait>(); request.waiter = wait; }
    LOGW("keyboard: from %08X, user %u flags %08X title \"%s\" description \"%s\" default \"%s\" buffer %08X (%u characters) overlapped %08X",
         uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, GuestWideString(title, 128).c_str(), GuestWideString(description, 256).c_str(),
         GuestWideString(defaultText, 128).c_str(), buffer, length, overlapped);
    // An asynchronous call marks its overlapped as running; the title polls it, and a result of zero reads
    // as "finished with nothing" -- which is how the first answer was lost.
    if (overlapped)
    {
        Store32(overlapped, X_ERROR_IO_PENDING);       // InternalLow
        Store32(overlapped + 4, 0);                    // InternalHigh
        Store32(overlapped + 24, 0);                   // dwExtendedError
    }
    bool open;
    KeyboardRequest older;
    {
        std::lock_guard lock(g_keyboardLock);
        open = g_keyboardOpen;
        if (open) { older = g_keyboard; g_keyboard = request; }
        else { g_keyboardOpen = true; g_keyboard = request; }
    }
    if (open) Answer(older, false, "", false);      // the title asked again: the old request is dropped
    else
    {
        PostNotification(kXnSysUi, 1);
        android::ui::ShowKeyboard(GuestWideString(title, 128), GuestWideString(description, 256),
                                  GuestWideString(defaultText, 128), length ? length - 1 : 32,
                                  [](bool ok, const std::string& text) {
            KeyboardRequest current;
            { std::lock_guard lock(g_keyboardLock); current = g_keyboard; g_keyboard = {}; g_keyboardOpen = false; }
            Answer(current, ok, text);
        });
    }
    if (overlapped) { ctx.r3.u64 = X_ERROR_IO_PENDING; return; }
    std::unique_lock lock(wait->m);
    wait->cv.wait(lock, [&] { return wait->done; });
    ctx.r3.u64 = wait->ok ? X_ERROR_SUCCESS : X_ERROR_CANCELLED;
#else
    (void)defaultText; (void)title; (void)description; (void)buffer; (void)length;
    if (overlapped) { CompleteOverlappedEx(overlapped, X_ERROR_CANCELLED, X_ERROR_CANCELLED, 0); ctx.r3.u64 = X_ERROR_IO_PENDING; }
    else ctx.r3.u64 = X_ERROR_CANCELLED;
#endif
}

// XamInputGetKeystrokeEx is in keystroke.cpp.

PPC_FUNC(__imp__XAudioGetSpeakerConfig)
{
    if (ctx.r3.u32) Store32(ctx.r3.u32, 0x00010001);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

PPC_FUNC(__imp__VdSetDisplayModeOverride) { ctx.r3.u64 = X_STATUS_SUCCESS; }

PPC_FUNC(__imp__NtQueryDirectoryFile)
{
    // Nothing in the game folder needs listing to boot; say so cleanly instead of
    // "success" over an unwritten buffer.
    LOGW("NtQueryDirectoryFile: handle %08X, pattern '%s' -- reported empty",
         ctx.r3.u32, ctx.r10.u32 ? GuestString(Load32(ctx.r10.u32 + 4), 128).c_str() : "");
    ctx.r3.u64 = X_STATUS_NO_MORE_FILES;
}

// ---- hashing ----------------------------------------------------------------------

namespace
{
    struct Sha1
    {
        uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
        uint8_t block[64]; uint32_t used = 0; uint64_t length = 0;
        static uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
        void Compress()
        {
            uint32_t w[80];
            for (int i = 0; i < 16; i++) w[i] = uint32_t(block[i*4]) << 24 | block[i*4+1] << 16 | block[i*4+2] << 8 | block[i*4+3];
            for (int i = 16; i < 80; i++) w[i] = Rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; i++)
            {
                uint32_t f, k;
                if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6; }
                const uint32_t t = Rol(a, 5) + f + e + k + w[i];
                e = d; d = c; c = Rol(b, 30); b = a; a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }
        void Update(const uint8_t* p, size_t n)
        {
            length += n;
            while (n--) { block[used++] = *p++; if (used == 64) { Compress(); used = 0; } }
        }
        void Final(uint8_t out[20])
        {
            const uint64_t bits = length * 8;
            const uint8_t pad = 0x80, zero = 0;
            Update(&pad, 1);
            while (used != 56) Update(&zero, 1);
            for (int i = 7; i >= 0; i--) { const uint8_t b = uint8_t(bits >> (i * 8)); Update(&b, 1); }
            for (int i = 0; i < 20; i++) out[i] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
        }
    };
}

PPC_FUNC(__imp__XeCryptSha)
{
    // (Input1, Size1, Input2, Size2, Input3, Size3, Output, OutputSize)
    Sha1 sha;
    const uint32_t pairs[3][2] = { { ctx.r3.u32, ctx.r4.u32 }, { ctx.r5.u32, ctx.r6.u32 }, { ctx.r7.u32, ctx.r8.u32 } };
    for (const auto& p : pairs) if (p[0] && p[1]) sha.Update(guest::Base() + p[0], p[1]);
    uint8_t digest[20];
    sha.Final(digest);
    if (ctx.r9.u32) std::memcpy(guest::Base() + ctx.r9.u32, digest, std::min<uint32_t>(ctx.r10.u32, 20));
}

PPC_FUNC(__imp__XNetLogonGetMachineID)
{
    if (ctx.r3.u32) { Store32(ctx.r3.u32, 0xFA000000); Store32(ctx.r3.u32 + 4, 0x00000001); }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// ---- data imports ------------------------------------------------------------------

void kernel::ResolveDataImports(uint32_t h, uint32_t headerSize)
{
    // XEX import libraries (optional header 0x000103FF): a string table, then per
    // library { size, digest[20], id, version, min_version, name u16, count u16,
    // record addresses[count] }. A record holds (type << 24 | ordinal) until it is
    // resolved: type 0 is a data slot, type 1 a function's thunk. An ordinal with
    // a thunk is a function, and its data slot is the import table entry.
    // Every module's own header (modules.cpp calls this for each one it maps).
    uint32_t imports = 0;
    for (uint32_t i = 0, count = Load32(h + 0x14); i < count && 0x18 + i * 8 + 8 <= headerSize; i++)
        if (Load32(h + 0x18 + i * 8) == 0x000103FF) imports = h + Load32(h + 0x1C + i * 8);
    if (!h || !imports) { LOGW("imports: no import table in the XEX header at %08X", h); return; }

    struct Known { uint32_t lib; uint16_t ordinal; const char* name; };
    const uint32_t stringsSize = Load32(imports + 4), libraries = Load32(imports + 8);
    uint32_t at = imports + 12 + stringsSize;
    uint32_t variables = 0, slots = 0;
    for (uint32_t lib = 0; lib < libraries; lib++)
    {
        const uint32_t size = Load32(at);
        const uint16_t count = Load16(at + 0x26);
        std::vector<uint32_t> records(count);
        for (uint16_t i = 0; i < count; i++) records[i] = Load32(at + 0x28 + i * 4);

        std::vector<uint32_t> thunkOf(0x10000, 0);
        for (uint32_t r : records)
        {
            const uint32_t v = Load32(r);
            if ((v >> 24) == 1) thunkOf[v & 0xFFFF] = r;
        }
        for (uint32_t r : records)
        {
            const uint32_t v = Load32(r);
            // (type << 24 | library << 16 | ordinal): anything else is resolved already,
            // and type 1 is a thunk. The library byte is this library's index -- xam's
            // slots (library 0) were skipped when this read "== 1".
            if ((v >> 24) != 0 || ((v >> 16) & 0xFF) != lib) continue;
            const uint16_t ordinal = uint16_t(v);
            if (const uint32_t thunk = thunkOf[ordinal]) { Store32(r, thunk); slots++; continue; }

            // A kernel variable. Some are one object every module shares:
            const bool kernelLib = lib == libraries - 1;    // xam first, xboxkrnl last
            if (kernelLib && ordinal == 0x0AD)              // KeTimeStampBundle
            { Store32(r, TimeStampBundle()); variables++; continue; }
            if (kernelLib && ordinal == 0x059)              // KeDebugMonitorData: null on retail
            { Store32(r, DebugMonitorData()); variables++; continue; }
            if (kernelLib && ordinal == 0x193)              // XexExecutableModuleHandle
            {
                // The variable holds the executable's LDR_DATA_TABLE_ENTRY.
                const uint32_t block = AllocateGuest(16, 16);
                Store32(block, modules::Handle(modules::kExecutable));
                Store32(r, block); variables++;
                continue;
            }
            // The rest get a block of their own, filled where the contents matter:
            uint32_t block = AllocateGuest(0x100, 16);
            if (!block) break;
            std::memset(guest::Base() + block, 0, 0x100);
            if (kernelLib && ordinal == 0x1C1)              // VdHSIOCalibrationLock
                InitialiseCriticalSection(block);
            else if (kernelLib && ordinal == 0x1C0)         // VdGpuClockInMHz
                Store32(block, 500);
            else if (kernelLib && ordinal == 0x156)         // XboxHardwareInfo: flags, CPU count
            {
                Store32(block, 0);
                guest::Base()[block + 4] = 6;               // three cores, two threads each
            }
            else if (kernelLib && ordinal == 0x158)         // XboxKrnlVersion 2.0.17559.0
            {
                Store16(block, 2); Store16(block + 2, 0); Store16(block + 4, 17559); Store16(block + 6, 0);
            }
            else if (kernelLib && ordinal == 0x1AE)         // ExLoadedCommandLine
                std::memcpy(guest::Base() + block, "\"default.xex\"", 14);
            Store32(r, block);
            variables++;
            LOGI("imports: library %u ordinal %03X is a variable: block at %08X", lib, ordinal, block);
        }
        at += size;
    }
    LOGI("imports: header %08X: %u variables given blocks, %u function table slots pointed at their thunks",
         h, variables, slots);
}

// ---- the console's fatal stop ---------------------------------------------------------

namespace
{
    [[noreturn]] void BugCheck(const PPCContext& ctx, uint32_t code, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
    {
        LOGE("KeBugCheck %08X (%08X %08X %08X %08X) from %08X on '%s' -- the title stopped itself",
             code, a1, a2, a3, a4, uint32_t(ctx.lr), crash::CurrentThreadName());
        platform::PrintBacktrace("KeBugCheck");
        // The console halts here. Parking the thread keeps the rest of the log
        // (and the other threads) observable instead of tearing the process down.
        for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    }
}

PPC_FUNC(__imp__KeBugCheck)   { BugCheck(ctx, ctx.r3.u32, 0, 0, 0, 0); }
PPC_FUNC(__imp__KeBugCheckEx) { BugCheck(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
