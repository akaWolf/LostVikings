#!/usr/bin/env python3
# Точная реплика loc_10a22 (src/vikings.exe_seg000.cpp):
# ring DS[0..0xFFF] = нули, bx=0; бит=1 → литерал, бит=0 → ссылка
# {si=word&0xFFF абсолютный, len=(word>>12)+3}; dx = word_10980 = размер.
# Выход: DEC DX; CMP DX,size; JB continue — underflow-условие, оригинал
# выдаёт size+1 байт (dx проходит через 0)! Резать на dx==0 = потерять хвост.
import struct, sys

def lv_decompress(comp, out_size):
    ring = bytearray(0x1000)
    bx = 0
    out = bytearray()
    si = 0
    dx = out_size
    while True:
        al = comp[si]; si += 1
        for _ in range(8):
            carry = al & 1
            al >>= 1
            if carry:
                ah = comp[si]; si += 1
                ring[bx] = ah; bx = (bx + 1) & 0xFFF
                out.append(ah); dx = (dx - 1) & 0xFFFF
                if dx >= out_size: return bytes(out)
            else:
                w = struct.unpack('<H', comp[si:si+2])[0]; si += 2
                cnt = (w >> 12) + 3
                p = w & 0xFFF
                for _ in range(cnt):
                    ah = ring[p]; p = (p + 1) & 0xFFF
                    ring[bx] = ah; bx = (bx + 1) & 0xFFF
                    out.append(ah); dx = (dx - 1) & 0xFFFF
                    if dx >= out_size: return bytes(out)

def chunk_raw(dat, idx):
    o  = struct.unpack('<I', dat[idx*4:idx*4+4])[0]
    o2 = struct.unpack('<I', dat[idx*4+4:idx*4+8])[0]
    return dat[o:o2]

if __name__ == '__main__':
    dat = open(sys.argv[1], 'rb').read()
    idx = int(sys.argv[2], 0)
    raw = chunk_raw(dat, idx)
    size = struct.unpack('<H', raw[0:2])[0]
    out = lv_decompress(raw[2:], size)
    open(sys.argv[3], 'wb').write(out)
    print(f"chunk {idx:#x}: {len(raw)} comp -> {len(out)} bytes -> {sys.argv[3]}")
    print(f"sig: {out[2:44]}")
