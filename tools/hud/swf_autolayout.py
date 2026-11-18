"""Derive HUD panel placement from the game's own HUDMenu.swf (stage 1280x720).

For each HUD element we take the absolute position of its first named instance (summing parent
translations up to the stage root) and build a crop box around it. Output: ini lines for the
[vr] section (hud_<name>_crop, _yaw_deg, _pitch_deg) that the proxy reads. Sizes are estimates;
the user fine-tunes them in MCM / Numpad tuning.

Usage: python swf_autolayout.py HUDMenu.swf [tanX tanY]
"""
import sys, zlib, math
from swf_layout import Bits, tags, parse_place2, walk

STAGE_W, STAGE_H = 1280.0, 720.0
# element -> (instance name, estimated width, height, anchor offset x/y of the box relative to the anchor)
TARGETS = {
    'hp':       ('HPMeter_mc',               260, 40, 0, 0),
    'ammo':     ('AmmoCount_mc',             160, 60, 0, 0),
    'compass':  ('CompassWidget_mc',         420, 40, 0, 0),
    'effects':  ('HUDActiveEffectsWidget_mc', 200, 80, 0, 0),
    'location': ('LocationText_mc',          320, 40, 0, 0),
    'notify':   ('HUDNotificationsGroup_mc', 420, 160, 0, 0),
    'prompt':   ('ButtonHintBar_mc',         360, 80, 0, 0),
}

def load(path):
    raw = open(path, 'rb').read()
    data = raw[:8] + zlib.decompress(raw[8:])
    bits = Bits(data, 8); nb = bits.read(5); [bits.read(nb, True) for _ in range(4)]; bits.align()
    out, names = [], {}
    walk(data, bits.bytepos() + 4, len(data), 0, out, names)
    return out, names

def absolute_positions(placements, names):
    """Return {instance_name: (x, y)} in stage pixels, summing translations along the parent chain."""
    by_char = {}   # sprite id -> list of placements that put it on the stage
    for p in placements:
        if p['char'] is not None and p['matrix']:
            by_char.setdefault(p['char'], []).append(p)
    cache = {}
    def origin(container, depth=0):
        if container == 0 or depth > 20:
            return (0.0, 0.0)
        if container in cache:
            return cache[container]
        best = (0.0, 0.0)
        for p in by_char.get(container, []):
            m = p['matrix']
            ox, oy = origin(p['container'], depth + 1)
            best = (ox + m[4] / 20.0, oy + m[5] / 20.0)
            break
        cache[container] = best
        return best
    result = {}
    for p in placements:
        if p['name'] and p['matrix'] and p['name'] not in result:
            ox, oy = origin(p['container'])
            m = p['matrix']
            result[p['name']] = (ox + m[4] / 20.0, oy + m[5] / 20.0)
    return result

if __name__ == '__main__':
    path = sys.argv[1]
    tanx = float(sys.argv[2]) if len(sys.argv) > 2 else 1.377
    tany = float(sys.argv[3]) if len(sys.argv) > 3 else 1.428
    placements, names = load(path)
    pos = absolute_positions(placements, names)
    for key, (inst, w, h, dx, dy) in TARGETS.items():
        if inst not in pos:
            print(f'; {key}: instance {inst} not found'); continue
        x, y = pos[inst]
        x += dx; y += dy
        u0 = max(0.0, (x - w / 2) / STAGE_W); u1 = min(1.0, (x + w / 2) / STAGE_W)
        v0 = max(0.0, (y - h / 2) / STAGE_H); v1 = min(1.0, (y + h / 2) / STAGE_H)
        cu = (u0 + u1) / 2; cv = (v0 + v1) / 2
        yaw = math.degrees(math.atan((cu * 2 - 1) * tanx))
        pitch = math.degrees(math.atan((1 - cv * 2) * tany))
        print(f'; {key}: {inst} at ({x:.0f},{y:.0f})')
        print(f'hud_{key}_crop={u0:.3f},{v0:.3f},{u1:.3f},{v1:.3f}')
        print(f'hud_{key}_yaw_deg={yaw:.1f}')
        print(f'hud_{key}_pitch_deg={pitch:.1f}')

