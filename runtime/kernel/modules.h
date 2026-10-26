#pragma once
// The title's code modules (ppc/ppc_modules.h). FIFA Street is default.xex, a
// launcher, and fifadllzf.xex.dll, the game, which the launcher brings in with
// XexLoadImage and enters through XexGetProcedureAddress(handle, 1). Each was
// recompiled on its own, at its own base, so loading one is mapping its image
// there and pointing the function table at its functions.
#include <cstdint>
#include <string>
#include <vector>

namespace kernel::modules
{
    // The module index of the executable (the one the process starts in).
    inline constexpr int kExecutable = 0;

    // Maps a module's decoded image (install::xex::Image), copies its XEX header
    // into guest memory, resolves its data imports, fills its slice of the
    // function table and makes its LDR_DATA_TABLE_ENTRY. The XEX file is needed
    // for the header; `image` is what the recompiled code was made from.
    bool Map(int index, const std::vector<uint8_t>& xexFile, const std::vector<uint8_t>& image);

    // Its function table slice alone, before the kernel is up (main.cpp).
    size_t FillFunctionTable(int index);

    // The module a guest path names ("game:\\fifadllzf.xex.dll"), by file name;
    // -1 for anything else (xam.xex, xboxkrnl.exe).
    int Find(const std::string& guestPath);
    // The module a handle (LDR_DATA_TABLE_ENTRY address) is, or -1.
    int FromHandle(uint32_t handle);

    bool     Loaded(int index);
    uint32_t Handle(int index);
    uint32_t Header(int index);         // guest address of its XEX header
    uint32_t HeaderSize(int index);
    uint32_t ImageBase(int index);
    uint32_t EntryPoint(int index);
    const char* Name(int index);

    // An optional header field: its value (key & 0xFF == 0) or its guest address; 0 if absent.
    uint32_t Field(int index, uint32_t key);
    // An export by ordinal, from the security info's export table; 0 if none.
    uint32_t Export(int index, uint32_t ordinal);
    // A PE section by name: address and size; false if there is none.
    bool Section(int index, const std::string& name, uint32_t& address, uint32_t& size);
}
