#!/usr/bin/env python3
"""Find branches whose target label is not in the function that contains them,
and derive the explicit function extents that make them reachable.

Some code (a `bdzf` ladder dispatching into an unrolled tail, for instance) jumps
to addresses that XenonAnalyse has made the entry points of *other* functions.
XenonRecomp then emits `goto loc_X` with no such label, which the C++ compiler
rejects ("use of undeclared label") only after minutes of building. This reads
the generated C++ instead, so it is a check that runs in seconds.

For each offending function F with outside targets T, the fix is a `functions`
entry covering F up to the first function start after the highest target, so the
targets become interior labels. The entries are merged into config/PES.toml
between marker comments, replacing the previous batch; run build.sh's loop
(recompile -> this tool) until it reports nothing.

usage: fix_ladders.py <ppc-dir> [--apply config/PES.toml]
"""
import bisect, glob, re, sys
import title

ppc = sys.argv[1]
func_re = re.compile(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)', re.M)
goto_re = re.compile(r'goto (loc_[0-9A-F]+);')
# A branch whose target is neither a label of its function nor a function start:
# XenonRecomp writes "// ERROR <target>" and returns there, silently.
# A conditional branch has its `if (...) {` line between the two.
error_re = re.compile(r'^\t// (b|b[a-z]+) [^\n]*\n(?:\tif \([^\n]*\{\n)?\t*// ERROR ([0-9A-F]+)$', re.M)
label_re = re.compile(r'^(loc_[0-9A-F]+):', re.M)

mapping = open(f"{ppc}/ppc_func_mapping.cpp").read()
starts = sorted(set(int(x, 16) for x in re.findall(r'\{ 0x([0-9A-F]{8}), ', mapping)))

bad = {}   # function start -> set of outside targets
dead = {}  # branch target -> functions that branch there and return instead
for path in glob.glob(f"{ppc}/ppc_recomp.*.cpp"):
    src = open(path).read()
    heads = [(m.start(), int(m.group(1), 16)) for m in func_re.finditer(src)]
    for i, (pos, addr) in enumerate(heads):
        end = heads[i + 1][0] if i + 1 < len(heads) else len(src)
        body = src[pos:end]
        missing = set(goto_re.findall(body)) - set(label_re.findall(body))
        if missing:
            bad.setdefault(addr, set()).update(int(m[4:], 16) for m in missing)
        for m in error_re.finditer(body):
            dead.setdefault(int(m.group(2), 16), set()).add(addr)

img = title.image()


def to_return(addr):
    """Size of the block at addr up to and including its first blr/bctr/b."""
    a = addr
    while True:
        w = int.from_bytes(img[a - title.BASE:a - title.BASE + 4], "big")
        a += 4
        if w in (0x4E800020, 0x4E800420) or (w >> 26) == 18 and not (w & 1):
            return a - addr


def straight(addr, size):
    """True when [addr, addr + size) branches nowhere before its last instruction."""
    for a in range(addr, addr + size - 4, 4):
        w = int.from_bytes(img[a - title.BASE:a - title.BASE + 4], "big")
        if (w >> 26) in (16, 18) or ((w >> 26) == 19 and ((w >> 1) & 0x3FF) in (16, 528)):
            return False
    return True


entries = {}
unresolved = []
# A dead branch into a block that runs straight to its blr (or a tail branch) --
# the shared ladder tails, MSVC's exception-handling epilogues -- is a call to
# that block made into a function: the same instructions, then the same return.
for t, sources in sorted(dead.items()):
    if t in starts:
        continue
    size = to_return(t)
    if straight(t, size):
        entries[t] = size
        print(f"  {t:08X}: dead branch target from {len(sources)} function(s) -> own function, size 0x{size:X}")
    else:
        unresolved.append(t)

for fn, targets in sorted(bad.items()):
    # A target before the function start (FIFA Street's leaf ladders branch back
    # into shared `li r3,N; blr` tails) cannot become a label by extending the
    # function; make it a function of its own so the branch is a tail call.
    for t in sorted(x for x in targets if x < fn):
        entries[t] = to_return(t)
        print(f"  sub_{fn:08X}: backward target {t:08X} -> own function, size 0x{entries[t]:X}")
    targets = {x for x in targets if x > fn}
    if not targets:
        continue
    hi = max(targets | {fn})
    j = bisect.bisect_right(starts, hi)
    end = starts[j] if j < len(starts) else hi + 4
    entries[fn] = end - fn
    print(f"  sub_{fn:08X}: {len(targets)} outside target(s), up to {max(targets):08X} -> size 0x{end - fn:X}")
print(f"{len(entries)} function(s) need an explicit extent", file=sys.stderr)
if unresolved:
    print(f"{len(unresolved)} dead branch target(s) left (not straight-line code): "
          + " ".join(f"{t:08X}" for t in unresolved), file=sys.stderr)

import tomlblock
grew = False
if "--apply" in sys.argv:
    path = sys.argv[sys.argv.index("--apply") + 1]
    grew = tomlblock.grew(path, "fix_ladders", entries)
    n = tomlblock.apply(path, "fix_ladders", entries)
    print(f"applied: {n} entries in {path}", file=sys.stderr)
else:
    grew = bool(entries)
# Non-zero only when the config changed: entries it already holds that the
# recompiler cannot act on would otherwise loop recompile.sh forever.
sys.exit(1 if grew else 0)
