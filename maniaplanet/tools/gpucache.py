"""Extract DXBC blobs from ManiaPlanet's GpuCache zips (GBX, LZO1X-compressed body)."""
import struct
import sys
import zipfile


def lzo1x_decompress(src, out_len):
    dst = bytearray()
    ip = 0
    n = len(src)

    def length_ext(base, mask):
        nonlocal ip
        t = src[ip] & mask
        ip += 1
        if t == 0:
            while src[ip] == 0:
                t += 255
                ip += 1
            t += base + src[ip]
            ip += 1
        return t

    def copy_match(dist, length):
        start = len(dst) - dist
        if start < 0:
            raise ValueError("lookbehind overrun")
        for k in range(length):
            dst.append(dst[start + k])

    # first byte special case
    state = 0
    if src[ip] > 17:
        t = src[ip] - 17
        ip += 1
        dst += src[ip:ip + t]
        ip += t
        state = 4 if t >= 4 else t
    while ip < n:
        t = src[ip]
        if t < 16:
            if state == 0:
                # literal run
                if t == 0:
                    ip += 1
                    while src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 15 + src[ip]
                ip += 1
                t += 3
                dst += src[ip:ip + t]
                ip += t
                state = 4
                continue
            elif state < 4:
                # 2-byte match, dist <= 1024
                ip += 1
                dist = (t >> 2) + (src[ip] << 2) + 1
                ip += 1
                copy_match(dist, 2)
                s = t & 3
            else:
                # 3-byte match, dist 2049..3072
                ip += 1
                dist = (t >> 2) + (src[ip] << 2) + 2049
                ip += 1
                copy_match(dist, 3)
                s = t & 3
        elif t >= 64:
            length = (t >> 5) + 1
            ip += 1
            dist = ((t >> 2) & 7) + (src[ip] << 3) + 1
            ip += 1
            copy_match(dist, length)
            s = t & 3
        elif t >= 32:
            length = length_ext(31, 31) + 2
            ds = struct.unpack_from('<H', src, ip)[0]
            ip += 2
            dist = (ds >> 2) + 1
            copy_match(dist, length)
            s = ds & 3
        else:  # 16..31
            dist_hi = (t & 8) << 11
            length = length_ext(7, 7) + 2
            ds = struct.unpack_from('<H', src, ip)[0]
            ip += 2
            dist = dist_hi + (ds >> 2)
            if dist == 0:
                break  # end of stream
            dist += 16384
            copy_match(dist, length)
            s = ds & 3
        if s:
            dst += src[ip:ip + s]
            ip += s
        state = s
    if len(dst) != out_len:
        raise ValueError(f"size {len(dst)} != {out_len}")
    return bytes(dst)


def gbx_body(d):
    assert d[:3] == b'GBX'
    o = 3
    ver = struct.unpack_from('<H', d, o)[0]; o += 2
    fmt = d[o:o + 4]; o += 4
    o += 4  # class id
    user = struct.unpack_from('<I', d, o)[0]; o += 4
    o += user
    o += 4  # node count
    ext = struct.unpack_from('<I', d, o)[0]; o += 4
    if ext:
        raise ValueError("external refs")
    if fmt[2:3] == b'C':
        ulen, clen = struct.unpack_from('<II', d, o); o += 8
        return lzo1x_decompress(d[o:o + clen], ulen)
    return d[o:]


def dxbc_blobs(body):
    out = []
    i = body.find(b'DXBC')
    while i >= 0:
        size = struct.unpack_from('<I', body, i + 24)[0]
        if i + size <= len(body):
            out.append(body[i:i + size])
            i = body.find(b'DXBC', i + size)
        else:
            i = body.find(b'DXBC', i + 4)
    return out


def shader_kind(blob):
    # SHEX/SHDR chunk: version token, program type in bits 16..31
    nch = struct.unpack_from('<I', blob, 28)[0]
    for k in range(nch):
        off = struct.unpack_from('<I', blob, 32 + 4 * k)[0]
        tag = blob[off:off + 4]
        if tag in (b'SHEX', b'SHDR'):
            tok = struct.unpack_from('<I', blob, off + 8)[0]
            return {0: 'ps', 1: 'vs', 2: 'gs', 3: 'hs', 4: 'ds', 5: 'cs'}.get(tok >> 16, '?')
    return '?'


def dump(zip_path, needle, out_file, checksum=''):
    """Writes the first pixel shader of the first entry whose name contains needle (for tests);
    with checksum, the variant whose DXBC checksum (hex) starts with it (a name has many)."""
    zf = zipfile.ZipFile(zip_path)
    for name in zf.namelist():
        if needle.lower() in name.lower():
            for b in dxbc_blobs(gbx_body(zf.read(name))):
                if shader_kind(b) == 'ps' and b[4:20].hex().startswith(checksum.lower()):
                    open(out_file, 'wb').write(b)
                    print(f"{name} -> {out_file} ({len(b)} bytes)")
                    return
    raise SystemExit(f"no pixel shader matching {needle}")


if __name__ == '__main__':
    if sys.argv[1] == '--dump':
        dump(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5] if len(sys.argv) > 5 else '')
        sys.exit(0)
    zips = sys.argv[1:]
    for z in zips:
        zf = zipfile.ZipFile(z)
        for name in zf.namelist():
            if not name.endswith('.Gbx'):
                continue
            try:
                body = gbx_body(zf.read(name))
            except Exception as e:
                print(f"ERR\t{name}\t{e}")
                continue
            for b in dxbc_blobs(body):
                print(f"{b[4:20].hex()}\t{shader_kind(b)}\t{len(b)}\t{name}")
