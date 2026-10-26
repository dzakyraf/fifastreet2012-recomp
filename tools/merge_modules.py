#!/usr/bin/env python3
"""Make the two recompiled modules (ppc/exe, ppc/dll) link into one library.

XenonRecomp treats each XEX as the whole program, so the two outputs clash:
  - both define PPCFuncMappings[]           -> PPCFuncMappings_exe / _dll
  - both define the register save/restore helpers (__savegprlr_14, ...) and
    the entry point _xstart
                                            -> the exe's copies get an _exe suffix
  - each ppc_config.h bounds PPC_CODE_* to its own .text, so an indirect call
    from the exe into a DLL export (XexGetProcedureAddress) is refused
                                            -> one span covering both modules;
                                               the per-module values stay as FIFA_<M>_*
  - each module gets its own kernel stubs   -> one ppc/stubs_generated.cpp for the
                                               union of both import lists

Writes ppc/ppc_modules.h (the module table for the runtime's XexLoadImage),
ppc/kernel_imports.txt, and ppc/ppc_recomp_shared.h: the runtime's view, which
declares both modules' functions and every import extern "C" (each module's own
header only knows its own, and a runtime PPC_FUNC(__imp__X) for an import of the
other module would otherwise get C++ linkage). Idempotent: run after every recompile of either module.
"""
import glob, os, re, subprocess, sys
import title

ROOT = title.ROOT
PPC = os.path.join(ROOT, "ppc")
MODULES = ("exe", "dll")
HELPER = re.compile(r"\b((?:__imp__)?(?:__(?:rest|save)(?:gprlr|fpr|vmx)_\d+|_xstart))(?!_exe)\b")


def write(path, text):
    """Writes only a changed file: every generated unit depends on these
    headers, and a rewrite with the same text would rebuild all of them."""
    if os.path.exists(path) and open(path).read() == text:
        return
    open(path, "w").write(text)


def config(m):
    s = open(os.path.join(PPC, m, "ppc_config.h")).read()
    vals = {}
    for key in ("IMAGE_BASE", "IMAGE_SIZE", "CODE_BASE", "CODE_SIZE"):
        mm = (re.search(rf"#define FIFA_{m.upper()}_{key} (0x[0-9A-Fa-f]+)", s)
              or re.search(rf"#define PPC_{key} (0x[0-9A-Fa-f]+)", s))
        vals[key] = int(mm.group(1), 16)
    return vals


for m in MODULES:
    if not os.path.isfile(os.path.join(PPC, m, "ppc_func_mapping.cpp")):
        sys.exit(f"ppc/{m} is missing; run MODULES={m} ./recompile.sh")

cfg = {m: config(m) for m in MODULES}
image_base = min(c["IMAGE_BASE"] for c in cfg.values())
image_end = max(c["IMAGE_BASE"] + c["IMAGE_SIZE"] for c in cfg.values())
code_base = min(c["CODE_BASE"] for c in cfg.values())
code_end = max(c["CODE_BASE"] + c["CODE_SIZE"] for c in cfg.values())

# 1. ppc_config.h: one span, per-module values kept under their own names.
for m in MODULES:
    path = os.path.join(PPC, m, "ppc_config.h")
    s = open(path).read()
    s = re.sub(r"#define (PPC|FIFA_[A-Z]+)_(IMAGE|CODE)_(BASE|SIZE) .*\n", "", s)
    # Every module's values in every copy: they share one include guard.
    per = "".join(f"#define FIFA_{n.upper()}_{k} 0x{v:X}ull\n" for n in MODULES for k, v in cfg[n].items())
    span = (f"#define PPC_IMAGE_BASE 0x{image_base:X}ull\n#define PPC_IMAGE_SIZE 0x{image_end - image_base:X}ull\n"
            f"#define PPC_CODE_BASE 0x{code_base:X}ull\n#define PPC_CODE_SIZE 0x{code_end - code_base:X}ull\n")
    s = s.replace("\n#ifdef PPC_INCLUDE_DETAIL", "\n" + per + "\n// Both modules: tools/merge_modules.py\n" + span + "\n#ifdef PPC_INCLUDE_DETAIL", 1)
    write(path, s)

