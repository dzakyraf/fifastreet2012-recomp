// The title's code modules: mapping each at its base, its XEX header, its data
// imports, its function table slice and the handle the title is given for it.
// See modules.h; the module table itself is generated (ppc/ppc_modules.h).
#include "modules.h"
#include "kernel.h"
#include "../guest.h"
#include "../log.h"
#include "../install/xex.h"
#include <ppc_modules.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace
{
    struct State
    {
        bool loaded = false;
        uint32_t handle = 0;        // LDR_DATA_TABLE_ENTRY
        uint32_t header = 0, headerSize = 0;
        uint32_t exportTable = 0;   // the security info's export table
        uint32_t entryPoint = 0;
    };
    State g_state[kFifaModuleCount];
    std::recursive_mutex g_lock;

    uint32_t Load32(uint32_t a) { return *GuestPtr<be32>(a); }
    void Store32(uint32_t a, uint32_t v) { *GuestPtr<be32>(a) = v; }
    uint16_t Load16(uint32_t a) { return *GuestPtr<be16>(a); }
    void Store16(uint32_t a, uint16_t v) { *GuestPtr<be16>(a) = v; }

    uint32_t Be32(const std::vector<uint8_t>& d, size_t at)
    {
        return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(std::tolower(uint8_t(c)));
        return s;
    }

    bool Valid(int index) { return index >= 0 && index < kFifaModuleCount; }

    // A UNICODE_STRING { length, maximum, buffer } with its characters after it.
    uint32_t MakeUnicodeName(const char* name)
    {
        const uint32_t n = uint32_t(std::strlen(name));
        const uint32_t text = kernel::AllocateGuest(n * 2 + 2, 4);
        for (uint32_t i = 0; i <= n; i++) Store16(text + i * 2, uint8_t(name[i]));
        return text;
    }

    // X_LDR_DATA_TABLE_ENTRY (Xenia's layout); the three list heads point at themselves.
    uint32_t MakeHandle(int index, const State& s)
    {
        const FifaModule& m = kFifaModules[index];
        const uint32_t entry = kernel::AllocateGuest(0x100, 16);
        if (!entry) return 0;
        std::memset(guest::Base() + entry, 0, 0x100);
        for (uint32_t link = 0; link < 0x18; link += 8) { Store32(entry + link, entry + link); Store32(entry + link + 4, entry + link); }
        Store32(entry + 0x18, uint32_t(m.imageBase));       // DllBase
        Store32(entry + 0x1C, uint32_t(m.imageBase));       // ImageBase
        Store32(entry + 0x20, uint32_t(m.imageSize));       // SizeOfImage
        const uint32_t n = uint32_t(std::strlen(m.name));
        const uint32_t text = MakeUnicodeName(m.name);
        for (uint32_t name : { entry + 0x24u, entry + 0x2Cu })   // FullDllName, BaseDllName
        {
            Store16(name, uint16_t(n * 2)); Store16(name + 2, uint16_t(n * 2 + 2)); Store32(name + 4, text);
        }
        Store32(entry + 0x38, uint32_t(m.imageSize));       // FullImageSize
        Store32(entry + 0x3C, s.entryPoint);                // EntryPoint
        Store16(entry + 0x40, 1);                           // LoadCount
        Store16(entry + 0x42, uint16_t(index));             // ModuleIndex
        Store32(entry + 0x44, uint32_t(m.imageBase));       // DllBaseOriginal
        Store32(entry + 0x58, s.header);                    // XexHeaderBase
        return entry;
    }
}

size_t kernel::modules::FillFunctionTable(int index)
{
    if (!Valid(index)) return 0;
    size_t n = 0;
    for (const PPCFuncMapping* m = kFifaModules[index].functions; m->guest != 0 || m->host != nullptr; ++m)
    {
        if (!m->host) continue;
        if (m->guest < PPC_CODE_BASE || m->guest >= PPC_CODE_BASE + PPC_CODE_SIZE)
        {
            LOGW("function %08zX outside code range, skipped", m->guest);
            continue;
        }
        *(PPCFunc**)(guest::Base() + guest::FunctionSlotOffset(uint32_t(m->guest), uint32_t(PPC_CODE_BASE))) = m->host;
        n++;
    }
    return n;
}

