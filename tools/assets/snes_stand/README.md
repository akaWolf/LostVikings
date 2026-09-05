# SNES DE stand (Mesen 2.1.1 headless) — parallax measurements

`parallax_probe.lua` runs under `Mesen --testRunner LostVikingsDE.sfc parallax_probe.lua`
(Xvfb display, a private HOME with the Mesen settings; `PAR_SLOT` = level slot to
force at frame 2400 via WRAM `$19ED` + `$0338 |= 1`). It hooks the CPU writes to
the PPU scroll registers `$210D..$2114` in banks 00 and 80 (the game runs from the
FastROM mirror) and logs per frame: the register write values with the scanline
(the level layer BG1 is written at the HUD/play split IRQ, line 48; the parallax
layer BG2 in vblank, line ~230), plus WRAM dumps every 100 frames.

Measured model (2026-09-04, exact on every logged frame of STRT/LLM0/FL0T/QCKS/PHR0):

    fx = head[+0x3F], fy = head[+0x41]           (8.8 fixed point, bit 15 = autoscroll)
    par_x = (cam_x * fx) >> 8                    fx & 0x8000: par_x += fx.low/256 per frame
    par_y = (cam_y * fy) >> 8                    (cam = play-area viewport origin, floor)

The BG2 value written in vblank of frame N pairs with the BG1 value of frame N+1.
The parallax tilemap in VRAM holds only the visible window of the map TILED with
its own period (16x16 tiles for the 8x8-quad Egypt/Factory maps).

Screen shake (2026-09-04): patch a COPY of the ROM so the level controller starts
with a shake — class 48's entry at file 0x21D0C (`51 0000 57 0c02`) → `2E 057F 51
0000` (op 2E: src 5, gate (0x7F05>>7)&0x1FE = 254 ticks) — and run parallax_probe on
it: BG1 h alternates cam/cam+5 every frame while BG2 keeps the value of the
UNshaken camera (FL0T 42/47 → 21, LLM0 0/5 → 0). The console derives the parallax
from the viewport without the shake; v2_draw_parallax does the same.

Level start frames (2026-09-04, UX stage 7 reference for the SNES-balance levels):
`level_shot.lua` forces `PAR_SLOT` the same way, auto-presses A on the dialog
flag (`$0338 & 0x70`) and writes `SHOT_OUT/shot_<slot>_f<N>.ppm` from
`emu.getScreenBuffer()` every 200 frames from 2600 to 4800. The forced load
keeps the PPU in forced blank until ~f3100 (`emu.takeScreenshot()` and the
buffer are black there) — the usable frames start at 3200 (password box up,
level behind it). SMRT/V8TR compared against the engine's SNES-balance
heads: same layout, art and colours.
