#!/usr/bin/env python3
"""Locate the compiler's register save/restore helpers in the decoded image and
print (or write) the XenonRecomp config lines for them.

XenonRecomp wants the start of each helper (prologue/epilogue sequences for
r14-r31 + LR, f14-f31, v14-v31 and v64-v127); they are not auto-detected, and the
patterns below are the ones its README gives, made executable:

  savegprlr_14  std r14,-0x98(r1); std r15,-0x90(r1)
  restgprlr_14  ld  r14,-0x98(r1); ld  r15,-0x90(r1)
  savefpr_14    stfd f14,-0x90(r12); stfd f15,-0x88(r12)
  restfpr_14    lfd  f14,-0x90(r12); lfd  f15,-0x88(r12)
  savevmx_14    li r11,-0x120; stvx v14,r1,r11
  restvmx_14    li r11,-0x120; lvx  v14,r1,r11
  savevmx_64    li r11,-0x400; stvx128 v64,r1,r11
  restvmx_64    li r11,-0x400; lvx128  v64,r1,r11

The vmx_14 pair is not always matchable by its own bytes (the compiler may
schedule the `li` differently); it is derived from the vmx_64 pair, which lies a
fixed 0x94 bytes (18 registers x 2 instructions + blr) after it.

usage: find_helpers.py [--write config/PES.toml]
Exits non-zero when a helper is missing or found more than once.
"""
import re, struct, sys
import title

img = title.image(); BASE = title.BASE
PATTERNS = {
    "savegprlr_14": "F9C1FF68F9E1FF70", "restgprlr_14": "E9C1FF68E9E1FF70",
    "savefpr_14": "D9CCFF70D9ECFF78",   "restfpr_14": "C9CCFF70C9ECFF78",
    "savevmx_64": "3960FC00100B61CB",   "restvmx_64": "3960FC00100B60CB",
}


def find_all(hexpat):
    pat, out, i = bytes.fromhex(hexpat), [], 0
    while (i := img.find(pat, i)) >= 0:
        if i % 4 == 0:
            out.append(BASE + i)
        i += 4
    return out


found, problems = {}, []
for name, pat in PATTERNS.items():
    hits = find_all(pat)
    if len(hits) == 1:
        found[name] = hits[0]
    else:
        problems.append(f"{name}: {len(hits)} matches {[hex(h) for h in hits]}")
for pair, sixty_four in (("savevmx_14", "savevmx_64"), ("restvmx_14", "restvmx_64")):
    if sixty_four in found:
        a = found[sixty_four] - 0x94
        want = bytes.fromhex("3960FEE0")      # li r11,-0x120
        if img[a - BASE:a - BASE + 4] == want:
            found[pair] = a
        else:
            problems.append(f"{pair}: no 'li r11,-0x120' at {a:08X} (vmx_64 - 0x94)")

order = ["restgprlr_14", "savegprlr_14", "restfpr_14", "savefpr_14",
         "restvmx_14", "savevmx_14", "restvmx_64", "savevmx_64"]
lines = [f"{k:<13}_address = 0x{found[k]:08X}".replace(f"{k:<13}_", k + "_") for k in order if k in found]
print("\n".join(lines))
for p in problems:
    print("!!", p, file=sys.stderr)

if "--write" in sys.argv and not problems:
    path = sys.argv[sys.argv.index("--write") + 1]
    s = open(path).read()
    for k in order:
        s = re.sub(rf"^{k}_address\s*=.*$", f"{k}_address = 0x{found[k]:08X}", s, flags=re.M)
    open(path, "w").write(s)
    print(f"wrote {path}", file=sys.stderr)
sys.exit(1 if problems else 0)
