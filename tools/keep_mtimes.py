#!/usr/bin/env python3
"""Give regenerated files whose text did not change their old modification times.

recompile.sh rewrites every file of a module; each generated unit is minutes of
compiling and most do not change between runs, so ninja should not see them as new.
usage: keep_mtimes.py <new dir> <previous copy of it>
"""
import filecmp, os, sys

new, old = sys.argv[1], sys.argv[2]
kept = changed = 0
for name in sorted(os.listdir(new)):
    a, b = os.path.join(new, name), os.path.join(old, name)
    if os.path.isfile(b) and filecmp.cmp(a, b, shallow=False):
        st = os.stat(b)
        os.utime(a, ns=(st.st_atime_ns, st.st_mtime_ns))
        kept += 1
    else:
        changed += 1
print(f"    {new}: {changed} files changed, {kept} unchanged")
