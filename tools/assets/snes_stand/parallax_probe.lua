local SLOT = tonumber(os.getenv("PAR_SLOT") or "4")
local OUT = "/tmp/claude-1000/-home-akawolf-projects-own-LostVikings/c8016d6e-afca-425a-b286-f18fb0eb846b/scratchpad/snes_par/"
local logf = io.open(OUT .. string.format("par4_%02X.txt", SLOT), "w")
local function L(s) logf:write(s .. "\n"); logf:flush() end
local WR = emu.memType.snesWorkRam
local function rdw(a) return emu.readWord(a, WR) end
local function wrw(a, v) emu.write(a, v & 0xFF, WR); emu.write(a+1, (v >> 8) & 0xFF, WR) end
local frame, lastLvl, wantA = 0, -1, 0
local writes = {}
local function onWrite(addr, value)
  local st = emu.getState()
  writes[#writes+1] = string.format("%04X:%02X@%s", addr, value, tostring(st["ppu.scanline"] or st["internalRegisters.vCounter"]))
end
for _, bank in ipairs({0x000000, 0x800000}) do
  emu.addMemoryCallback(onWrite, emu.callbackType.write, bank + 0x210D, bank + 0x2114, emu.cpuType.snes, emu.memType.snesMemory)
end
local function dumpwram(tag)
  local f = io.open(OUT .. string.format("%s_%02X_f%d_wram.bin", tag, SLOT, frame), "wb")
  local t = {}
  for a = 0, 0xFFFF do t[#t+1] = string.char(emu.read(a, WR)) end
  f:write(table.concat(t)); f:close()
end
emu.addEventCallback(function()
  local inp = {}
  if (frame >= 1300 and frame < 1306) or (frame >= 1900 and frame < 1906) then inp.start = true end
  if frame < wantA and (frame % 8) < 4 then inp.a = true end
  if frame >= 2650 and frame < 2656 then inp.start = true end
  if (frame >= 3200 and frame < 3206) or (frame >= 3240 and frame < 3246) then inp.a = true end
  if frame >= 3300 and frame < 3900 then inp.right = true; if (frame % 90) < 6 then inp.b = true end; if (frame % 90) >= 45 and (frame % 90) < 51 then inp.a = true end end
  if frame >= 3900 and frame < 4500 then inp.left = true end
  if frame >= 4500 and frame < 4800 then inp.right = true end
  emu.setInput(inp, 0)
end, emu.eventType.inputPolled)
emu.addEventCallback(function()
  frame = frame + 1
  local lvl = rdw(0x19D1)
  if frame > 2401 then local b = rdw(0x0338); if b & 0x70 ~= 0 then wantA = frame + 6 end end
  if lvl ~= lastLvl then L(string.format("f%d: 19D1=%04X 19ED=%04X 0338=%04X", frame, lvl, rdw(0x19ED), rdw(0x0338))); lastLvl = lvl end
  if frame == 2400 then wrw(0x19ED, SLOT); local v = emu.read(0x0338, WR); emu.write(0x0338, v | 1, WR); L(string.format("f2400: FORCED 19ED=%02X", SLOT)) end
  if frame >= 2600 and frame <= 4800 then
    local st = emu.getState()
    L(string.format("P f%d lvl=%04X b1=%s,%s b2=%s,%s vx=%d,%d,%d W=%s", frame, lvl,
      tostring(st["ppu.layers[0].hscroll"]), tostring(st["ppu.layers[0].vscroll"]), tostring(st["ppu.layers[1].hscroll"]), tostring(st["ppu.layers[1].vscroll"]),
      rdw(0x0DF1), rdw(0x0DF3), rdw(0x0DF5), table.concat(writes, " ")))
  end
  writes = {}
  if frame >= 3100 and frame <= 4800 and frame % 100 == 0 then dumpwram('D') end
  if frame == 4801 then L("done"); emu.stop() end
end, emu.eventType.startFrame)
