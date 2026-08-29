#!/usr/bin/env python3
"""edit_server.py — local editor server (task #106): the full edit loop
(save -> pack -> play) driven from the browser, no downloads.

Serves the level_edit pages rendered from a SCRATCH copy of assets/
(created on first start, reused afterwards so edits accumulate; the
canonical assets/ tree is never touched) and exposes:

  GET  /               level list (game order, passwords, edit links)
  GET  /edit/<CID>     the editor page for that level (server mode)
  GET  /png/<CID>      current level render from the scratch tree
  POST /api/save       {kind: tilemap|header, chunk, data} -> scratch json
  POST /api/pack       assetc.pack over the scratch tree -> .compiled
  POST /api/play       launch the windowed game with V2_ASSETS_DIR=scratch
  POST /api/play_replay {replay, frames, snap} headless run + PPM snap
                       (test channel — the same flow playtest.py uses)
  GET  /api/status     {running, pid} of the launched game

Usage:
  tools/assets/edit_server.py [--port 8137] [--scratch /tmp/lv_edit_scratch]
                              [--fresh]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assetc  # noqa: E402
import level_render as LR  # noqa: E402
import level_edit as LE  # noqa: E402
import texts_exe as TX  # noqa: E402
import anim_bank as AB  # noqa: E402

SCRATCH = "/tmp/lv_edit_scratch"
GAME = [None]          # Popen of the running game (windowed)
LOCK = threading.Lock()

SAVE_KINDS = {
    "tilemap": ("tilemaps", "tilemap_u16"),
    "header": ("level_headers", "level_header_stripe"),
}
# gtld ("bg_tileset" role — historical mislabel; the chunk is the quad
# TEMPLATE TABLE, sub_173C7) is stored in the open tree as the TilesBase
# png+json pair: rebuild both via the role converter (bijective codec).


def scratch_init(fresh):
    src = os.path.join(LR.ROOT, "assets")
    assert os.path.abspath(SCRATCH) != os.path.abspath(src)
    if fresh and os.path.exists(SCRATCH):
        shutil.rmtree(SCRATCH)
    if not os.path.exists(SCRATCH):
        shutil.copytree(src, SCRATCH)
        print(f"scratch: created from assets/ -> {SCRATCH}")
    else:
        print(f"scratch: reusing {SCRATCH}")


def do_pack():
    with LOCK:
        assetc.ASSETS = SCRATCH
        assetc.pack()
        # dialog texts: if the scratch tree carries an edited texts json,
        # bake a patched exe_static for the play env (V2_EXE_STATIC)
        tj = os.path.join(SCRATCH, "texts_exe.json")
        if os.path.exists(tj):
            with open(tj) as f:
                js = json.load(f)
            img = TX.compile_texts(js, TX.load_image())
            out = os.path.join(SCRATCH, "exe_static.bin")
            with open(out + ".tmp", "wb") as f:
                f.write(img)
            os.replace(out + ".tmp", out)
    return len(os.listdir(os.path.join(SCRATCH, ".compiled")))


def sprite_page(cid_hex):
    """Editor for an UNCOMPRESSED sprite bank (sprite_banks/<cid>.bin):
    a dense array of 72-byte units — 4 planes x 2 strips x (1 mask + 8
    data) bytes, the verified glyph/type-1 pixel model. Frame layout per
    class is anim-driven; the editor exposes the raw unit grid."""
    import base64 as b64mod
    cid = int(cid_hex, 16)
    path = os.path.join(SCRATCH, "sprite_banks", f"{cid_hex}.bin")
    if not os.path.exists(path):
        raise FileNotFoundError(f"not an open sprite bank: {cid_hex}")
    with open(path, "rb") as f:
        data = f.read()
    nunits = len(data) // 72
    tail = data[nunits * 72:]
    # palette: first level whose stripe lists this bank
    pal = None
    for fn in sorted(os.listdir(os.path.join(SCRATCH, "level_headers"))):
        if not fn.endswith(".json"):
            continue
        try:
            st = LR.parse_stripe(LR.header_raw(fn[:-5], SCRATCH))
        except Exception:
            continue
        if any(b["chunk"] == cid for b in st["sprite_banks"]):
            pal = LR.compose_palette([(e["chunk"], e["start"])
                                      for e in st["pal_list"]], SCRATCH)
            break
    if pal is None:
        pal = [(i, i, i) for i in range(256)]
    return (SPRITE_PAGE
            % {"cid": cid_hex, "n": nunits,
               "b64": b64mod.b64encode(data[:nunits * 72]).decode(),
               "tail": tail.hex(),
               "pal": json.dumps([list(c) for c in pal],
                                 separators=(",", ":"))})


SPRITE_PAGE = """<!doctype html><meta charset="utf-8">
<title>LV sprites %(cid)s</title>
<style>
 body{background:#111;color:#ddd;font:13px monospace}
 canvas{image-rendering:pixelated;border:1px solid #333}
 #grid{cursor:crosshair}
</style>
<h3>sprite bank %(cid)s — %(n)d units (72B strips, glyph model)</h3>
<button id="save">save</button> <span id="st"></span>
<span style="color:#888">LMB paint, RMB erase (clears the mask bit), M pick</span><br>
<canvas id="grid"></canvas>
<canvas id="units"></canvas>
<canvas id="sw"></canvas> color <span id="col">15</span>
<script>
const N=%(n)d, TAIL="%(tail)s", PAL=%(pal)s, CID="%(cid)s";
const U=Uint8Array.from(atob("%(b64)s"),c=>c.charCodeAt(0));
let SEL=0, COL=15;
// unit pixel model: plane-major 4x(2 strips x 9B); pixel(x,y):
// p=x&3, col=(x>>2)&1, st=y>>2, r=y&3, b=r*2+col, off=p*18+st*9
function upix(u,x,y){
  const p=x&3, c2=(x>>2)&1, st=y>>2, r=y&3, b=r*2+c2, off=u*72+p*18+st*9;
  return (U[off]>>(7-b))&1 ? U[off+1+b] : -1;
}
function upixw(u,x,y,v){
  const p=x&3, c2=(x>>2)&1, st=y>>2, r=y&3, b=r*2+c2, off=u*72+p*18+st*9;
  if(v<0){ U[off]&=~(1<<(7-b)); U[off+1+b]=0; }
  else { U[off]|=(1<<(7-b)); U[off+1+b]=v; }
}
const units=document.getElementById('units');
const grid=document.getElementById('grid');
const UC=32;
function drawUnit(u,g,x0,y0,z){
  for(let y=0;y<8;y++) for(let x=0;x<8;x++){
    const v=upix(u,x,y);
    if(v<0){ g.fillStyle=((x^y)&1)?'#222':'#2a2a2a'; }
    else { const[r,gg,b]=PAL[v]; g.fillStyle=`rgb(${r},${gg},${b})`; }
    g.fillRect(x0+x*z,y0+y*z,z,z);
  }
}
function drawUnits(){
  const rows=Math.ceil(N/UC);
  units.width=UC*18; units.height=rows*18;
  const g=units.getContext('2d');
  g.fillStyle='#181818'; g.fillRect(0,0,units.width,units.height);
  for(let u=0;u<N;u++)
    drawUnit(u,g,(u%%UC)*18+1,((u/UC)|0)*18+1,2);
  g.strokeStyle='#ff4';
  g.strokeRect((SEL%%UC)*18+0.5,((SEL/UC)|0)*18+0.5,17,17);
}
function drawGrid(){
  grid.width=8*24; grid.height=8*24;
  const g=grid.getContext('2d');
  drawUnit(SEL,g,0,0,24);
  g.strokeStyle='#333';
  for(let i=0;i<=8;i++){
    g.beginPath();g.moveTo(i*24,0);g.lineTo(i*24,192);g.stroke();
    g.beginPath();g.moveTo(0,i*24);g.lineTo(192,i*24);g.stroke();
  }
}
function drawSw(){
  const sw=document.getElementById('sw');
  sw.width=16*8; sw.height=16*8;
  const g=sw.getContext('2d');
  for(let i=0;i<256;i++){
    const[r,gg,b]=PAL[i];
    g.fillStyle=`rgb(${r},${gg},${b})`;
    g.fillRect((i%%16)*8,((i/16)|0)*8,8,8);
  }
  g.strokeStyle='#fff';
  g.strokeRect((COL%%16)*8+.5,((COL/16)|0)*8+.5,7,7);
  document.getElementById('col').textContent=COL.toString(16).toUpperCase();
}
units.onclick=e=>{
  const r=units.getBoundingClientRect();
  const u=(((e.clientY-r.top)/18)|0)*UC+(((e.clientX-r.left)/18)|0);
  if(u<N){ SEL=u; drawUnits(); drawGrid(); }
};
document.getElementById('sw').onclick=e=>{
  const r=e.target.getBoundingClientRect();
  COL=((((e.clientY-r.top)/8)|0)*16+(((e.clientX-r.left)/8)|0))&0xFF;
  drawSw();
};
let paint=false;
function gpaint(e){
  const r=grid.getBoundingClientRect();
  const x=((e.clientX-r.left)/24)|0, y=((e.clientY-r.top)/24)|0;
  if(x<0||y<0||x>7||y>7) return;
  upixw(SEL,x,y,(e.buttons&2)?-1:COL);
  drawGrid();
  const g=units.getContext('2d');
  drawUnit(SEL,g,(SEL%%UC)*18+1,((SEL/UC)|0)*18+1,2);
}
grid.onmousedown=e=>{paint=true;gpaint(e);e.preventDefault();};
grid.onmousemove=e=>{if(paint)gpaint(e);};
grid.oncontextmenu=e=>e.preventDefault();
window.addEventListener('mouseup',()=>{paint=false;});
document.getElementById('save').onclick=async()=>{
  let bin=''; U.forEach(v=>{bin+=String.fromCharCode(v);});
  const r=await fetch('/api/save',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({kind:'sprite_bank',chunk:CID,
                         data:{b64:btoa(bin),tail:TAIL}})});
  const js=await r.json();
  document.getElementById('st').textContent=js.ok?'saved (pack to bake)':('ERR '+js.error);
};
drawUnits(); drawGrid(); drawSw();
</script>"""


ANIM_PAGE = """<!doctype html><meta charset="utf-8">
<title>LV anim %(cid)s</title>
<style>body{background:#111;color:#ddd;font:13px monospace}
canvas{image-rendering:pixelated;border:1px solid #333}
#grid{cursor:crosshair}</style>
<h3>anim bank %(cid)s — %(n)d frames (32x32, 4-bit + layer at draw)</h3>
<button id="save">save</button> <span id="st"></span>
<span style="color:#888">LMB paint, RMB erase; colors are the LOW nibble
(the engine ORs the palette layer per object)</span><br>
frame <span id="fsel">0</span>: <canvas id="grid"></canvas>
<canvas id="frames"></canvas><br>
nibble: <span id="col">F</span> <canvas id="sw"></canvas>
<script>
const CID="%(cid)s", JS=%(js)s;
let SEL=0, COL=15;
// frame pixel model (op 0x34DC target = plane-major 4 x 32 strips x 9B):
// pixel(x,y): p=x&3, col=x>>2 (0..7), strip index = p*32+y, bit=col
function fpix(f,x,y){
  const st=JS.frames[f].strips[(x&3)*32+y];
  return (st[0]>>(7-(x>>2)))&1 ? st[1][x>>2] : -1;
}
function fpixw(f,x,y,v){
  const st=JS.frames[f].strips[(x&3)*32+y];
  const b=x>>2;
  if(v<0){ st[0]&=~(1<<(7-b)); st[1][b]=-1; }
  else { st[0]|=(1<<(7-b)); st[1][b]=v; }
}
const GREY=i=>{const g=i<0?0:32+i*13; return i<0?null:[g,g,g];};
function drawFrame(f,g,x0,y0,z){
  for(let y=0;y<32;y++) for(let x=0;x<32;x++){
    const v=fpix(f,x,y);
    if(v<0){ g.fillStyle=((x^y)&1)?'#1c1c2c':'#242434'; }
    else { const c=GREY(v); g.fillStyle=`rgb(${c[0]},${c[1]},${c[2]})`; }
    g.fillRect(x0+x*z,y0+y*z,z,z);
  }
}
const frames=document.getElementById('frames'), grid=document.getElementById('grid');
const FC=16;
function drawFrames(){
  const rows=Math.ceil(JS.frames.length/FC);
  frames.width=FC*34; frames.height=rows*34;
  const g=frames.getContext('2d');
  g.fillStyle='#181818'; g.fillRect(0,0,frames.width,frames.height);
  for(let f=0;f<JS.frames.length;f++)
    drawFrame(f,g,(f%%FC)*34+1,((f/FC)|0)*34+1,1);
  g.strokeStyle='#ff4';
  g.strokeRect((SEL%%FC)*34+.5,((SEL/FC)|0)*34+.5,33,33);
}
function drawGrid(){
  document.getElementById('fsel').textContent=SEL;
  grid.width=32*10; grid.height=32*10;
  drawFrame(SEL,grid.getContext('2d'),0,0,10);
}
function drawSw(){
  const sw=document.getElementById('sw');
  sw.width=16*14; sw.height=14;
  const g=sw.getContext('2d');
  for(let i=0;i<16;i++){
    const c=GREY(i); g.fillStyle=`rgb(${c[0]},${c[1]},${c[2]})`;
    g.fillRect(i*14,0,14,14);
  }
  g.strokeStyle='#ff4'; g.strokeRect(COL*14+.5,.5,13,13);
  document.getElementById('col').textContent=COL.toString(16).toUpperCase();
}
frames.onclick=e=>{
  const r=frames.getBoundingClientRect();
  const f=(((e.clientY-r.top)/34)|0)*FC+(((e.clientX-r.left)/34)|0);
  if(f<JS.frames.length){ SEL=f; drawFrames(); drawGrid(); }
};
document.getElementById('sw').onclick=e=>{
  const r=e.target.getBoundingClientRect();
  COL=Math.min(15,((e.clientX-r.left)/14)|0); drawSw();
};
let paint=false;
function gp(e){
  const r=grid.getBoundingClientRect();
  const x=((e.clientX-r.left)/10)|0, y=((e.clientY-r.top)/10)|0;
  if(x<0||y<0||x>31||y>31) return;
  fpixw(SEL,x,y,(e.buttons&2)?-1:COL);
  drawGrid();
  drawFrame(SEL,frames.getContext('2d'),(SEL%%FC)*34+1,((SEL/FC)|0)*34+1,1);
}
grid.onmousedown=e=>{paint=true;gp(e);e.preventDefault();};
grid.onmousemove=e=>{if(paint)gp(e);};
grid.oncontextmenu=e=>e.preventDefault();
window.addEventListener('mouseup',()=>{paint=false;});
document.getElementById('save').onclick=async()=>{
  const r=await fetch('/api/save',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({kind:'anim_bank',chunk:CID,data:JS})});
  const js2=await r.json();
  document.getElementById('st').textContent=js2.ok?'saved (pack to bake)':('ERR '+js2.error);
};
drawFrames(); drawGrid(); drawSw();
</script>"""


def anim_page(cid_hex):
    path = os.path.join(SCRATCH, "misc", "anim_bank", f"{cid_hex}.bin")
    if not os.path.exists(path):
        raise FileNotFoundError(f"not an open anim bank: {cid_hex}")
    with open(path, "rb") as f:
        data = f.read()
    js = AB.chunk_to_frames(data)
    return ANIM_PAGE % {"cid": cid_hex, "n": len(js["frames"]),
                        "js": json.dumps(js, separators=(",", ":"))}


def texts_page():
    tj = os.path.join(SCRATCH, "texts_exe.json")
    if not os.path.exists(tj):
        js = TX.extract(TX.load_image())
    else:
        with open(tj) as f:
            js = json.load(f)
    rows = []
    for e in js["entries"]:
        t = (e["text"].replace("&", "&amp;").replace("<", "&lt;")
             .replace('"', "&quot;").replace("\n", "&#10;"))
        rows.append(
            f"<tr><td>{e['i']}</td><td>{e['w']}x{e['h']}</td>"
            f"<td><textarea data-i='{e['i']}' rows='2' cols='42'>{t}</textarea>"
            f"</td></tr>")
    return ("<!doctype html><meta charset='utf-8'><title>LV texts</title>"
            "<style>body{background:#111;color:#ddd;font:13px monospace}"
            "textarea{background:#222;color:#ddd;border:1px solid #444}"
            "td{padding:2px 6px;vertical-align:top}</style>"
            "<h2>dialog texts (seg001) — budget <span id='bud'></span></h2>"
            "<button onclick='saveTexts()'>save</button> <span id='st'></span>"
            "<table>" + "".join(rows) + "</table>"
            "<script>"
            "const ZONE=" + str(TX.BUDGET_END) + ";"
            "function budget(){let n=" + str(js["table_end"]) + ";"
            " const seen=new Map();"
            " document.querySelectorAll('textarea').forEach(t=>{"
            "  const k=t.value; if(!seen.has(k)) seen.set(k, 3+k.length);});"
            " seen.forEach(v=>{n+=v;});"
            " document.getElementById('bud').textContent=n+'/'+ZONE;"
            " return n;}"
            "budget();"
            "document.addEventListener('input',budget);"
            "async function saveTexts(){"
            " const texts={};"
            " document.querySelectorAll('textarea').forEach(t=>{texts[t.dataset.i]=t.value;});"
            " const r=await fetch('/api/save',{method:'POST',"
            "  headers:{'Content-Type':'application/json'},"
            "  body:JSON.stringify({kind:'texts',texts:texts})});"
            " const js2=await r.json();"
            " document.getElementById('st').textContent=js2.ok?'saved (pack to bake)':('ERR '+js2.error);}"
            "</script>")


MOD_DIRS = ("tilemaps", "level_headers", "tilesets", "bg_tilesets",
            "tile_masks", "palettes", "level_scripts", "sprite_banks",
            "unreferenced")


# task #104: the five console-exclusive levels of the extended SNES build.
# Every field verified against the ROM (chunk table @0x58000, stripe grammar,
# per-class configs @0x010000); the donor is a PC slot of the SAME world, so
# the port reuses PC-native classes/banks/anims. pal = an unreferenced
# archive id that takes the converted BG palette (one per level so all five
# can live in one tree at once).
SNES_LEVELS = [
    {"id": 0x16C, "name": "TR33", "world": "Caves",   "donor": "002A",
     "pal": "0155", "dims": "66x43"},
    {"id": 0x171, "name": "SNDS", "world": "Egypt",   "donor": "0053",
     "pal": "0156", "dims": "108x32"},
    {"id": 0x176, "name": "TMPL", "world": "Egypt",   "donor": "0055",
     "pal": "015C", "dims": "140x50"},
    {"id": 0x17B, "name": "RVTS", "world": "Factory", "donor": "007A",
     "pal": "00DB", "dims": "70x40"},
    {"id": 0x180, "name": "PDDY", "world": "Candy",   "donor": "00A6",
     "pal": "00D9", "dims": "80x45"},
]


def snes_page():
    import snes2pc as SP
    have_rom = os.path.exists(SP.ROM_PATH)
    rows = []
    for e in SNES_LEVELS:
        rows.append(
            f"<tr><td>{e['id']:04X}</td><td>{e['name']}</td>"
            f"<td>{e['world']}</td><td>{e['dims']}</td>"
            f"<td><input size='4' value='{e['donor']}' "
            f"id='d{e['id']:X}'></td>"
            f"<td><input size='4' value='{e['pal']}' id='p{e['id']:X}'></td>"
            f"<td><button onclick=\"conv('{e['id']:X}')\">convert</button></td>"
            f"<td id='s{e['id']:X}'></td>"
            f"<td><a href='/edit/{e['donor']}'>edit slot</a></td></tr>")
    warn = ("" if have_rom else
            "<p style='color:#f88'>ROM not found at " + SP.ROM_PATH +
            " — conversion will fail.</p>")
    return ("<!doctype html><meta charset='utf-8'><title>SNES exclusives</title>"
            "<style>body{background:#111;color:#ddd;font:14px monospace}"
            "a{color:#8cf}td{padding:3px 8px;border-bottom:1px solid #222}"
            "input{background:#222;color:#ddd;border:1px solid #444}"
            "</style><h2>SNES DE exclusives &rarr; scratch tree</h2>" + warn +
            "<p>Converting writes the donor slot's header, tilemap, tileset, "
            "templates and a BG palette chunk. The donor's PC level is "
            "REPLACED in the scratch tree (canonical assets/ untouched); play "
            "it with its own password or V2_START_LEVEL.</p>"
            "<table><tr><th>snes</th><th>name</th><th>world</th><th>dims</th>"
            "<th>donor</th><th>pal chunk</th><th></th><th></th><th></th></tr>"
            + "".join(rows) + "</table>"
            "<p><a href='/'>&larr; level list</a></p>"
            "<script>"
            "async function conv(id){"
            " const st=document.getElementById('s'+id); st.textContent='...';"
            " const r=await fetch('/api/snes/convert',{method:'POST',"
            "  headers:{'Content-Type':'application/json'},"
            "  body:JSON.stringify({snes:parseInt(id,16),"
            "   donor:document.getElementById('d'+id).value,"
            "   pal:document.getElementById('p'+id).value})});"
            " const js=await r.json();"
            " st.textContent=js.ok?('ok: '+js.tiles+' tiles, '+js.spawns+"
            "  ' spawns, prio dropped '+js.prio):('ERR '+js.error);}"
            "</script>")


def mod_rel_ok(rel):
    parts = rel.split("/")
    return (len(parts) == 2 and parts[0] in MOD_DIRS
            and ".." not in rel and not rel.startswith("/"))


def mod_export():
    """Files under the mod dirs that differ from the canonical assets/."""
    import base64 as b64mod
    canon = os.path.join(LR.ROOT, "assets")
    out = {}
    for sub in MOD_DIRS:
        sdir = os.path.join(SCRATCH, sub)
        if not os.path.isdir(sdir):
            continue
        for fn in sorted(os.listdir(sdir)):
            sp = os.path.join(sdir, fn)
            if not os.path.isfile(sp):
                continue
            with open(sp, "rb") as f:
                sdata = f.read()
            cp = os.path.join(canon, sub, fn)
            if os.path.exists(cp):
                with open(cp, "rb") as f:
                    if f.read() == sdata:
                        continue
            out[f"{sub}/{fn}"] = b64mod.b64encode(sdata).decode()
    return out


def level_listing():
    pws = LR.level_passwords()
    rows = []
    for idx, (hc, sc) in enumerate(LR.level_tables()):
        cid = f"{hc:04X}"
        if not os.path.exists(os.path.join(SCRATCH, "level_headers",
                                           f"{cid}.json")):
            continue
        pw = pws[idx] if idx < len(pws) else "-"
        script = f"{sc:04X}" if sc != 0xFFFF else "-"
        rows.append(f'<tr><td>{idx}</td><td>{pw}</td>'
                    f'<td><a href="/edit/{cid}">{cid}</a></td>'
                    f'<td>{script}</td>'
                    f'<td><img src="/png/{cid}" loading="lazy" '
                    f'style="height:48px;image-rendering:pixelated"></td></tr>')
    return ("<!doctype html><meta charset='utf-8'><title>LV edit server</title>"
            "<style>body{background:#111;color:#ddd;font:14px monospace}"
            "a{color:#8cf}td{padding:2px 10px;border-bottom:1px solid #222}"
            "</style><h2>LV editor — scratch tree: " + SCRATCH + "</h2>"
            "<table><tr><th>lvl</th><th>pw</th><th>header</th><th>lvs</th>"
            "<th></th></tr>" + "".join(rows) + "</table>"
            "<p><a href='/texts'>dialog texts</a> &middot; "
            "<a href='/snes'>SNES exclusives</a></p>"
            "<h3>clone level (same world)</h3>"
            "src <input id='c_src' size='4'> → dst <input id='c_dst' size='4'> "
            "<button onclick='doClone()'>clone</button> <span id='c_st'></span>"
            "<h3>mod package</h3>"
            "<button onclick='modExport()'>export mod.json</button> "
            "<input type='file' id='modf' accept='.json'>"
            "<button onclick='modImport()'>import</button> <span id='m_st'></span>"
            "<script>"
            "async function doClone(){"
            " const r=await fetch('/api/clone',{method:'POST',headers:{'Content-Type':'application/json'},"
            "  body:JSON.stringify({src:document.getElementById('c_src').value,"
            "                       dst:document.getElementById('c_dst').value})});"
            " const js=await r.json();"
            " document.getElementById('c_st').textContent=js.ok?('ok -> '+js.dst_header):('ERR '+js.error);}"
            "async function modExport(){"
            " const r=await fetch('/api/mod/export'); const js=await r.json();"
            " const a=document.createElement('a');"
            " a.href=URL.createObjectURL(new Blob([JSON.stringify(js.mod,null,1)],{type:'application/json'}));"
            " a.download='mod.json'; a.click();"
            " document.getElementById('m_st').textContent=Object.keys(js.mod).length+' files';}"
            "async function modImport(){"
            " const f=document.getElementById('modf').files[0];"
            " if(!f){document.getElementById('m_st').textContent='pick a file';return;}"
            " const files=JSON.parse(await f.text());"
            " const r=await fetch('/api/mod/import',{method:'POST',headers:{'Content-Type':'application/json'},"
            "  body:JSON.stringify({files:files})});"
            " const js=await r.json();"
            " document.getElementById('m_st').textContent=js.ok?('written '+js.written):('ERR '+js.error);}"
            "</script>")


class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *a):
        sys.stderr.write("[srv] " + fmt % a + "\n")

    def _send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj))

    def _err(self, msg, code=400):
        self._json({"ok": False, "error": msg}, code)

    def do_GET(self):
        try:
            if self.path == "/":
                return self._send(200, level_listing(), "text/html")
            if self.path.startswith("/edit/"):
                cid = self.path[6:].split("?")[0].upper()
                html = LE.render_page(cid, server=True, root=SCRATCH)
                return self._send(200, html, "text/html")
            if self.path.startswith("/anim/"):
                cid_hex = self.path[6:].split("?")[0].upper()
                return self._send(200, anim_page(cid_hex), "text/html")
            if self.path.startswith("/sprites/"):
                cid_hex = self.path[9:].split("?")[0].upper()
                return self._send(200, sprite_page(cid_hex), "text/html")
            if self.path == "/texts":
                return self._send(200, texts_page(), "text/html")
            if self.path == "/snes":
                return self._send(200, snes_page(), "text/html")
            if self.path.startswith("/png/"):
                cid = self.path[5:].split("?")[0].upper()
                png, _, _ = LR.render(cid, root=SCRATCH)
                return self._send(200, png, "image/png")
            if self.path == "/api/mod/export":
                return self._json({"ok": True, "mod": mod_export()})
            if self.path == "/api/status":
                p = GAME[0]
                running = p is not None and p.poll() is None
                return self._json({"ok": True, "running": running,
                                   "pid": p.pid if running else None})
            self._err("not found", 404)
        except Exception as e:                                  # noqa: BLE001
            self._err(f"{type(e).__name__}: {e}", 500)

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            if self.path == "/api/save":
                return self.api_save(body)
            if self.path == "/api/pack":
                return self._json({"ok": True, "packed": do_pack()})
            if self.path == "/api/play":
                return self.api_play()
            if self.path == "/api/play_replay":
                return self.api_play_replay(body)
            if self.path == "/api/clone":
                return self.api_clone(body)
            if self.path == "/api/mod/import":
                return self.api_mod_import(body)
            if self.path == "/api/snes/convert":
                return self.api_snes_convert(body)
            self._err("not found", 404)
        except Exception as e:                                  # noqa: BLE001
            self._err(f"{type(e).__name__}: {e}", 500)

    def api_save(self, body):
        kind = body.get("kind")
        if kind == "gtld":
            return self.api_save_gtld(body)
        if kind == "tileset":
            return self.api_save_tileset(body)
        if kind == "tilemask":
            return self.api_save_tilemask(body)
        if kind == "texts":
            return self.api_save_texts(body)
        if kind == "sprite_bank":
            return self.api_save_sprite_bank(body)
        if kind == "anim_bank":
            return self.api_save_anim_bank(body)
        if kind not in SAVE_KINDS:
            return self._err(f"bad kind {kind!r}")
        sub, fmt = SAVE_KINDS[kind]
        data = body.get("data")
        chunk = str(body.get("chunk", "")).upper()
        if not isinstance(data, dict) or data.get("format") != fmt:
            return self._err(f"data.format must be {fmt!r}")
        if str(data.get("chunk", "")).upper() != chunk or len(chunk) != 4:
            return self._err("chunk mismatch")
        path = os.path.join(SCRATCH, sub, f"{chunk}.json")
        if not os.path.exists(path):
            return self._err(f"unknown chunk {chunk} for {kind}")
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=1)
        os.replace(tmp, path)
        return self._json({"ok": True, "path": path})

    def api_save_gtld(self, body):
        chunk = str(body.get("chunk", "")).upper()
        data = body.get("data") or {}
        words = data.get("words")
        rest = data.get("rest", "")
        if len(chunk) != 4 or not isinstance(words, list) or len(words) % 4:
            return self._err("gtld: need chunk + words (multiple of 4)")
        if not os.path.exists(os.path.join(SCRATCH, "bg_tilesets",
                                           f"{chunk}.json")):
            return self._err(f"unknown gtld chunk {chunk}")
        raw = bytearray()
        for w in words:
            w = int(w) & 0xFFFF
            raw += bytes((w & 0xFF, w >> 8))
        raw += bytes.fromhex(rest)
        files = assetc.converter_for("bg_tileset").extract(int(chunk, 16),
                                                           bytes(raw))
        for rel, blob in files:
            path = os.path.join(SCRATCH, rel)
            tmp = path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(blob)
            os.replace(tmp, path)
        return self._json({"ok": True, "files": [r for r, _ in files]})

    def api_save_tileset(self, body):
        import base64 as b64mod
        chunk = str(body.get("chunk", "")).upper()
        data = body.get("data") or {}
        if len(chunk) != 4 or "b64" not in data:
            return self._err("tileset: need chunk + b64")
        if not os.path.exists(os.path.join(SCRATCH, "tilesets",
                                           f"{chunk}.json")):
            return self._err(f"unknown tileset chunk {chunk}")
        raw = b64mod.b64decode(data["b64"])
        if len(raw) % 64:
            return self._err("tileset bytes must be a multiple of 64")
        raw += bytes.fromhex(data.get("tail", ""))
        files = assetc.converter_for("tileset").extract(int(chunk, 16), raw)
        for rel, blob in files:
            path = os.path.join(SCRATCH, rel)
            tmp = path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(blob)
            os.replace(tmp, path)
        return self._json({"ok": True, "files": [r for r, _ in files]})

    def api_save_tilemask(self, body):
        import base64 as b64mod
        chunk = str(body.get("chunk", "")).upper()
        data = body.get("data") or {}
        if len(chunk) != 4 or "b64" not in data:
            return self._err("tilemask: need chunk + b64")
        if not os.path.exists(os.path.join(SCRATCH, "tile_masks",
                                           f"{chunk}.json")):
            return self._err(f"unknown tile_masks chunk {chunk}")
        raw = b64mod.b64decode(data["b64"])
        if len(raw) % 8:
            return self._err("mask bytes must be a multiple of 8")
        raw += bytes.fromhex(data.get("tail", ""))
        files = assetc.converter_for("tile_masks").extract(int(chunk, 16), raw)
        for rel, blob in files:
            path = os.path.join(SCRATCH, rel)
            tmp = path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(blob)
            os.replace(tmp, path)
        return self._json({"ok": True, "files": [r for r, _ in files]})

    def api_clone(self, body):
        """Play level SRC in slot DST (same world): DST header = SRC stripe
        with the tilemap reference retargeted to DST's own tilemap chunk
        (so the copy is independently editable); tileset/templates stay
        shared (same world). The .lvs script comes from the LEVEL TABLE
        (exe_static), not the stripe — hence the same-world restriction."""
        src = str(body.get("src", "")).upper()
        dst = str(body.get("dst", "")).upper()
        if src == dst or len(src) != 4 or len(dst) != 4:
            return self._err("need distinct src/dst")
        sp = os.path.join(SCRATCH, "level_headers", f"{src}.json")
        dp = os.path.join(SCRATCH, "level_headers", f"{dst}.json")
        if not (os.path.exists(sp) and os.path.exists(dp)):
            return self._err("unknown src/dst header")
        if LR.script_for_header(int(src, 16)) !=            LR.script_for_header(int(dst, 16)):
            return self._err("src and dst are in different worlds "
                             "(.lvs class tables differ)")
        with open(sp) as f:
            src_named = json.load(f)
        with open(dp) as f:
            dst_named = json.load(f)
        src_raw = bytes.fromhex(src_named["raw"])
        st = LR.parse_stripe(src_raw)
        head = bytearray(bytes.fromhex(st["head"]))
        dst_tm = int(dst_named["tilemap"], 16)
        head[0x2E] = dst_tm & 0xFF
        head[0x2F] = dst_tm >> 8
        st["head"] = bytes(head).hex()
        out = dict(src_named)
        out["chunk"] = dst
        out["tilemap"] = f"{dst_tm:04X}"
        out["raw"] = LR.serialize_stripe(st).hex()
        # copy the tilemap content into dst's own chunk
        with open(os.path.join(SCRATCH, "tilemaps",
                               f"{src_named['tilemap'].upper()}.json")) as f:
            tmj = json.load(f)
        tmj["chunk"] = f"{dst_tm:04X}"
        for path, obj in ((dp, out),
                          (os.path.join(SCRATCH, "tilemaps",
                                        f"{dst_tm:04X}.json"), tmj)):
            tmp = path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(obj, f, indent=1)
            os.replace(tmp, path)
        return self._json({"ok": True, "dst_header": dst,
                           "dst_tilemap": f"{dst_tm:04X}"})

    def api_snes_convert(self, body):
        """Convert one SNES-exclusive level into the scratch tree over a
        donor PC slot of the same world (task #104)."""
        import snes2pc as SP
        snes = int(body.get("snes", 0))
        donor = str(body.get("donor", "")).upper()
        pal = str(body.get("pal", "")).upper()
        if not any(e["id"] == snes for e in SNES_LEVELS):
            return self._err(f"unknown SNES level 0x{snes:X}")
        if len(donor) != 4 or not os.path.exists(
                os.path.join(SCRATCH, "level_headers", f"{donor}.json")):
            return self._err(f"unknown donor slot {donor}")
        if len(pal) != 4:
            return self._err("pal chunk must be 4 hex digits")
        if not os.path.exists(SP.ROM_PATH):
            return self._err(f"ROM not found: {SP.ROM_PATH}")
        with LOCK:
            info = SP.convert_level(snes, donor, SCRATCH, int(pal, 16))
        return self._json({"ok": True, "tiles": info["tiles"],
                           "spawns": info["spawns"],
                           "prio": info["prio_dropped"],
                           "dims": info["dims"], "donor": donor})

    def api_mod_import(self, body):
        files = body.get("files") or {}
        import base64 as b64mod
        written = []
        for rel, b64 in files.items():
            if not mod_rel_ok(rel):
                return self._err(f"bad path {rel!r}")
            path = os.path.join(SCRATCH, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(b64mod.b64decode(b64))
            os.replace(tmp, path)
            written.append(rel)
        return self._json({"ok": True, "written": len(written)})

    def api_save_texts(self, body):
        texts = body.get("texts") or {}
        tj = os.path.join(SCRATCH, "texts_exe.json")
        if os.path.exists(tj):
            with open(tj) as f:
                js = json.load(f)
        else:
            js = TX.extract(TX.load_image())
        by_i = {str(e["i"]): e for e in js["entries"]}
        n = 0
        for k, v in texts.items():
            e = by_i.get(str(k))
            if e is None:
                return self._err(f"bad text index {k}")
            if not all(c == "\n" or 0x20 <= ord(c) < 0x7F for c in v):
                return self._err(f"text {k}: ASCII + newline only")
            if e["text"] != v:
                e["text"] = v
                n += 1
        # budget check via the compiler itself
        try:
            TX.compile_texts(js, TX.load_image())
        except ValueError as ve:
            return self._err(str(ve))
        with open(tj + ".tmp", "w") as f:
            json.dump(js, f, indent=1)
        os.replace(tj + ".tmp", tj)
        return self._json({"ok": True, "changed": n})

    def api_save_sprite_bank(self, body):
        import base64 as b64mod
        chunk = str(body.get("chunk", "")).upper()
        data = body.get("data") or {}
        path = os.path.join(SCRATCH, "sprite_banks", f"{chunk}.bin")
        if len(chunk) != 4 or "b64" not in data or not os.path.exists(path):
            return self._err(f"bad sprite bank {chunk}")
        raw = b64mod.b64decode(data["b64"])
        if len(raw) % 72:
            return self._err("bank bytes must be a multiple of 72")
        raw += bytes.fromhex(data.get("tail", ""))
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(raw)
        os.replace(tmp, path)
        return self._json({"ok": True, "path": path})

    def api_save_anim_bank(self, body):
        chunk = str(body.get("chunk", "")).upper()
        js = body.get("data") or {}
        path = os.path.join(SCRATCH, "misc", "anim_bank", f"{chunk}.bin")
        if len(chunk) != 4 or "frames" not in js or not os.path.exists(path):
            return self._err(f"bad anim bank {chunk}")
        raw = AB.frames_to_chunk(js)
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(raw)
        os.replace(tmp, path)
        return self._json({"ok": True, "bytes": len(raw)})

    def api_play(self):
        p = GAME[0]
        if p is not None and p.poll() is None:
            return self._err(f"game already running (pid {p.pid})", 409)
        env = dict(os.environ)
        env["V2_ASSETS_DIR"] = os.path.join(SCRATCH, ".compiled")
        exe_img = os.path.join(SCRATCH, "exe_static.bin")
        if os.path.exists(exe_img):
            env["V2_EXE_STATIC"] = exe_img
        GAME[0] = subprocess.Popen([os.path.join(LR.ROOT, "vikings")],
                                   cwd=LR.ROOT, env=env)
        return self._json({"ok": True, "pid": GAME[0].pid})

    def api_play_replay(self, body):
        replay = body.get("replay", "tests/replays/level1.inp")
        snap = int(body.get("snap", 0))
        frames = int(body.get("frames", 0)) or (snap + 100 if snap else 600)
        env = dict(os.environ)
        env.update({"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy",
                    "V2_NOVSYNC": "1", "V2_AIL_FRAME_TICKS": "1",
                    "V2_ASSETS_DIR": os.path.join(SCRATCH, ".compiled")})
        exe_img = os.path.join(SCRATCH, "exe_static.bin")
        if os.path.exists(exe_img):
            env["V2_EXE_STATIC"] = exe_img
        if snap:
            env["V2_LADDER_SNAP"] = str(snap)
            for f in (snap, snap + 1):
                p = f"/tmp/ladder_f{f}.ppm"
                if os.path.exists(p):
                    os.unlink(p)
        if body.get("headless"):
            cmd = [os.path.join(LR.ROOT, "vikings_headless"),
                   f"--replay-input={os.path.join(LR.ROOT, replay)}",
                   f"--max-frames={frames}"]
        else:
            cmd = [os.path.join(LR.ROOT, "vikings"),
                   "--replay", os.path.join(LR.ROOT, replay),
                   f"--max-frames={frames}"]
        r = subprocess.run(cmd, cwd=LR.ROOT, env=env, timeout=600,
                           capture_output=True)
        out = {"ok": True, "exit": r.returncode}
        if snap:
            src = f"/tmp/ladder_f{snap}.ppm"
            if os.path.exists(src):
                import base64
                with open(src, "rb") as f:
                    out["snap_ppm_b64"] = base64.b64encode(f.read()).decode()
            else:
                out["ok"] = False
                out["error"] = f"snap frame {snap} not reached"
        return self._json(out)


def main():
    global SCRATCH
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8137)
    ap.add_argument("--scratch", default=SCRATCH)
    ap.add_argument("--fresh", action="store_true",
                    help="recreate the scratch tree from assets/")
    args = ap.parse_args()
    SCRATCH = os.path.abspath(args.scratch)
    scratch_init(args.fresh)
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), H)
    print(f"editor server: http://127.0.0.1:{args.port}/")
    srv.serve_forever()


if __name__ == "__main__":
    main()
