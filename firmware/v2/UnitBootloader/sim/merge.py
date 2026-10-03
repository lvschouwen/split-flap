#!/usr/bin/env python3
"""Merge app + fielded twiboot hex into one image for simavr."""
import sys


def read_ihex(p):
    data = {}
    base = 0
    for line in open(p):
        line = line.strip()
        if not line.startswith(":"):
            continue
        b = bytes.fromhex(line[1:])
        n, addr, typ = b[0], (b[1] << 8) | b[2], b[3]
        if typ == 4:
            base = ((b[4] << 8) | b[5]) << 16
        elif typ == 2:
            base = ((b[4] << 8) | b[5]) << 4
        elif typ == 0:
            for i in range(n):
                data[base + addr + i] = b[4 + i]
    return data


def emit(path, data):
    out = []
    addrs = sorted(data)
    i = 0
    while i < len(addrs):
        start = addrs[i]
        chunk = [data[start]]
        j = i + 1
        while j < len(addrs) and addrs[j] == addrs[j - 1] + 1 and len(chunk) < 16:
            chunk.append(data[addrs[j]])
            j += 1
        rec = [len(chunk), (start >> 8) & 0xFF, start & 0xFF, 0] + chunk
        csum = (-sum(rec)) & 0xFF
        out.append(":" + bytes(rec + [csum]).hex())
        i = j
    out.append(":00000001FF")
    open(path, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    out_path, app_path, boot_path = sys.argv[1], sys.argv[2], sys.argv[3]
    d = {}
    d.update(read_ihex(app_path))
    d.update(read_ihex(boot_path))
    emit(out_path, d)
    print(f"merged {len(d)} bytes, span {min(d):#06x}..{max(d):#06x}")
