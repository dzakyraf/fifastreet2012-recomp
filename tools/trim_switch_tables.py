#!/usr/bin/env python3
"""Cut XenonAnalyse's jump tables off at the first entry that is not a code address.

The analyser sizes a table from the bounds check before the dispatch; when a
table is followed by more code or data, an over-long count reads that as case
labels. Anything outside .text, or not word aligned, is not a label.
usage: trim_switch_tables.py <switch_tables.toml>   (rewritten in place)"""
import re, sys
import title

tva, tsz = title.text(title.image())
path = sys.argv[1]
head, *blocks = open(path).read().split('[[switch]]')
out, trimmed = [], 0
for b in blocks:
    vals = [int(x, 16) for x in re.findall(r'^\s+0x([0-9A-F]+),$', b, re.M)]
    n = next((i for i, v in enumerate(vals) if not (tva <= v < tva + tsz) or v % 4), len(vals))
    if n < len(vals):
        trimmed += 1
        kept, lines = 0, []
        for ln in b.split('\n'):
            if re.match(r'^\s+0x([0-9A-F]+),$', ln):
                kept += 1
                if kept > n:
                    continue
            lines.append(ln)
        # `r` is the index register, not a count; the label list alone is the size.
        b = '\n'.join(lines)
    out.append(b)
open(path, 'w').write(head + ''.join('[[switch]]' + b for b in out))
print(f"trimmed {trimmed} of {len(blocks)} tables")
