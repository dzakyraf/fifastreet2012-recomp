#!/usr/bin/env python3
"""Jump tables XenonAnalyse did not recognise.

XenonAnalyse only finds `cmplwi; bgt; lis; rlwinm; addi; lwzx; mtctr; bctr` when the guard and the
dispatch sit together. PES's compiler sometimes puts stores between them (sub_8284E0F0: four `sth`
after the `cmplwi`), so the `bctr` became an indirect call to a case label -- "indirect call to
8284EBAC ... not identified as a function start" -- and the task that hit it never finished (the
in-match freeze). This finds every such dispatch in the image by walking back from each `bctr`:
  mtctr rT <- lwzx rT,rA,rB <- (rB = rlwinm rB,rI,2,0,29) and (rA = lis + addi)
and writes the ones the analyser's tables do not cover.

FIFA Street also has tables of halfword offsets from a base label, with no guard
in front (the index is loop state the compiler knew to be in range):
  lis/addi rT = table; rlwinm rX,rI,1,0,30; lhzx r0,rT,rX; lis/addi rB = base;
  add rC,rB,r0; mtctr rC; bctr
Their length is read from the table: entries while base + offset stays a word
inside the function.
usage: find_missed_switches.py <analyser tables.toml> <out extra.toml>"""
import re, struct, sys
import title

img = title.image(); BASE = title.BASE
TVA, TSZ = title.text(img)
def word(a): return struct.unpack_from('>I', img, a - BASE)[0]
def sx16(v): return v - 0x10000 if v & 0x8000 else v

covered = []
text = open(sys.argv[1]).read()
for m in re.finditer(r'\[\[switch\]\]\s*base = (0x[0-9A-Fa-f]+)', text):
    covered.append(int(m.group(1), 16))
covered.sort()
def is_covered(bctr):
    return any(bctr - 64 <= b <= bctr for b in covered)

starts = sorted(set(title.functions(img)))
import bisect
def function_of(a):
    i = bisect.bisect_right(starts, a) - 1
    return starts[i] if i >= 0 else TVA

found, skipped = [], []
for b in range(TVA, TVA + TSZ, 4):
    if word(b) != 0x4E800420 or is_covered(b): continue
    w = word(b - 4)
    if (w & 0xFC1FFFFF) != 0x7C0903A6: continue              # mtctr rT
    rT = (w >> 21) & 31
    w = word(b - 8)
    if (w >> 26) != 31 or ((w >> 1) & 0x3FF) != 23 or ((w >> 21) & 31) != rT: continue   # lwzx rT,rA,rB
    rA, rB = (w >> 16) & 31, (w >> 11) & 31
    # walk back for the table address (lis/addi on rA or rB) and the scaled index (rlwinm rB,rI,2,0,29)
    tbl = hi = None; idx = None; first = b - 8; lis_reg = None
    for a in range(b - 12, max(b - 12 * 4 - 4 * 24, TVA), -4):
        w = word(a); op = w >> 26
        # The dispatch is one basic block: past a b / blr / bctr is other code
        # (FIFA's 83B89FB8, a call through a struct of function pointers, borrowed
        # an unrelated block's rlwinm and guard and became a one-label "switch").
        if (op == 18 and not (w & 1)) or w in (0x4E800020, 0x4E800420): break
        if op == 21 and ((w >> 16) & 31) in (rB, rA) and ((w >> 11) & 31) == 2 and ((w >> 6) & 31) == 0 and ((w >> 1) & 31) == 29 and idx is None:
            idx = (w >> 21) & 31; scaled = (w >> 16) & 31; first = min(first, a)
        elif op == 14 and tbl is None and ((w >> 21) & 31) in (rA, rB) and ((w >> 16) & 31) in (rA, rB):
            lo = sx16(w & 0xFFFF); lis_reg = (w >> 16) & 31; tbl = lo; first = min(first, a)
        elif op == 15 and tbl is not None and hi is None and ((w >> 21) & 31) == lis_reg and ((w >> 16) & 31) == 0:
            hi = w & 0xFFFF; first = min(first, a)
        if idx is not None and hi is not None: break
    if idx is None or hi is None or tbl is None: continue
    table = ((hi << 16) + tbl) & 0xFFFFFFFF
    fn = function_of(b)
    # guard: cmplwi crX,rI,N ... bgt crX,default, looking back from the bctr
    n = default = None
    for a in range(b - 8, max(b - 4 * 80, TVA), -4):
        w = word(a)
        if ((w >> 26) == 18 and not (w & 1)) or w in (0x4E800020, 0x4E800420): break
        if (w >> 26) == 10 and ((w >> 16) & 31) == idx and n is None:
            n = w & 0xFFFF
        if (w >> 26) == 16 and (w >> 21) & 31 == 12 and (w & 3) == 0 and default is None and n is None:
            li = sx16(w & 0xFFFC); default = (a + li) & 0xFFFFFFFF
    if n is None or default is None: skipped.append((b, fn, 'no guard')); continue
    labels = []
    for i in range(n + 1):
        v = word(table + 4 * i)
        if not (TVA <= v < TVA + TSZ): break
        labels.append(v)
    if len(labels) != n + 1: skipped.append((b, fn, 'table runs out')); continue
    found.append((first, idx, default, labels, b, fn))

