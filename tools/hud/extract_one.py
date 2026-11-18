import struct, zlib, sys
src, want, out = sys.argv[1], sys.argv[2], sys.argv[3]
with open(src, 'rb') as f:
    hdr = f.read(24)
    magic, ver, typ, count, name_off = struct.unpack('<4sI4sIQ', hdr)
    assert magic == b'BTDX', magic
    if typ != b'GNRL':
        sys.exit('not GNRL: %r' % typ)
    f.seek(24)
    recs = [struct.unpack('<IIIIQIII', f.read(36)) for _ in range(count)]
    f.seek(name_off)
    names = []
    for _ in range(count):
        n, = struct.unpack('<H', f.read(2))
        names.append(f.read(n).decode('utf-8', 'replace'))
    for name, r in zip(names, recs):
        if name.lower().endswith(want.lower()):
            _, _, _, _, off, packed, unpacked, _ = r
            f.seek(off)
            data = f.read(packed) if packed else f.read(unpacked)
            if packed:
                data = zlib.decompress(data)
            open(out, 'wb').write(data)
            print('extracted', name, len(data), 'bytes')
            break
    else:
        print('not found:', want, '| total files:', count)
