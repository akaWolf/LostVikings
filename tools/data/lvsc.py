#!/usr/bin/env python3
"""lvsc — the .lvsf compiler + DATA.DAT packer (authoring pipeline).

build FILE.lvsf CHUNK_HEX      compile free-form text, verify against the
                               extracted chunk (reports identical/changed)
pack [CID_HEX=FILE.lvsf ...] -o OUT.DAT
                               rebuild DATA.DAT from assets_raw/chunks/comp
                               blocks, replacing the given chunks with
                               store-mode LZSS of the compiled text.
                               With no replacements the output must be
                               byte-identical to the original (verified).

Store-mode LZSS: every flag byte 0xFF (8 literals) — a valid stream for
the original decompressor (sub_10982), ~12.5% larger than raw. Real
compression is not needed for authoring; the engine only ever inflates.
"""
import sys, os, struct, importlib.util, json

spec = importlib.util.spec_from_file_location('lf', 'tools/data/lvs_full.py')
lf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lf)
spec2 = importlib.util.spec_from_file_location('xd', 'tools/data/extract_datadat.py')
xd = importlib.util.module_from_spec(spec2)
spec2.loader.exec_module(xd)

def lzss_store(payload: bytes) -> bytes:
    """All-literal LZSS stream (flag 0xFF per 8 bytes)."""
    out = bytearray()
    for i in range(0, len(payload), 8):
        out.append(0xFF)
        out += payload[i:i+8]
    return bytes(out)

def lzss_compress(payload: bytes) -> bytes:
    """Greedy LZSS matching the sub_10982 decompressor: 4096-byte
    zero-initialized ring, absolute 12-bit ring offsets, 4-bit len-3
    (3..18). References may overlap the write cursor (byte-at-a-time
    copy semantics) and may point at the untouched zero region."""
    n = len(payload)
    ring = bytearray(4096)
    bx = 0
    # hash chains over ring positions by the 3-byte group they started
    from collections import defaultdict, deque
    chains = defaultdict(deque)   # (b0,b1,b2) -> ring positions (recent first)
    out = bytearray()
    i = 0
    flag_pos = None
    flag_bit = 8
    def put(bit):
        nonlocal flag_pos, flag_bit
        if flag_bit == 8:
            flag_pos = len(out)
            out.append(0)
            flag_bit = 0
        if bit:
            out[flag_pos] |= (1 << flag_bit)
        flag_bit += 1
    def push_byte(v):
        nonlocal bx
        ring[bx] = v
        bx = (bx + 1) & 0xFFF
    def match_len(off, limit):
        # simulate the decompressor copy: reads mutate nothing here, but
        # the copy in the decoder writes as it reads — model with a
        # virtual ring snapshot advanced byte by byte.
        l = 0
        o = off
        vb = bx
        tmp = {}
        while l < limit:
            v = tmp.get(o, ring[o])
            if v != payload[i + l]:
                break
            tmp[vb & 0xFFF] = v
            vb += 1
            o = (o + 1) & 0xFFF
            l += 1
        return l
    while i < n:
        limit = min(18, n - i)
        best_len = 0
        best_off = 0
        if limit >= 3:
            key = bytes(payload[i:i+3])
            tried = 0
            for off in chains.get(key, ()):
                l = match_len(off, limit)
                if l > best_len:
                    best_len, best_off = l, off
                    if l == limit:
                        break
                tried += 1
                if tried >= 64:
                    break
            # zero-region shortcut: a run of zeros can reference any zero
            # spot of the ring (position bx works: it is about to be
            # overwritten byte-at-a-time by the copy itself = zero fill
            # only if ring[bx] is still zero)
            if best_len < 3 and payload[i] == 0:
                zl = 0
                while zl < limit and payload[i + zl] == 0:
                    zl += 1
                if zl >= 3:
                    o = ring.find(b'\x00\x00\x00')
                    if o >= 0 and o + zl <= 4096 and all(
                            ring[o + k] == 0 for k in range(min(zl, 4096 - o))):
                        best_len = min(zl, 4096 - o)
                        best_off = o
        if best_len >= 3:
            put(0)
            out += struct.pack('<H', (best_off & 0xFFF) | ((best_len - 3) << 12))
            for k in range(best_len):
                key = bytes(payload[i + k:i + k + 3])
                if len(key) == 3:
                    chains[key].appendleft(bx)
                    if len(chains[key]) > 128:
                        chains[key].pop()
                push_byte(payload[i + k])
            i += best_len
        else:
            put(1)
            out.append(payload[i])
            key = bytes(payload[i:i+3])
            if len(key) == 3:
                chains[key].appendleft(bx)
                if len(chains[key]) > 128:
                    chains[key].pop()
            push_byte(payload[i])
            i += 1
    return bytes(out)

