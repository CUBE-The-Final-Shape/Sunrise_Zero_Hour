#!/usr/bin/env python3
"""Explore the client's reflection registry in a memory-layout dump (WIP)."""
import struct, sys
DUMP = r"E:\Sunrise\destiny2_live_dump.bin"
BASE = 0x7ff729760000
GHIDRA_BASE = 0x7ff740df0000
f = open(DUMP, "rb")
def rd(va, n):
    f.seek(va - BASE); return f.read(n)
def u32(va): return struct.unpack("<I", rd(va, 4))[0]
def u64(va): return struct.unpack("<Q", rd(va, 8))[0]
def g(va):  # ghidra-space address for display
    return va - BASE + GHIDRA_BASE
# registry: 32-byte entries {tag, desc, parent?, back}; located from the known squad entry
REG_KNOWN = 0x7ff744012d80 - GHIDRA_BASE + BASE
def find_registry():
    # walk backwards/forwards while entries look valid
    start = REG_KNOWN
    while True:
        t = u64(start - 32)
        if not (0x80800000 <= t <= 0x8080ffff): break
        start -= 32
    end = REG_KNOWN
    while True:
        t = u64(end + 32)
        if not (0x80800000 <= t <= 0x8080ffff): break
        end += 32
    return start, end
REG_START, REG_END = find_registry()
REG = {}
for e in range(REG_START, REG_END + 32, 32):
    tag, desc, x, back = struct.unpack("<4Q", rd(e, 32))
    REG[tag] = (e, desc, x, back)
def hexdump(va, n, cols=8):
    b = rd(va, n)
    for off in range(0, n, cols * 4):
        row = b[off:off + cols * 4]
        print("%x +%03x: %s" % (g(va), off, " ".join("%08x" % v for v in struct.unpack("<%dI" % (len(row) // 4), row[:len(row) // 4 * 4]))))
if __name__ == "__main__" and (len(sys.argv) < 2 or sys.argv[1] not in ("rec", "stream", "kids")):
    print("registry %x..%x entries=%d" % (g(REG_START), g(REG_END), len(REG)))
    for a in sys.argv[1:]:
        tag = int(a, 16)
        if tag not in REG: print("%08x: not in registry" % tag); continue
        e, desc, x, back = REG[tag]
        xt = u64(x) if x else 0
        print("== %08x entry=%x desc=%x x=%x(tag %08x) back=%x" % (tag, g(e), g(desc), g(x), xt, g(back)))
        size = u32(desc)
        hexdump(desc, min(size + 0x10, 0x200))

KIND_NAMES = {0x101: "class", 0x105: "uint", 0x109: "u32", 0x003: "int", 0x002: "bool", 0x126: "k126"}
def record_header(va):
    """Returns (size, nameHash, parentTag, structSize, flags, attrs[(a,b)], hdr_va, fieldCount, tag)."""
    size = u32(va); nhash = u32(va + 8); parent = u32(va + 0x10); ssize = u32(va + 0x14); flags = u32(va + 0x18)
    alen = u32(va + 0x48)
    attrs = [struct.unpack("<2I", rd(va + 0x58 + 8 * i, 8)) for i in range((alen - 0x10) // 8)] if alen >= 0x10 else []
    hdr = va + 0x48 + alen
    nfields = u32(hdr); tag = u32(hdr + 8)
    return size, nhash, parent, ssize, flags, attrs, hdr, nfields, tag
def parse_fields(va):
    size, nhash, parent, ssize, flags, attrs, hdr, nfields, tag = record_header(va)
    fields = []
    p = hdr + 0x20
    for i in range(nfields):
        if p + 40 > va + size or u32(p + 12) != 0x3f000000: break
        off, off2, idx, _, kind, cls, flag, w, z1, z2 = struct.unpack("<10I", rd(p, 40))
        fields.append((idx, off, kind, cls, flag, w)); p += 40
    return fields
def parse_record(va, depth=0):
    size, nhash, parent, ssize, flags, attrs, hdr, nfields, tag = record_header(va)
    ind = "  " * depth
    print("%s[rec %x size=%x hash=%08x parent=%08x tag=%08x ssize=%x flags=%x nfields=%d attrs=%s]" % (
        ind, g(va), size, nhash, parent, tag, ssize, flags, nfields, ["%08x->%08x" % a for a in attrs]))
    for idx, off, kind, cls, flag, w in parse_fields(va):
        print("%s  field idx=%-2d off=+%03x kind=%04x(%s) cls=%08x flag=%d width=%d" % (ind, idx, off, kind, KIND_NAMES.get(kind, "?"), cls, flag, w))
if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "rec":
    for a in sys.argv[2:]:
        tag = int(a, 16)
        if tag in REG:
            e, desc, x, back = REG[tag]; print("== %08x desc=%x x->%08x" % (tag, g(desc), u64(x) if x else 0)); parse_record(desc)
        else:
            print("== %08x not in registry" % tag)

def walk_stream(start, max_bytes=0x2000000):
    """Walks consecutive records from `start`; returns list of (va, size, hash, tag)."""
    out = []; p = start
    while p < start + max_bytes:
        size = u32(p)
        if size < 0x10 or size > 0x100000 or size % 4: break
        tag = u32(p + 0x68) if size >= 0x6c else u32(p + 0x10)
        out.append((p, size, u32(p + 8), tag)); p += (size + 7) & ~7
    return out
if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "stream":
    lo = min(d for (_, d, _, _) in REG.values()); hi = max(d for (_, d, _, _) in REG.values())
    print("desc range %x..%x" % (g(lo), g(hi)))
    recs = walk_stream(lo)
    print("records from lo:", len(recs), "end %x" % g(recs[-1][0] + recs[-1][1]) if recs else "")
    # try to extend backwards: find a record ending exactly at lo by scanning candidate starts
    tags = {}
    for va, size, h, tag in recs: tags.setdefault(tag, []).append(va)
    print("distinct tags:", len(tags))
    import pickle; pickle.dump(recs, open("tools/reflect_stream.pkl", "wb"))

def load_all():
    import pickle, os
    recs = pickle.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "reflect_stream.pkl"), "rb"))
    by_tag, children = {}, {}
    for va, size, h, _ in recs:
        try:
            hdr = record_header(va)
        except Exception:
            continue
        size, nhash, parent, ssize, flags, attrs, hdrva, nfields, tag = hdr
        by_tag.setdefault(tag, []).append(va)
        children.setdefault(parent, []).append((tag, va))
    return recs, by_tag, children
if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "kids":
    recs, by_tag, children = load_all()
    for a in sys.argv[2:]:
        t = int(a, 16)
        print("== children of %08x: %d" % (t, len(children.get(t, []))))
        for tag, va in children.get(t, []):
            size, nhash, parent, ssize, flags, attrs, hdrva, nfields, tag2 = record_header(va)
            print("   %08x hash=%08x ssize=%x nfields=%d attrs=%s" % (tag, nhash, ssize, nfields, ["%08x->%08x" % x for x in attrs]))