# ---- halfword offsets from a base, unguarded ----------------------------------
def half(a): return struct.unpack_from('>H', img, a - BASE)[0]
functions = title.functions(img)
def function_end(start):
    i = bisect.bisect_right(starts, start)
    nxt = starts[i] if i < len(starts) else TVA + TSZ
    return max(start + functions.get(start, 0), nxt)

for b in range(TVA, TVA + TSZ, 4):
    if word(b) != 0x4E800420 or is_covered(b): continue
    w = word(b - 4)
    if (w & 0xFC1FFFFF) != 0x7C0903A6: continue              # mtctr rC
    rC = (w >> 21) & 31
    regs = {}             # register -> constant built by lis/addi (walking forward)
    seq = [b - 4 * k for k in range(12, 0, -1)]
    lhzx = idx = None; target_base = None; first = None
    for a in seq:
        x = word(a); op = x >> 26
        rt, ra = (x >> 21) & 31, (x >> 16) & 31
        if op == 15 and ra == 0:                              # lis
            regs[rt] = (x & 0xFFFF) << 16; first = a if first is None else first
        elif op == 14 and ra in regs:                         # addi on a lis
            regs[rt] = (regs[ra] + sx16(x & 0xFFFF)) & 0xFFFFFFFF
        elif op == 21 and ((x >> 11) & 31) == 1 and ((x >> 6) & 31) == 0 and ((x >> 1) & 31) == 30:
            idx = (x >> 21) & 31                              # rlwinm rX,rI,1,0,30
        elif op == 31 and ((x >> 1) & 0x3FF) == 279:          # lhzx rD,rA,rB
            rA, rB = ra, (x >> 11) & 31
            table = regs.get(rA, regs.get(rB))
            if table is not None: lhzx = (rt, table)
        elif op == 31 and ((x >> 1) & 0x3FF) == 266 and lhzx and rt == rC:   # add rC,rB,rD
            rA, rB = ra, (x >> 11) & 31
            other = rB if rA == lhzx[0] else rA
            target_base = regs.get(other)
    if lhzx is None or idx is None or target_base is None: continue
    fn = function_of(b); end = function_end(fn)
    labels = []
    for i in range(4096):
        t = (target_base + half(lhzx[1] + 2 * i)) & 0xFFFFFFFF
        if t % 4 or not (fn <= t < end): break
        labels.append(t)
    if not labels: skipped.append((b, fn, 'halfword table empty')); continue
    # No guard, so no default: an index past the table returns, as XenonRecomp does.
    found.append((first, idx, b + 4, labels, b, fn))

with open(sys.argv[2], 'w') as out:
    out.write('# Generated by tools/find_missed_switches.py: jump tables XenonAnalyse did not recognise\n')
    for first, idx, default, labels, b, fn in sorted(found):
        out.write('[[switch]]\nbase = 0x%08X\nr = %d\ndefault = 0x%08X\nlabels = [\n' % (first, idx, default))
        out.write(''.join('    0x%08X,\n' % l for l in labels)); out.write(']\n')
print('missed jump tables:', len(found), ' skipped:', len(skipped))
for b, fn, why in skipped[:10]: print('  skipped bctr %08X in %08X: %s' % (b, fn, why))
for first, idx, default, labels, b, fn in sorted(found)[:8]: print('  table for bctr %08X in %08X: %d labels, index r%d' % (b, fn, len(labels), idx))