# 2. Mapping tables and the exe's helpers.
for m in MODULES:
    for path in glob.glob(os.path.join(PPC, m, "*.cpp")) + glob.glob(os.path.join(PPC, m, "*.h")):
        s = open(path).read()
        t = re.sub(r"\bPPCFuncMappings\b", f"PPCFuncMappings_{m}", s)
        if m == "exe":
            t = HELPER.sub(r"\1_exe", t)
        if t != s:
            open(path, "w").write(t)

# 3. One set of kernel stubs for both modules.
need = set()
for m in MODULES:
    need |= {l.strip() for l in open(os.path.join(PPC, m, "kernel_imports.txt")) if l.strip()}
    stub = os.path.join(PPC, m, "stubs_generated.cpp")
    if os.path.exists(stub):
        os.remove(stub)
write(os.path.join(PPC, "kernel_imports.txt"), "".join(n + "\n" for n in sorted(need)))
env = dict(os.environ, FIFA_MODULE="all")
subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_kernel_stubs.py")], check=True, env=env)
# The stubs see one module's ppc_recomp_shared.h, which declares only that module's
# imports extern "C"; declare the union so every stub gets C linkage.
stubs = os.path.join(PPC, "stubs_generated.cpp")
s = open(stubs).read()
decls = 'extern "C" {\n' + "".join(f"PPC_FUNC(__imp__{n});\n" for n in sorted(need)) + "}\n\n"
open(stubs, "w").write(s.replace('#include "kernel/kernel.h"\n\n', '#include "kernel/kernel.h"\n\n' + decls, 1))

# 4. The runtime's header: one context, everything both modules declare.
import shutil
for f in ("ppc_config.h", "ppc_context.h"):
    write(os.path.join(PPC, f), open(os.path.join(PPC, "exe", f)).read())
names = set()
head = None
for m in MODULES:
    text = open(os.path.join(PPC, m, "ppc_recomp_shared.h")).read()
    if head is None:
        head = text[:text.index('extern "C" {')]
    names |= set(re.findall(r"^PPC_FUNC\(([A-Za-z0-9_]+)\);$", text, re.M))
names |= {"__imp__" + n for n in need}
write(os.path.join(PPC, "ppc_recomp_shared.h"),
    head + 'extern "C" {\n' + "".join(f"PPC_FUNC({n});\n" for n in sorted(names)) + "}\n")

# 5. Module table for the runtime (XexLoadImage maps the DLL and runs its entry).
rows = []
for m, xex in (("exe", "default.xex"), ("dll", "fifadllzf.xex.dll")):
    rows.append(f'    {{ "{xex}", FIFA_{m.upper()}_IMAGE_BASE, FIFA_{m.upper()}_IMAGE_SIZE, '
                f'FIFA_{m.upper()}_CODE_BASE, FIFA_{m.upper()}_CODE_SIZE, PPCFuncMappings_{m} }},')
write(os.path.join(PPC, "ppc_modules.h"),
    "// Generated by tools/merge_modules.py -- do not edit.\n#pragma once\n"
    '#include "exe/ppc_config.h"\n#include <cstdint>\n\nstruct PPCFuncMapping;\n'
    'extern PPCFuncMapping PPCFuncMappings_exe[];\nextern PPCFuncMapping PPCFuncMappings_dll[];\n\n'
    "struct FifaModule {\n    const char* name;    // as the game asks for it (XexLoadImage takes the path)\n"
    "    uint64_t imageBase, imageSize, codeBase, codeSize;\n    PPCFuncMapping* functions;\n};\n\n"
    "inline constexpr int kFifaModuleCount = 2;\ninline FifaModule kFifaModules[kFifaModuleCount] = {\n"
    + "\n".join(rows) + "\n};\n")

print(f"merged: image {image_base:08X}..{image_end:08X}, code {code_base:08X}..{code_end:08X}, "
      f"{len(need)} kernel imports")
