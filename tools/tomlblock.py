"""Merge `{ address, size }` entries into a marked block of a recompiler config.

Each generator tool owns one block, delimited by `# --- <tool> begin/end ---`
inside `functions = [ ... ]`. Re-running a tool replaces its own block and never
touches the others; an address already in the block keeps the larger extent, so
repeated passes only grow function bounds and the loop converges.
"""
import re

ENTRY = re.compile(r"address = 0x([0-9A-Fa-f]+), size = 0x([0-9A-Fa-f]+)")


def apply(path, tool, entries):
    begin, end = f"# --- {tool} begin ---", f"# --- {tool} end ---"
    s = open(path).read()
    block_re = re.compile(re.escape(begin) + r"(.*?)" + re.escape(end), re.S)
    m = block_re.search(s)
    merged = {int(a, 16): int(z, 16) for a, z in ENTRY.findall(m.group(1))} if m else {}
    for a, z in entries.items():
        merged[a] = max(merged.get(a, 0), z)
    block = begin + "\n" + "".join(f"    {{ address = 0x{a:08X}, size = 0x{z:X} }},\n"
                                   for a, z in sorted(merged.items())) + end
    if m:
        s = s[:m.start()] + block + s[m.end():]
    else:
        s = s.replace("functions = [\n", "functions = [\n" + block + "\n", 1)
    open(path, "w").write(s)
    return len(merged)


def grew(path, tool, entries):
    """True if applying `entries` would add or enlarge anything."""
    begin, end = f"# --- {tool} begin ---", f"# --- {tool} end ---"
    m = re.search(re.escape(begin) + r"(.*?)" + re.escape(end), open(path).read(), re.S)
    have = {int(a, 16): int(z, 16) for a, z in ENTRY.findall(m.group(1))} if m else {}
    return any(have.get(a, 0) < z for a, z in entries.items())
