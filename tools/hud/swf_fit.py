"""Fit the whole game HUD (stage 1280x720) into the frame box given by screen edges.

Changes only the root-level placement of the HUD sprite (scale + translate), so every element keeps its
relative layout. Uniform scale = min(boxW/1280, boxH/720); the HUD is centred in the box.
Usage: python swf_fit.py IN.swf OUT.swf LEFT RIGHT TOP BOTTOM   (edges in degrees)
"""
import sys, zlib, struct, math
from swf_layout import Bits, tags, parse_place2, walk
from swf_patch import encode_matrix_scaled

STAGE_W, STAGE_H = 1280.0, 720.0
TAN_H, TAN_V = 1.377, 1.428   # eye half-tangents from the headset (horizontal, vertical)

def box_from_edges(left, right, top, bottom):
    tl = math.tan(math.radians(left)); tr = math.tan(math.radians(right))
    tt = math.tan(math.radians(top)); tb = math.tan(math.radians(bottom))
    x0 = 0.5 + tl / (2 * TAN_H); x1 = 0.5 + tr / (2 * TAN_H)
    y0 = 0.5 - tt / (2 * TAN_V); y1 = 0.5 - tb / (2 * TAN_V)
    return x0, y0, x1, y1

def main():
    src, dst = sys.argv[1], sys.argv[2]
    left, right, top, bottom = map(float, sys.argv[3:7])
    x0, y0, x1, y1 = box_from_edges(left, right, top, bottom)
    bw, bh = (x1 - x0), (y1 - y0)
    # work in screen pixels of a 1280x720 reference
    bw_px, bh_px = bw * 1280.0, bh * 720.0
    s = min(bw_px / STAGE_W, bh_px / STAGE_H)
    cx, cy = (x0 + x1) / 2 * 1280.0, (y0 + y1) / 2 * 720.0
    tx = cx - s * STAGE_W / 2.0
    ty = cy - s * STAGE_H / 2.0
    raw = open(src, 'rb').read()
    data = raw[:8] + zlib.decompress(raw[8:])
    bits = Bits(data, 8); nb = bits.read(5); [bits.read(nb, True) for _ in range(4)]; bits.align()
    placements, names = [], {}
    walk(data, bits.bytepos() + 4, len(data), 0, placements, names)
    edits = []
    def scan(body, end, container=0):
        for code, pos, b, length in tags(data, body, end):
            if code == 39:
                sid, = struct.unpack_from('<H', data, b)
                scan(b + 4, b + length, sid)
            elif code == 26:
                info = parse_place2(data, b, length)
                if container == 0 and info['matrix'] and info['mpos'][0] is not None:
                    m = info['matrix']
                    # compose: new = fit(old); HUD px = tx + s * old px (no rotation in the root placements)
                    new_sx = s * m[0] / 65536.0; new_sy = s * m[1] / 65536.0
                    new_tx = tx + s * (m[4] / 20.0); new_ty = ty + s * (m[5] / 20.0)
                    new_m = encode_matrix_scaled(new_sx, new_sy, round(new_tx * 20), round(new_ty * 20))
                    body_old = data[b:b + length]
                    ms, me = info['mpos']
                    new_body = body_old[:ms - b] + new_m + body_old[me - b:]
                    tag = struct.pack('<H', (code << 6) | (len(new_body) if len(new_body) < 0x3F else 0x3F))
                    if len(new_body) >= 0x3F: tag += struct.pack('<I', len(new_body))
                    edits.append((pos, b + length, tag + new_body))
    scan(bits.bytepos() + 4, len(data), 0)
    out = bytearray(); last = 0
    for st, en, new in sorted(edits):
        out += data[last:st] + new; last = en
    out += data[last:]
    res = bytes(out)
    open(dst, 'wb').write(b'CWS' + res[3:8] + zlib.compress(res[8:], 9))
    print(f'scale={s:.3f} offset=({tx:.1f},{ty:.1f})px root-placements={len(edits)} box=({x0:.3f},{y0:.3f})-({x1:.3f},{y1:.3f})')

if __name__ == '__main__':
    main()
