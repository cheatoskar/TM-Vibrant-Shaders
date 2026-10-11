"""Prints the constant buffers and resource bindings (RDEF chunk) of ManiaPlanet's cached shaders.

    python -I tools/rdef.py "<ManiaPlanet>/Packs/GpuCache_D3D11_SM5.zip" <name substring> [...]

Used to find where the engine keeps its camera (WorldToCamera, projection, previous frame) and which
textures a pass reads, so the mod can read them at run time.
"""
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gpucache import dxbc_blobs, gbx_body, shader_kind  # noqa: E402

RES_TYPES = {0: 'cbuffer', 1: 'tbuffer', 2: 'texture', 3: 'sampler', 4: 'uav_rw', 5: 'structured', 6: 'uav_rw_structured',
             7: 'byteaddress', 8: 'uav_rw_byteaddress', 9: 'uav_append', 10: 'uav_consume', 11: 'uav_counter'}


def cstr(data, off):
    end = data.index(b'\0', off)
    return data[off:end].decode('latin1')


def chunk(blob, tag):
    n = struct.unpack_from('<I', blob, 28)[0]
    for k in range(n):
        off = struct.unpack_from('<I', blob, 32 + 4 * k)[0]
        if blob[off:off + 4] == tag:
            size = struct.unpack_from('<I', blob, off + 4)[0]
            return blob[off + 8:off + 8 + size]
    return None


def rdef(blob):
    d = chunk(blob, b'RDEF')
    cb_count, cb_off, rb_count, rb_off, target, flags, creator = struct.unpack_from('<7I', d, 0)
    sm5 = (target & 0xffff) >= 0x500
    out = {'resources': [], 'cbuffers': []}
    for i in range(rb_count):
        name, typ, ret, dim, samples, bind, count, fl = struct.unpack_from('<8I', d, rb_off + 32 * i)
        out['resources'].append((RES_TYPES.get(typ, typ), cstr(d, name), bind, count))
    vsize = 40 if sm5 else 24
    for i in range(cb_count):
        name, var_count, var_off, size, fl, typ = struct.unpack_from('<6I', d, cb_off + 24 * i)
        variables = []
        for v in range(var_count):
            vname, start, vs, vflags = struct.unpack_from('<4I', d, var_off + vsize * v)
            variables.append((start, vs, cstr(d, vname), bool(vflags & 2)))
        out['cbuffers'].append((cstr(d, name), size, variables))
    return out


def main():
    zf = zipfile.ZipFile(sys.argv[1])
    needles = [n.lower() for n in sys.argv[2:]]
    for entry in zf.namelist():
        if not any(n in entry.lower() for n in needles):
            continue
        for blob in dxbc_blobs(gbx_body(zf.read(entry))):
            kind = shader_kind(blob)
            info = rdef(blob)
            print(f"== {entry} [{kind}] {blob[4:12].hex()}")
            for typ, name, bind, count in info['resources']:
                print(f"   {typ:10s} {name} @{bind}" + (f" x{count}" if count > 1 else ''))
            for name, size, variables in info['cbuffers']:
                used = [v for v in variables if v[3]]
                print(f"   cbuffer {name} ({size} bytes): " +
                      ', '.join(f"{n}@{s}[{vs}]" for s, vs, n, u in used))
            break  # the first variant is enough to see the layout


if __name__ == '__main__':
    main()
