#!/usr/bin/env python3
"""The FIFA Street module the tools work on: flat image, recompiled C++, switch
tables and build directory names. Section table is read from the image.

FIFA Street ships two code modules: default.xex (a small loader, base 0x82000000)
and fifadllzf.xex (the game, base 0x82300000). Each is recompiled on its own;
FIFA_MODULE=exe|dll picks which one the tools see (default: dll)."""
import os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TITLES = {
    "exe": dict(base=0x82000000, xex="fifa/default.xex", image="fifa/default.pe", ppc="ppc/exe",
                toml="config/exe.toml", tables="config/exe_switch_tables.toml", build="build",
                syms="config/exe_symbols.txt"),
    "dll": dict(base=0x82300000, xex="fifa/fifadllzf.xex", image="fifa/fifadllzf.pe", ppc="ppc/dll",
                toml="config/dll.toml", tables="config/dll_switch_tables.toml", build="build",
                syms="config/dll_symbols.txt"),
    # Both modules at once: only the merged outputs (ppc/kernel_imports.txt, stubs).
    "all": dict(base=0x82000000, xex="fifa/default.xex", image="fifa/default.pe", ppc="ppc",
                toml="config/exe.toml", tables="config/exe_switch_tables.toml", build="build",
                syms="config/exe_symbols.txt"),
}
NAME = os.environ.get("FIFA_MODULE", "dll")
_t = TITLES[NAME]
BASE = _t["base"]

XEX = os.path.join(ROOT, _t["xex"])
IMAGE = os.path.join(ROOT, _t["image"])
PPC = os.path.join(ROOT, _t["ppc"])
TOML = os.path.join(ROOT, _t["toml"])
TABLES = os.path.join(ROOT, _t["tables"])
BUILD = os.path.join(ROOT, _t["build"])
SYMBOLS = os.path.join(ROOT, _t["syms"])


def ppc(name):
    """A file in the title's recompiled-output directory."""
    return os.path.join(PPC, name)


_syms = None
_syms_by_name = None


def _load_symbols():
    """{address: name} from the title's symbol map, a hand-maintained sidecar
    that names functions the recompiler only knows as sub_<ADDR>. One entry per
    line, `ADDR  name` (ADDR hex, 0x optional); blank lines and # comments are
    ignored. Missing file -> no names. The recompiled C++ is generated and keyed
    by address, so names live here, not in it."""
    global _syms, _syms_by_name
    if _syms is not None:
        return _syms
    _syms, _syms_by_name = {}, {}
    try:
        lines = open(SYMBOLS).read().splitlines()
    except FileNotFoundError:
        return _syms
    for raw in lines:
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.replace("=", " ").split()
        if len(parts) < 2:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        name = parts[1]
        _syms[addr] = name
        _syms_by_name[name] = addr
    return _syms


def symbols():
    """{address: name} for this title (see _load_symbols)."""
    return dict(_load_symbols())


def symname(addr):
    """The name mapped to a guest address, or None."""
    return _load_symbols().get(addr)


def symaddr(name):
    """The guest address of a named symbol, or None."""
    _load_symbols()
    return _syms_by_name.get(name)


def label(addr):
    """`sub_XXXXXXXX` for a guest address, with ` (name)` appended when known --
    the way the tools print a function."""
    name = symname(addr)
    return f"sub_{addr:08X}" + (f" ({name})" if name else "")


def image():
    return open(IMAGE, "rb").read()


def sections(img):
    """{name: (guest address, size)} from the PE section table. The image is
    flat, so a section's file offset equals its address minus BASE."""
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    count = struct.unpack_from("<H", img, pe + 6)[0]
    optional = struct.unpack_from("<H", img, pe + 20)[0]
    out = {}
    for i in range(count):
        o = pe + 24 + optional + i * 40
        name = img[o:o + 8].rstrip(b"\0").decode()
        size, rva = struct.unpack_from("<II", img, o + 8)
        out[name] = (BASE + rva, size)
    return out


def text(img):
    """(guest address, size) of the code section."""
    return sections(img)[".text"]


def functions(img):
    """{start: size} for every function .pdata describes. Leaf functions have
    no entry; the callers are the way to find those."""
    va, size = sections(img)[".pdata"]
    pd = img[va - BASE:va - BASE + size]
    out = {}
    for i in range(len(pd) // 8):
        a, d = struct.unpack_from(">II", pd, i * 8)
        if a == 0:
            break
        out[a] = ((d >> 8) & 0x3FFFFF) * 4
    return out


if __name__ == "__main__":
    img = image()
    t, f = text(img), functions(img)
    print(f"title {NAME}: {IMAGE}")
    print(f"  code {t[0]:08X} +{t[1]:X}, {len(f)} functions in .pdata, recompiled output in {PPC}")
