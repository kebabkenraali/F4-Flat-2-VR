"""Move named HUD elements in a Fallout 4 HUDMenu.swf by (dx, dy) pixels.

Usage: python -I swf_patch.py IN.swf OUT.swf "Name:dx:dy" ["Name:dx:dy" ...]
Only the translation of the matching PlaceObject2 tags changes; scale/rotation and everything else are kept.
Reads the user's own extracted file; never writes into the game folder.
"""
import sys, zlib, struct
from swf_layout import Bits, tags, parse_place2, walk

class BitsW:
    def __init__(self): self.out = []; self.n = 0
    def put(self, v, nbits):
        for i in range(nbits - 1, -1, -1):
            self.out.append((v >> i) & 1); self.n += 1
    def bytes(self):
        bits = self.out + [0] * ((8 - len(self.out) % 8) % 8)
        return bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))

def nbits_signed(v):
    if v == 0: return 0
    n = 1
    while not (-(1 << (n - 1)) <= v < (1 << (n - 1))): n += 1
    return n

def encode_matrix(orig, tx, ty):
    sx, sy, r0, r1 = orig[0], orig[1], orig[2], orig[3]
    w = BitsW()
    has_scale = not (sx == 65536 and sy == 65536)
    w.put(1 if has_scale else 0, 1)
    if has_scale:
        n = max(nbits_signed(sx), nbits_signed(sy)); w.put(n, 5); w.put(sx & ((1 << n) - 1), n); w.put(sy & ((1 << n) - 1), n)
    has_rot = not (r0 == 0 and r1 == 0)
    w.put(1 if has_rot else 0, 1)
    if has_rot:
        n = max(nbits_signed(r0), nbits_signed(r1)); w.put(n, 5); w.put(r0 & ((1 << n) - 1), n); w.put(r1 & ((1 << n) - 1), n)
    n = max(nbits_signed(tx), nbits_signed(ty))
    w.put(n, 5)
    if n:
        w.put(tx & ((1 << n) - 1), n); w.put(ty & ((1 << n) - 1), n)
    return w.bytes()

def patch(data, targets):
    """targets: {name: (dx_twips, dy_twips)}. Returns new decompressed data and list of changed names."""
    changed = []
    edits = []  # (tag_start, tag_end, new_bytes)
    def scan(body, end, container):
        for code, pos, b, length in tags(data, body, end):
            if code == 39:
                sid, = struct.unpack_from('<H', data, b)
                scan(b + 4, b + length, sid)
            elif code == 26:
                info = parse_place2(data, b, length)
                if info['name'] in targets and info['matrix'] and info['mpos'][0] is not None:
                    dx, dy = targets[info['name']]
                    m = info['matrix']
                    ms, me = info['mpos']
                    tx, ty = m[4] + dx, m[5] + dy
                    new_m = encode_matrix(m, tx, ty)
                    body_old = data[b:b + length]
                    head = body_old[:ms - b]
                    tail = body_old[me - b:]
                    new_body = head + new_m + tail
                    new_tag = struct.pack('<H', (code << 6) | (len(new_body) if len(new_body) < 0x3F else 0x3F))
                    if len(new_body) >= 0x3F:
                        new_tag += struct.pack('<I', len(new_body))
                    edits.append((pos, b + length, new_tag + new_body))
                    changed.append(info['name'])
    scan(None or 0, len(data), 0) if False else None
    # walk from the first tag after the frame header
    bits = Bits(data, 8); nb = bits.read(5); [bits.read(nb, True) for _ in range(4)]; bits.align()
    start = bits.bytepos() + 4
    scan(start, len(data), 0)
    out = bytearray(); last = 0
    for s, e, new in sorted(edits):
        out += data[last:s] + new; last = e
    out += data[last:]
    return bytes(out), changed

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    targets = {}
    for spec in sys.argv[3:]:
        name, dx, dy = spec.split(':')
        targets[name] = (round(float(dx) * 20), round(float(dy) * 20))
    raw = open(src, 'rb').read()
    assert raw[:3] == b'CWS'
    data = raw[:8] + zlib.decompress(raw[8:])
    new_data, changed = patch(data, targets)
    body = new_data[8:]
    struct.pack_into('<I', bytearray(new_data), 4, len(new_data))
    out = b'CWS' + new_data[3:8] + zlib.compress(new_data[8:], 9)
    open(dst, 'wb').write(out)
    print('moved:', sorted(set(changed)), '| out size', len(out))

def encode_matrix_scaled(sx, sy, tx, ty):
    """Matrix with scale (float factors) and translation (twips). No rotation."""
    w = BitsW()
    sxi, syi = int(round(sx * 65536)), int(round(sy * 65536))
    w.put(1, 1)
    n = max(nbits_signed(sxi), nbits_signed(syi)); w.put(n, 5)
    w.put(sxi & ((1 << n) - 1), n); w.put(syi & ((1 << n) - 1), n)
    w.put(0, 1)
    n = max(nbits_signed(tx), nbits_signed(ty)); w.put(n, 5)
    if n:
        w.put(tx & ((1 << n) - 1), n); w.put(ty & ((1 << n) - 1), n)
    return w.bytes()
