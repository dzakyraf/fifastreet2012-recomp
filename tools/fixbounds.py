#!/usr/bin/env python3
"""Derive explicit function boundaries for leaf functions whose jump-table cases
fall outside the analyzer's detected extent.

Function starts are taken from: .pdata entries, every `bl` target in .text, and
the first instruction after a run of zero padding. A function containing a jump
table then runs from the greatest such start <= the bctr, to the smallest start
greater than the highest case label (backing off over trailing padding)."""
import struct, re, sys, bisect, collections
import title

img = title.image(); BASE = title.BASE
def w(a): return struct.unpack_from(">I", img, a-BASE)[0]
TVA, TSZ = title.text(img)

starts = set(title.functions(img))

prev_pad=False
for a in range(TVA, TVA+TSZ, 4):
    x=w(a)
    if (x>>26)==18 and (x&1):                       # bl
        d=x&0x03FFFFFC
        if d & 0x02000000: d-=0x04000000
        t = d if (x&2) else a+d
        if TVA<=t<TVA+TSZ: starts.add(t)
    if prev_pad and x!=0: starts.add(a)
    prev_pad = (x==0)
# Functions reached only through a vtable or a function-pointer table are no bl target and have no
# .pdata, so nothing above knows them (sub_82848EE8 sat inside sub_82848DE8, whose bound was stretched
# over it and over ITS jump table -- the in-match freeze's second cause). A code pointer stored in the
# data, right after a return or branch or padding, is a function start; the labels of the jump
# tables are code pointers too and are not.
import vtable_starts
_labels = vtable_starts.labels_of(sys.argv[1])
_vt = vtable_starts.find(img, BASE, (TVA, TSZ), _labels) - starts
starts |= _vt
print(f"  vtable-only function starts found: {len(_vt)}", file=sys.stderr)
starts=sorted(starts)

tables={}
for blk in open(sys.argv[1]).read().split("[[switch]]")[1:]:
    b=int(re.search(r"base = 0x([0-9A-F]+)",blk).group(1),16)
    d=int(re.search(r"default = 0x([0-9A-F]+)",blk).group(1),16)
    labs=[int(x,16) for x in re.findall(r"^    0x([0-9A-F]+),$",blk,re.M)]
    tables[b]=(labs,d)

bad=set()
for line in open(sys.argv[2]):
    m=re.match(r"ERROR: Switch case at ([0-9A-F]+) ",line)
    if m: bad.add(int(m.group(1),16))

best={}
for bctr in sorted(bad):
    tb=bctr-0x14
    if tb not in tables:
        c=[b for b in tables if b<bctr and bctr-b<=0x30]
        if not c: print(f"  !! no table for bctr 0x{bctr:08X}"); continue
        tb=max(c)
    labs,dflt=tables[tb]
    targets=[t for t in labs+[dflt] if t and TVA<=t<TVA+TSZ]  # drop table entries outside code
    if not targets: print(f"  !! bctr 0x{bctr:08X}: no in-code targets"); continue
    hi=max(targets)
    s = starts[bisect.bisect_right(starts,bctr)-1]
    j = bisect.bisect_right(starts,hi)
    e = starts[j] if j<len(starts) else hi+0x100
    while e-4>hi and w(e-4)==0: e-=4                 # back off trailing padding
    # Functions reached only through a vtable are no bl target, so the next known
    # start may lie past one (sub_82800C88 swallowed sub_82800D08, a thunk the
    # match calls 400 times). End instead at the first return -- blr, or a b out
    # of the function -- at or after the highest label that no branch of the
    # function jumps past.
    reach=hi
    for a in range(s, e, 4):
        x=w(a); op=x>>26
        if op==16 and not (x&1):                     # bc (not bcl): an internal target
            d=x&0xFFFC
            if d&0x8000: d-=0x10000
            t=d if (x&2) else a+d
            if s<=t<e: reach=max(reach,t)
        if op==18 and not (x&1):                     # b
            d=x&0x03FFFFFC
            if d&0x02000000: d-=0x04000000
            t=d if (x&2) else a+d
            if s<=t<e: reach=max(reach,t)
            elif a>=reach: e=a+4; break
        if x==0x4E800020 and a>=reach: e=a+4; break  # blr
    if min(targets) < s or hi >= e:
        print(f"  !! could not bracket bctr 0x{bctr:08X} (start 0x{s:08X} end 0x{e:08X} targets 0x{min(targets):08X}..0x{hi:08X})")
        continue
    best[s]=max(best.get(s,0), e)

print("=== derived boundaries ===")
for s in sorted(best):
    e=best[s]
    absorbed=[x for x in starts if s<x<e]
    print(f"  0x{s:08X}..0x{e:08X} size 0x{e-s:X}   interior starts absorbed: {len(absorbed)}")
if "--apply" in sys.argv:
    import tomlblock
    cfg = sys.argv[sys.argv.index("--apply") + 1]
    changed = tomlblock.grew(cfg, "fixbounds", {s_: best[s_] - s_ for s_ in best})
    n = tomlblock.apply(cfg, "fixbounds", {s_: best[s_] - s_ for s_ in best})
    print(f"fixbounds: {n} entries in {cfg}" + (" (changed)" if changed else " (no change)"), file=sys.stderr)
    sys.exit(2 if changed else 0)
print("\n=== TOML ===")
for s in sorted(best):
    print(f"    {{ address = 0x{s:08X}, size = 0x{best[s]-s:X} }},")