bool kernel::modules::Map(int index, const std::vector<uint8_t>& xexFile, const std::vector<uint8_t>& image)
{
    std::lock_guard held(g_lock);
    if (!Valid(index)) return false;
    State& s = g_state[index];
    if (s.loaded) return true;
    const FifaModule& m = kFifaModules[index];

    // The image, at the base it was recompiled for.
    if (uint64_t(image.size()) > m.imageSize)
        LOGW("%s: image is %zu bytes, larger than its %llu; truncating", m.name, image.size(),
             (unsigned long long)m.imageSize);
    const size_t size = std::min<size_t>(image.size(), size_t(m.imageSize));
    std::memcpy(guest::Base() + m.imageBase, image.data(), size);

    // The XEX header, which the title and the kernel read fields of.
    if (xexFile.size() < 0x18) { LOGE("%s: no XEX header", m.name); return false; }
    const uint32_t headerSize = Be32(xexFile, 8);
    if (headerSize < 0x18 || headerSize > xexFile.size() || headerSize > (4u << 20))
    { LOGE("%s: odd XEX header size %u", m.name, headerSize); return false; }
    s.header = AllocateGuest(headerSize, 16);
    if (!s.header) return false;
    std::memcpy(guest::Base() + s.header, xexFile.data(), headerSize);
    s.headerSize = headerSize;
    s.entryPoint = Field(index, 0x00010100);
    // xex2_security_info.export_table: an address in the image, or 0.
    const uint32_t security = Be32(xexFile, 0x10);
    if (security + 0x164 <= headerSize) s.exportTable = Be32(xexFile, security + 0x160);

    s.handle = MakeHandle(index, s);
    s.loaded = true;   // before the imports: XexExecutableModuleHandle names the handle

    const size_t functions = FillFunctionTable(index);
    ResolveDataImports(s.header, s.headerSize);
    LOGI("module %s: %zu bytes at %08X, header at %08X, entry %08X, %zu functions, handle %08X%s",
         m.name, size, uint32_t(m.imageBase), s.header, s.entryPoint, functions, s.handle,
         s.exportTable ? ", has exports" : "");
    return true;
}

int kernel::modules::Find(const std::string& guestPath)
{
    std::string name = guestPath;
    if (const size_t slash = name.find_last_of("\\/:"); slash != std::string::npos) name.erase(0, slash + 1);
    name = Lower(name);
    for (int i = 0; i < kFifaModuleCount; i++)
    {
        const std::string mine = Lower(kFifaModules[i].name);
        // fifadllzf.xex.dll is also asked for without its .dll.
        if (name == mine || name + ".dll" == mine) return i;
    }
    return -1;
}

int kernel::modules::FromHandle(uint32_t handle)
{
    if (!handle) return -1;
    for (int i = 0; i < kFifaModuleCount; i++)
        if (g_state[i].loaded && g_state[i].handle == handle) return i;
    return -1;
}

bool kernel::modules::Loaded(int index)         { return Valid(index) && g_state[index].loaded; }
uint32_t kernel::modules::Handle(int index)     { return Valid(index) ? g_state[index].handle : 0; }
uint32_t kernel::modules::Header(int index)     { return Valid(index) ? g_state[index].header : 0; }
uint32_t kernel::modules::HeaderSize(int index) { return Valid(index) ? g_state[index].headerSize : 0; }
uint32_t kernel::modules::ImageBase(int index)  { return Valid(index) ? uint32_t(kFifaModules[index].imageBase) : 0; }
uint32_t kernel::modules::EntryPoint(int index) { return Valid(index) ? g_state[index].entryPoint : 0; }
const char* kernel::modules::Name(int index)    { return Valid(index) ? kFifaModules[index].name : "?"; }

uint32_t kernel::modules::Field(int index, uint32_t key)
{
    if (!Valid(index)) return 0;
    const State& s = g_state[index];
    if (!s.header) return 0;
    const uint32_t count = Load32(s.header + 0x14);
    for (uint32_t i = 0; i < count && 0x18 + i * 8 + 8 <= s.headerSize; i++)
    {
        const uint32_t k = Load32(s.header + 0x18 + i * 8), v = Load32(s.header + 0x1C + i * 8);
        if (k != key) continue;
        return (key & 0xFF) == 0 ? v : s.header + v;
    }
    return 0;
}

// xex2_export_table: magic[3], modulenumber[2], version[3], imagebaseaddr (>> 16),
// count, base, then one offset (from the image base) per ordinal.
uint32_t kernel::modules::Export(int index, uint32_t ordinal)
{
    if (!Loaded(index)) return 0;
    const uint32_t table = g_state[index].exportTable;
    if (!table) return 0;
    const uint32_t imageBase = Load32(table + 0x20) << 16;
    const uint32_t count = Load32(table + 0x24), first = Load32(table + 0x28);
    if (ordinal < first || ordinal - first >= count) return 0;
    const uint32_t offset = Load32(table + 0x2C + (ordinal - first) * 4);
    return offset ? imageBase + offset : 0;
}

bool kernel::modules::Section(int index, const std::string& name, uint32_t& address, uint32_t& size)
{
    if (!Loaded(index)) return false;
    const uint32_t base = ImageBase(index);
    const uint8_t* image = guest::Base() + base;
    uint32_t pe;
    std::memcpy(&pe, image + 0x3C, 4);                      // the PE headers are little-endian
    uint16_t sections, optional;
    std::memcpy(&sections, image + pe + 6, 2);
    std::memcpy(&optional, image + pe + 20, 2);
    for (uint16_t i = 0; i < sections; i++)
    {
        const uint8_t* header = image + pe + 24 + optional + i * 40;
        const std::string sectionName(reinterpret_cast<const char*>(header), strnlen(reinterpret_cast<const char*>(header), 8));
        if (sectionName != name) continue;
        uint32_t virtualSize, rva;
        std::memcpy(&virtualSize, header + 8, 4);
        std::memcpy(&rva, header + 12, 4);
        address = base + rva;
        size = virtualSize;
        return true;
    }
    return false;
}
