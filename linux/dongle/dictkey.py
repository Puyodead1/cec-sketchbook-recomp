"""Recover the dict.db string key (16 bytes) via known plaintext, then dump decrypted dictionary.
Cipher (sub_8083F13, per 16-byte block, NUL-terminated strings):
    v = c; if v != k[i]: v ^= k[i]; p = ((v>>2)&0x33) | ((v<<2)&0xCC)
"""
import struct, sys, os, collections

path = sys.argv[1]
d = open(path, "rb").read()
nsec = struct.unpack_from("<H", d, 14)[0]
secs = []
for s in range(nsec):
    sid, _, off, _ = struct.unpack_from("<HHII", d, 16 + 12 * s)
    secs.append((sid, off))

def swp(v):
    return ((v >> 2) & 0x33) | ((v << 2) & 0xCC)

def cstr(b, o):
    e = b.index(b"\0", o)
    return b[o:e]

entries = {}  # (sec, id) -> list of encrypted strings
for sid, so in secs:
    cnt = struct.unpack_from("<H", d, so + 2)[0]
    name = d[so + 4:so + 16].rstrip(b"\0")
    for i in range(cnt):
        eid, size, eoff = struct.unpack_from("<HHI", d, so + 16 + 8 * i)
        blob = d[so + eoff: so + eoff + size]
        n = struct.unpack_from("<h", blob, 2)[0]
        strs = []
        p1, p2 = struct.unpack_from("<II", blob, 4)
        if p1: strs.append(cstr(blob, p1))
        if p2: strs.append(cstr(blob, p2))
        for r in range(1, n):
            ps = struct.unpack_from("<I", blob, 12 * r + 8)[0]
            if ps: strs.append(cstr(blob, ps))
        entries[(sid, eid)] = strs
    print("section", sid, name, cnt, "entries", file=sys.stderr)

def dec(c, key):
    out = bytearray()
    for i, ch in enumerate(c):
        k = key[i % 16]
        if k is None:
            out.append(ord("?")); continue
        v = ch if ch == k else ch ^ k
        out.append(swp(v))
    return bytes(out)

# known plaintext: entry 294 field at +8 == "camera.wav"
key = [None] * 16
known = []
sec = secs[0][0]
cam = entries[(sec, 294)]
print("entry 294 raw:", cam, file=sys.stderr)

def learn(c, p):
    for i, (cc, pp) in enumerate(zip(c, p)):
        kk = cc ^ swp(pp)
        j = i % 16
        if key[j] is None:
            key[j] = kk
        elif key[j] != kk and cc != key[j]:
            print("conflict at", j, hex(key[j]), hex(kk), file=sys.stderr)

if __name__ == "__main__":
    plain = sys.argv[2:] if len(sys.argv) > 2 else []
    # which string of entry 294 is "+8"? it's p2 if present else p1; try the 10-char one
    for c in cam:
        if len(c) == len("camera.wav"):
            learn(c, b"camera.wav")
    for kp in plain:          # extra "sec:id:idx=plaintext" hints
        loc, txt = kp.split("=", 1)
        s_, i_, x_ = map(int, loc.split(":"))
        learn(entries[(s_, i_)][x_], txt.encode("latin1"))
    print("key:", " ".join("??" if k is None else "%02x" % k for k in key))
    for (s, i), strs in sorted(entries.items()):
        print(s, i, [dec(c, key).decode("latin1") for c in strs])
