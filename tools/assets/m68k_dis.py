#!/usr/bin/env python3
"""68k disassembly of the SMD ROM (capstone): m68k_dis.py START_HEX END_HEX.
Run inside `nix-shell -p python3Packages.capstone`. See docs2/GENESIS_ROM_INTERNALS.md."""
import sys, struct
sys.path.insert(0, "/home/akawolf/projects/own/LostVikings/tools/assets")
import smd2pc, capstone
R = smd2pc.SmdRom().rom
start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_000)
md.skipdata = True
for ins in md.disasm(R[start:end], start):
    print(f"{ins.address:06X}: {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")
