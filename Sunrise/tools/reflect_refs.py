import sys, struct
sys.argv = ["x"]
from reflect_dump import *
from collections import Counter
recs, by_tag, children = load_all()
targets = [int(a, 16) for a in (sys.orig_argv[2:] if hasattr(sys, "orig_argv") else [])] or [0x80807f71, 0x80807f73, 0x80809ed8, 0x808060d2]
hits = []
for va, size, h, _ in recs:
    b = rd(va, size)
    for t in targets:
        pat = struct.pack("<I", t); i = b.find(pat)
        while i >= 0:
            hits.append((t, va, i)); i = b.find(pat, i + 4)
print(Counter("%08x" % t for t, _, _ in hits))
for t, va, i in hits[:80]:
    size, nhash, parent, ssize, flags, attrs, hdrva, nfields, tag = record_header(va)
    print("%08x in rec %x (+%x) tag=%08x hash=%08x ssize=%x nfields=%d attrs=%s" % (t, g(va), i, tag, nhash, ssize, nfields, ["%08x->%08x" % a for a in attrs]))
