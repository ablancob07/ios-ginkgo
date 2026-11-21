#!/usr/bin/env python3
"""Split the DTBs appended to a legacy (header v0) Image.gz-dtb boot.img.

ginkgo's stock boot.img is a gzip'd Image followed by several concatenated DTBs
(one per board variant). ABL picks the one matching msm-id/board-id, so our shim
image must carry the same DTBs.

usage: ginkgo-extract-dtbs.py backup/boot.img outdir
writes outdir/stock_N.dtb and outdir/all.dtb (concatenation, original order)
"""
import re
import struct
import sys
import os

img, out = sys.argv[1], sys.argv[2]
d = open(img, 'rb').read()
page = struct.unpack('<I', d[36:40])[0]
ksize = struct.unpack('<I', d[8:12])[0]
k = d[page:page + ksize]
os.makedirs(out, exist_ok=True)
offs = [m.start() for m in re.finditer(b'\xd0\x0d\xfe\xed', k)]
dtbs = []
for o in offs:
    total = struct.unpack('>I', k[o + 4:o + 8])[0]
    # a real FDT header: version 17, sane size, chained to the next one
    if 0x1000 < total < 0x200000 and struct.unpack('>I', k[o + 20:o + 24])[0] in (16, 17):
        dtbs.append(k[o:o + total])
for i, b in enumerate(dtbs):
    open(os.path.join(out, 'stock_%d.dtb' % i), 'wb').write(b)
open(os.path.join(out, 'all.dtb'), 'wb').write(b''.join(dtbs))
print('%d dtbs, sizes %s' % (len(dtbs), [len(b) for b in dtbs]))
