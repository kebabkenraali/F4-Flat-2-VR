import zlib, struct, sys

class Bits:
    def __init__(self, b, pos=0):
        self.b, self.bitpos = b, pos * 8
    def read(self, n, signed=False):
        v = 0
        for _ in range(n):
            byte = self.b[self.bitpos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.bitpos & 7))) & 1)
            self.bitpos += 1
        if signed and n and (v >> (n - 1)) & 1:
            v -= 1 << n
        return v
    def align(self):
        self.bitpos = (self.bitpos + 7) & ~7
    def bytepos(self):
        return self.bitpos >> 3

def read_matrix(bits):
    """Returns (sx, sy, rs0, rs1, tx, ty, start_bit, end_bit) with tx/ty in twips."""
    start = bits.bitpos
    sx, sy, r0, r1 = 65536, 65536, 0, 0
    if bits.read(1):
        n = bits.read(5); sx = bits.read(n, True); sy = bits.read(n, True)
    if bits.read(1):
        n = bits.read(5); r0 = bits.read(n, True); r1 = bits.read(n, True)
    n = bits.read(5)
    tx = bits.read(n, True) if n else 0
    ty = bits.read(n, True) if n else 0
    bits.align()
    return sx, sy, r0, r1, tx, ty, start, bits.bitpos

def skip_cxform_alpha(bits):
    has_add = bits.read(1); has_mult = bits.read(1)
    n = bits.read(4)
    if has_mult: bits.read(4 * n) if False else [bits.read(n) for _ in range(4)]
    if has_add: [bits.read(n) for _ in range(4)]
    bits.align()

def tags(data, pos, end):
    while pos < end:
        code_len, = struct.unpack_from('<H', data, pos)
        code, length = code_len >> 6, code_len & 0x3F
        hdr = 2
        if length == 0x3F:
            length, = struct.unpack_from('<I', data, pos + 2); hdr = 6
        body = pos + hdr
        yield code, pos, body, length
        pos = body + length

def parse_place2(data, body, length):
    flags = data[body]
    p = body + 1
    depth, = struct.unpack_from('<H', data, p); p += 2
    char = None
    if flags & 0x02:
        char, = struct.unpack_from('<H', data, p); p += 2
    matrix = None
    mstart = mend = None
    if flags & 0x04:
        bits = Bits(data, p)
        matrix = read_matrix(bits)
        mstart, mend = matrix[6] // 8, (matrix[7] + 7) // 8
        p = bits.bytepos()
    if flags & 0x08:
        bits = Bits(data, p); skip_cxform_alpha(bits); p = bits.bytepos()
    if flags & 0x10:
        p += 2
    name = None
    if flags & 0x20:
        e = data.index(b'\x00', p)
        name = data[p:e].decode('latin1'); p = e + 1
    return dict(flags=flags, depth=depth, char=char, name=name, matrix=matrix, mpos=(mstart, mend), body=body)

def walk(data, body, end, container, out, sprite_names):
    for code, pos, b, length in tags(data, body, end):
        if code == 39:  # DefineSprite
            sid, = struct.unpack_from('<H', data, b)
            walk(data, b + 4, b + length, sid, out, sprite_names)
        elif code == 26:  # PlaceObject2
            info = parse_place2(data, b, length)
            info['container'] = container
            info['tag_pos'] = (pos, b, length)
            out.append(info)
        elif code == 76:  # SymbolClass
            n, = struct.unpack_from('<H', data, b); p = b + 2
            for _ in range(n):
                cid, = struct.unpack_from('<H', data, p); p += 2
                e = data.index(b'\x00', p); sprite_names[cid] = data[p:e].decode('latin1'); p = e + 1

if __name__ == '__main__':
    raw = open(sys.argv[1], 'rb').read()
    assert raw[:3] == b'CWS', raw[:3]
    data = raw[:8] + zlib.decompress(raw[8:])
    bits = Bits(data, 8)
    nb = bits.read(5); xmin, xmax, ymin, ymax = [bits.read(nb, True) for _ in range(4)]
    bits.align()
    fps_p = bits.bytepos() + 2
    print('stage px: %.0f x %.0f' % ((xmax - xmin) / 20, (ymax - ymin) / 20))
    out, names = [], {}
    walk(data, fps_p + 2, len(data), 0, out, names)
    inv = {v: k for k, v in names.items()}
    rows = []
    for info in out:
        if not info['name'] or not info['matrix']:
            continue
        sx, sy, r0, r1, tx, ty, *_ = info['matrix']
        rows.append((info['name'], names.get(info['container'], 'root' if info['container'] == 0 else str(info['container'])),
                     tx / 20.0, ty / 20.0, sx / 65536, sy / 65536, info['depth']))
    for r in sorted(rows, key=lambda r: r[0]):
        print('%-30s parent=%-22s x=%8.1f y=%8.1f scale=%.2f/%.2f depth=%d' % r)
