#!/usr/bin/env python3
"""Set each jump table's `r` to the register that really indexes it.

XenonAnalyse takes `r` from the cmplwi guarding the dispatch. Usually that is the
index, but the compiler may compare one copy (e.g. r21) and index with another
(r11, reloaded from a spill slot), and an earlier version of trim_switch_tables.py
overwrote `r` with the label count. The recompiler switches on ctx.r<r> and its
default case returns -- without the function's epilogue -- so a wrong register
leaves the guest stack unbalanced (sub_834E62A0: r1 off by its 944-byte frame,
the callers' saved registers read from the wrong place, a job worker deleting
"job 0x10" during the Exhibition match load).

The index register is read from the dispatch itself, walking back from the bctr:
mtctr rT <- lwzx/lhzx/lbzx rT,rA,rB (+ add for offset tables) <- rlwinm rB,rI,sh
(scaling) or rI used directly.
usage: fix_switch_regs.py <switch_tables.toml> [--apply]"""
import re, struct, sys
import title

img = title.image()
BASE = title.BASE

def word(va):
    return struct.unpack_from('>I', img, va - BASE)[0]

def fields(w):
    return w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 1) & 0x3FF

def index_register(base):
    """`base` is where the dispatch sequence starts (after the guard's branch).
    The table load (lwzx/lhzx/lbzx) between it and the bctr has the table base
    (built by lis/addi) as one operand and the index as the other -- scaled by an
    rlwinm earlier in the sequence for word/halfword tables. Returns the index
    register, or None when the sequence has another shape."""
    seq = []
    for k in range(-2, 14):
        w = word(base + 4 * k)
        seq.append(w)
        if w == 0x4E800420: break
    else:
        return None
    load = None
    for n, w in enumerate(seq):
        op, d, a, b, xo = fields(w)
        if op == 31 and xo in (23, 279, 87):
            load = (n, a, b)
            break
    if not load: return None
    n, a, b = load
    built = set()                               # registers lis/addi set before the load
    for w in seq[:n]:
        op, d, ra, _, _ = fields(w)
        if op in (14, 15): built.add(d)
    others = [r for r in (a, b) if r not in built]
    if len(others) != 1: return None
    idx = others[0]
    for w in reversed(seq[:n]):                 # a scaling rlwinm defines it from the index
        op, src, dest, sh, _ = fields(w)
        if op == 21 and dest == idx and sh in (1, 2):
            return src
    return idx

path = sys.argv[1]; apply = '--apply' in sys.argv
text = open(path).read()
head, *blocks = text.split('[[switch]]')
changed = unknown = 0
out = []
for b in blocks:
    base = int(re.search(r'base = 0x([0-9A-F]+)', b).group(1), 16)
    r = int(re.search(r'^r = (\d+)', b, re.M).group(1))
    real = index_register(base)
    if real is None:
        unknown += 1
    elif real != r:
        changed += 1
        print(f'{base:08X}: r{r} -> r{real}')
        b = re.sub(r'^r = \d+', f'r = {real}', b, count=1, flags=re.M)
    out.append(b)
if apply:
    open(path, 'w').write(head + ''.join('[[switch]]' + b for b in out))
print(f'{changed} of {len(blocks)} tables index with another register; {unknown} dispatches not decoded', file=sys.stderr)
sys.exit(1 if changed and apply else 0)