def compile_file(path):
    return lf.compile_free(open(path).read())

def cmd_build(path, cid):
    img = compile_file(path)
    orig = open(f'assets_raw/chunks/dec/{cid:04d}.bin', 'rb').read()
    same = img == orig
    nd = sum(1 for a, b in zip(img, orig) if a != b) + abs(len(img) - len(orig))
    print(f'0x{cid:X}: compiled {len(img)} bytes; '
          f'{"IDENTICAL to original" if same else f"changed ({nd} byte diffs)"}')
    # compressor self-check: decompress(compress(img)) == img.
    # NOTE the size+1 canon: the u16 field is len-1; the decompressor
    # stops on dx underflow after emitting field+1 bytes.
    st = lzss_compress(img)
    rt = xd.lzss_decompress(st, len(img) - 1)
    assert rt == img, 'LZSS compress round-trip failed'
    print(f'  compressed: {len(st)} bytes '
          f'({100.0 * len(st) / len(img):.1f}%, engine limit 45182)')
    return img

def cmd_pack(replacements, out_path):
    data = open('DATA.DAT', 'rb').read()
    first_off = struct.unpack_from('<I', data, 0)[0]
    n = first_off // 4
    offs = [struct.unpack_from('<I', data, i * 4)[0] for i in range(n)]
    blocks = []
    for i in range(n):
        start = offs[i]
        end = offs[i + 1] if i + 1 < n else len(data)
        if i in replacements:
            img = compile_file(replacements[i])
            stream = lzss_compress(img)
            assert xd.lzss_decompress(stream, len(img) - 1) == img
            comp = struct.pack('<H', len(img) - 1) + stream
            if len(comp) >= 0xB080:
                raise ValueError(f'chunk 0x{i:X}: compressed {len(comp)} '
                                 f'exceeds the engine limit 0xB080')
            blocks.append(comp)
            print(f'chunk 0x{i:X}: replaced ({len(img)} raw -> {len(comp)} packed)')
        else:
            blocks.append(data[start:end])
    # rebuild the header: same count, offsets recomputed sequentially
    out = bytearray()
    cur = first_off
    for b in blocks:
        out += struct.pack('<I', cur)
        cur += len(b)
    for b in blocks:
        out += b
    open(out_path, 'wb').write(out)
    if not replacements:
        same = bytes(out) == data
        print(f'pack identity (no replacements): {same}')
        return 0 if same else 1
    print(f'packed {out_path}: {len(out)} bytes ({len(replacements)} replaced)')
    return 0

def cmd_locate(path, addr):
    text = open(path).read()
    lm = []
    lf.compile_free(text, line_map=lm)
    lines = text.splitlines()
    best = None
    for lineno, a in lm:
        if a <= addr and (best is None or a > best[1]):
            best = (lineno, a)
    if best is None:
        print(f'no code line at or before 0x{addr:04X}')
        return 1
    lineno, a = best
    print(f'0x{addr:04X} -> line {lineno} (code starts @0x{a:04X}):')
    for i in range(max(0, lineno - 2), min(len(lines), lineno + 1)):
        mark = '>' if i == lineno - 1 else ' '
        print(f'{mark} {i+1}: {lines[i]}')
    return 0

def main():
    if sys.argv[1] == 'build':
        cmd_build(sys.argv[2], int(sys.argv[3], 16))
        return 0
    if sys.argv[1] == 'locate':
        return cmd_locate(sys.argv[2], int(sys.argv[3], 16))
    if sys.argv[1] == 'pack':
        reps = {}
        out = 'DATA_NEW.DAT'
        args = sys.argv[2:]
        i = 0
        while i < len(args):
            if args[i] == '-o':
                out = args[i + 1]; i += 2
            else:
                cid, path = args[i].split('=')
                reps[int(cid, 16)] = path
                i += 1
        return cmd_pack(reps, out)
    print(__doc__)
    return 1

if __name__ == '__main__':
    sys.exit(main())
