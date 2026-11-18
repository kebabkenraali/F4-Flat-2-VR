"""Writes fo4stereo.esp: one quest (start game enabled) with the Fo4StereoHud script attached.

Layout copied from records in Fallout4.esm (QUST with VMAD/DNAM). The plugin depends only on Fallout4.esm.
Usage: python make_hud_esp.py OUT.esp
"""
import struct, sys

def sub(typ, data):
    return typ + struct.pack('<H', len(data)) + data

def record(typ, flags, formid, body):
    return typ + struct.pack('<IIII', len(body), flags, formid, 0) + struct.pack('<HH', 131, 0) + body

def group_qust(records):
    body = b''.join(records)
    return b'GRUP' + struct.pack('<I', 24 + len(body)) + b'QUST' + struct.pack('<III', 0, 0, 0) + body

def make(out_path):
    tes4 = record(b'TES4', 0, 0, b''.join([
        sub(b'HEDR', struct.pack('<fII', 0.95, 1, 0x800)),   # version, record count, next object id
        sub(b'MAST', b'Fallout4.esm\x00'),
        sub(b'DATA', struct.pack('<Q', 0)),
    ]))
    edid = sub(b'EDID', b'Fo4StereoHudQuest\x00')
    script_name = b'Fo4StereoHud'
    vmad = struct.pack('<hhH', 6, 2, 1) + struct.pack('<H', len(script_name)) + script_name \
        + struct.pack('<BH', 0x01, 0) + struct.pack('<B', 0)
    dnam = struct.pack('<HBB', 0x0001, 0, 0) + b'\x00' * 8   # 0x0001 = start game enabled
    quest = record(b'QUST', 0, 0x01000800, sub(b'EDID', edid[6:]) + sub(b'VMAD', vmad) + sub(b'DNAM', dnam))
    data = tes4 + group_qust([quest])
    open(out_path, 'wb').write(data)
    return len(data)

if __name__ == '__main__':
    print('wrote', make(sys.argv[1]), 'bytes')
