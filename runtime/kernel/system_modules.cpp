// The system modules (xam.xex, xboxkrnl.exe) as the title sees them when it looks inside at run time.
//
// Most of what a title calls in them it imports, and the import table is resolved at load. FIFA Street
// also asks for some by ordinal -- XexGetModuleHandle("xam.xex"), then XexGetProcedureAddress(handle, n)
// -- and calls what it is given through a pointer. There is no module image here to export from, so the
// ones that are provided get a stub: an address in the image's unused gap (inside the range the
// recompiled code's indirect calls accept) whose function-table slot is the host function.
#include <ppc_recomp_shared.h>
#include "kernel.h"
#include "../guest.h"
#include "../log.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

namespace
{
    struct Export { uint32_t ordinal; PPCFunc* function; const char* name; };

    // xam.xex (module 0). Add an entry when a run logs "XexGetProcedureAddress: ... ordinal N not found".
    const Export kXam[] = {
        { 624, kernel::XamContentDeleteTitle, "XamContentDeleteTitle" },
        { 625, kernel::XamContentCreateTitle, "XamContentCreateTitle" },
        { 633, kernel::XamContentAggregateCreateEnumerator, "XamContentAggregateCreateEnumerator" },
    };

    constexpr const char* kNames[] = { "xam.xex", "xboxkrnl.exe" };
    constexpr int kModuleCount = 2;

    // Between the executable's image and the game DLL's: committed, zero, and never executed.
    constexpr uint32_t kStubBase = 0x821C0000u;

    std::mutex g_lock;
    uint32_t g_handles[kModuleCount]{};
    std::map<uint32_t, uint32_t> g_stubs;   // (module << 16 | ordinal) -> address
    uint32_t g_nextStub = kStubBase;
}

int kernel::SystemModule(const std::string& name)
{
    std::string base = name;
    if (const size_t slash = base.find_last_of("\\/:"); slash != std::string::npos) base.erase(0, slash + 1);
    for (char& c : base) c = char(std::tolower(uint8_t(c)));
    for (int i = 0; i < kModuleCount; i++) if (base == kNames[i]) return i;
    return -1;
}

uint32_t kernel::SystemModuleHandle(int module)
{
    if (module < 0 || module >= kModuleCount) return 0;
    std::lock_guard held(g_lock);
    if (!g_handles[module])
    {
        // A small block of its own: the title only compares the handle with null and hands it back.
        g_handles[module] = AllocateGuest(0x100, 16);
        if (g_handles[module]) std::memset(guest::Base() + g_handles[module], 0, 0x100);
    }
    return g_handles[module];
}

int kernel::SystemModuleOfHandle(uint32_t handle)
{
    if (!handle) return -1;
    std::lock_guard held(g_lock);
    for (int i = 0; i < kModuleCount; i++) if (g_handles[i] == handle) return i;
    return -1;
}

uint32_t kernel::SystemExport(int module, uint32_t ordinal)
{
    if (module != 0) return 0;
    for (const Export& e : kXam)
    {
        if (e.ordinal != ordinal) continue;
        std::lock_guard held(g_lock);
        const uint32_t key = uint32_t(module) << 16 | ordinal;
        if (const auto found = g_stubs.find(key); found != g_stubs.end()) return found->second;
        const uint32_t address = g_nextStub;
        g_nextStub += 8;
        *(PPCFunc**)(guest::Base() + guest::FunctionSlotOffset(address, uint32_t(PPC_CODE_BASE))) = e.function;
        g_stubs[key] = address;
        LOGI("system export: %s ordinal %u -> stub %08X (%s)", kNames[module], ordinal, address, e.name);
        return address;
    }
    return 0;
}
