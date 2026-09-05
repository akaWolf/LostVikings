-- UX stage 7: SNES DE reference frames of a level start (force the slot like parallax_probe,
-- auto-press A on the dialog flag, PNG screenshots via emu.takeScreenshot)
local SLOT = tonumber(os.getenv("PAR_SLOT") or "23")
local OUT = os.getenv("SHOT_OUT") or "/tmp/"
local logf = io.open(OUT .. string.format("shot_%02X.txt", SLOT), "w")
local function L(s) logf:write(s .. "\n"); logf:flush() end
local WR = emu.memType.snesWorkRam
local function rdw(a) return emu.readWord(a, WR) end
local function wrw(a, v) emu.write(a, v & 0xFF, WR); emu.write(a+1, (v >> 8) & 0xFF, WR) end
local frame, lastLvl, wantA = 0, -1, 0
emu.addEventCallback(function()
  local inp = {}
  if (frame >= 1300 and frame < 1306) or (frame >= 1900 and frame < 1906) then inp.start = true end
  if frame < wantA and (frame % 8) < 4 then inp.a = true end
  if frame >= 2650 and frame < 2656 then inp.start = true end
  emu.setInput(inp, 0)
end, emu.eventType.inputPolled)
emu.addEventCallback(function()
  frame = frame + 1
  local lvl = rdw(0x19D1)
  if frame > 2401 then local b = rdw(0x0338); if b & 0x70 ~= 0 then wantA = frame + 6 end end
  if lvl ~= lastLvl then L(string.format("f%d: 19D1=%04X 19ED=%04X 0338=%04X", frame, lvl, rdw(0x19ED), rdw(0x0338))); lastLvl = lvl end
  if frame == 2400 then wrw(0x19ED, SLOT); local v = emu.read(0x0338, WR); emu.write(0x0338, v | 1, WR); L(string.format("f2400: FORCED 19ED=%02X", SLOT)) end
  if frame >= 2600 and frame <= 4800 and frame % 200 == 0 then
    local st = emu.getState()
    local sz = emu.getScreenSize(); local buf = emu.getScreenBuffer()
    local f = io.open(OUT .. string.format("shot_%02X_f%d.ppm", SLOT, frame), "wb")
    f:write(string.format("P6\n%d %d\n255\n", sz.width, sz.height))
    local t = {}
    for i = 1, sz.width*sz.height do local c = buf[i]; t[#t+1] = string.char((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF) end
    f:write(table.concat(t)); f:close()
    L(string.format("f%d shot: lvl=%04X 0338=%04X b1=%s,%s cam=%d,%d blank=%s bright=%s mean=%d", frame, lvl, rdw(0x0338), tostring(st["ppu.layers[0].hscroll"]), tostring(st["ppu.layers[0].vscroll"]), rdw(0x0DF1), rdw(0x0DF3), tostring(st["ppu.forcedBlank"]), tostring(st["ppu.screenBrightness"]), (buf[1000] or 0)))
  end
  if frame == 4801 then L("done"); emu.stop() end
end, emu.eventType.startFrame)
