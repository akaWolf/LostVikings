#!/usr/bin/env python3
"""level_edit.py — quad-map editor page (task #103, editor v1).

Generates a self-contained HTML editor for one level:
  - the level rendered on a canvas (from the same verified pipeline as
    level_render.py), 16px quad grid;
  - a template palette (every 8-byte gs_tiledata entry rendered as its
    16x16 quad) — click to select, click the map to stamp;
  - per-quad TYPE (map word bits 10-15) editing;
  - undo, and EXPORT of the edited tilemap as assetc-compatible JSON
    (assets/tilemaps/<id>.json replacement) or a raw .bin.

The written JSON drops into the Stage-5 asset pipeline unchanged:
  cp <download> assets/tilemaps/<id>.json && assetc pack
and the game plays the edit (V2_ASSETS_DIR path).

Usage:
  python3 tools/assets/level_edit.py 00CA [-o out.html]
"""
import argparse
import base64
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assetc import read_payload, tile_decode, png_write  # noqa: E402
import level_render as LR  # noqa: E402


def render_template_atlas(gtld, tgfx, pal, cols=16):
    """Every template as a 16x16 quad; returns (png_bytes, ntpl, cols)."""
    ntpl = len(gtld) // 8
    rows = (ntpl + cols - 1) // cols
    stride = cols * 16
    img = bytearray(stride * rows * 16)
    cache = {}
    for t in range(ntpl):
        e = gtld[t * 8:t * 8 + 8]
        bx, by = (t % cols) * 16, (t // cols) * 16
        for (dy, dx, o) in ((0, 0, 0), (0, 1, 2), (1, 0, 4), (1, 1, 6)):
            dw = e[o] | (e[o + 1] << 8)
            toff = dw & 0xFFC0
            px = cache.get(toff)
            if px is None:
                px = tile_decode(tgfx[toff:toff + 64].ljust(64, b"\x00"))
                cache[toff] = px
            hf, vf = (dw >> 4) & 1, (dw >> 5) & 1
            cx, cy = bx + dx * 8, by + dy * 8
            for y in range(8):
                sy = 7 - y if vf else y
                row = px[sy * 8:sy * 8 + 8]
                if hf:
                    row = row[::-1]
                img[(cy + y) * stride + cx:(cy + y) * stride + cx + 8] = row
    return png_write(stride, rows * 16, bytes(img), pal), ntpl, cols


PAGE = """<!doctype html>
<meta charset="utf-8">
<title>LV edit %(cid)s</title>
<style>
 body { margin:0; background:#111; color:#ddd; font:13px monospace;
        display:flex; flex-direction:column; height:100vh; }
 #bar { padding:6px 8px; background:#222; flex:none; }
 #bar button, #bar input, #bar select { font:inherit; background:#333;
        color:#ddd; border:1px solid #555; }
 #main { display:flex; flex:1; min-height:0; }
 #left { overflow:auto; flex:1; }
 #right { width:300px; flex:none; overflow:auto; background:#181818;
          border-left:1px solid #333; padding:6px; }
 canvas { image-rendering: pixelated; display:block; }
 #pal { cursor:crosshair; }
 #map { cursor:crosshair; }
 #tip { position:fixed; background:#000c; border:1px solid #555;
        padding:4px 6px; pointer-events:none; display:none;
        white-space:pre; z-index:9; }
</style>
<div id="bar">
 level %(cid)s — %(qw)dx%(qh)d quads | tool:
 <select id="tool"><option value="stamp">stamp</option>
 <option value="type">set type</option><option value="pick">pick</option></select>
 type <input id="tyval" size="2" value="0"> (hex)
 | sel tpl <span id="selt">0</span>
 <button id="undo">undo</button>
 <button id="expjson">export .json</button>
 <button id="expbin">export .bin</button>
 zoom <select id="z"><option>1</option><option selected>2</option>
 <option>3</option></select>
 <span id="stat"></span>
</div>
<div id="main">
 <div id="left"><canvas id="map"></canvas></div>
 <div id="right">templates (%(ntpl)d):<canvas id="pal"></canvas></div>
</div>
<div id="tip"></div>
<script>
const QW=%(qw)d, QH=%(qh)d, NT=%(ntpl)d, PCOLS=%(pcols)d, TM_ID="%(tmid)s";
const MAP=%(map)s, TAIL="%(tail)s";
const lvl=new Image(); lvl.src="data:image/png;base64,%(png)s";
const atlas=new Image(); atlas.src="data:image/png;base64,%(apng)s";
const map=document.getElementById('map'), pal=document.getElementById('pal'),
      tip=document.getElementById('tip'), tool=document.getElementById('tool'),
      tyval=document.getElementById('tyval'), selt=document.getElementById('selt'),
      stat=document.getElementById('stat');
let Z=2, SEL=0, hist=[];
const mc=map.getContext('2d'), pc=pal.getContext('2d');
function redrawQuad(q){
  const qx=q%%QW, qy=(q-qx)/QW, t=MAP[q]&0x3FF;
  mc.imageSmoothingEnabled=false;
  mc.drawImage(atlas,(t%%PCOLS)*16,((t/PCOLS)|0)*16,16,16,
               qx*16*Z,qy*16*Z,16*Z,16*Z);
}
function drawAll(){
  map.width=QW*16*Z; map.height=QH*16*Z;
  mc.imageSmoothingEnabled=false;
  mc.drawImage(lvl,0,0,QW*16*Z,QH*16*Z);
  for(let q=0;q<QW*QH;q++) if(EDITED.has(q)) redrawQuad(q);
}
const EDITED=new Set();
function drawPal(){
  pal.width=PCOLS*16; pal.height=Math.ceil(NT/PCOLS)*16+4;
  pc.imageSmoothingEnabled=false; pc.drawImage(atlas,0,0);
  pc.strokeStyle='#ff4';
  pc.strokeRect((SEL%%PCOLS)*16+0.5,((SEL/PCOLS)|0)*16+0.5,15,15);
}
lvl.onload=()=>{ drawAll(); };
atlas.onload=()=>{ drawPal(); };
document.getElementById('z').onchange=e=>{ Z=+e.target.value; drawAll(); };
pal.onclick=e=>{
  const r=pal.getBoundingClientRect();
  const tx=((e.clientX-r.left)/16)|0, ty=((e.clientY-r.top)/16)|0;
  const t=ty*PCOLS+tx; if(t<NT){ SEL=t; selt.textContent=t.toString(16).toUpperCase(); drawPal(); }
};
function quadAt(e){
  const r=map.getBoundingClientRect();
  const qx=((e.clientX-r.left)/(16*Z))|0, qy=((e.clientY-r.top)/(16*Z))|0;
  return (qx<0||qy<0||qx>=QW||qy>=QH)?-1:qy*QW+qx;
}
map.onclick=e=>{
  const q=quadAt(e); if(q<0) return;
  if(tool.value==='pick'){ SEL=MAP[q]&0x3FF;
    selt.textContent=SEL.toString(16).toUpperCase();
    tyval.value=(MAP[q]>>10).toString(16).toUpperCase(); drawPal(); return; }
  hist.push([q,MAP[q]]);
  if(tool.value==='stamp'){
    MAP[q]=(MAP[q]&0xFC00)|SEL;
  } else {
    const ty=parseInt(tyval.value,16)||0;
    MAP[q]=(MAP[q]&0x3FF)|((ty&0x3F)<<10);
  }
  EDITED.add(q); redrawQuad(q);
  stat.textContent=` edits:${EDITED.size}`;
};
map.onmousemove=e=>{
  const q=quadAt(e); if(q<0){tip.style.display='none';return;}
  const w=MAP[q];
  tip.textContent=`(${q%%QW},${(q/QW)|0}) word ${w.toString(16).padStart(4,'0').toUpperCase()}`+
    ` tpl ${(w&0x3FF).toString(16).toUpperCase()} type ${(w>>10).toString(16).toUpperCase()}`;
  tip.style.display='block';
  tip.style.left=(e.clientX+14)+'px'; tip.style.top=(e.clientY+14)+'px';
};
map.onmouseleave=()=>tip.style.display='none';
document.getElementById('undo').onclick=()=>{
  const h=hist.pop(); if(!h) return;
  MAP[h[0]]=h[1]; redrawQuad(h[0]);
};
function dl(name, blob){
  const a=document.createElement('a');
  a.href=URL.createObjectURL(blob); a.download=name; a.click();
}
document.getElementById('expjson').onclick=()=>{
  const rows=[];
  for(let y=0;y<QH;y++){
    const r=[];
    for(let x=0;x<QW;x++) r.push(MAP[y*QW+x].toString(16).padStart(4,'0').toUpperCase());
    rows.push(r.join(' '));
  }
  const js={format:"tilemap_u16", chunk:TM_ID, width:QW, height:QH,
            tail:TAIL, rows:rows};
  dl(TM_ID+'.json', new Blob([JSON.stringify(js,null,1)],{type:'application/json'}));
};
document.getElementById('expbin').onclick=()=>{
  const b=new Uint8Array(MAP.length*2+TAIL.length/2);
  MAP.forEach((w,i)=>{ b[i*2]=w&0xFF; b[i*2+1]=w>>8; });
  for(let i=0;i<TAIL.length;i+=2)
    b[MAP.length*2+i/2]=parseInt(TAIL.substr(i,2),16);
  dl(TM_ID+'.bin', new Blob([b],{type:'application/octet-stream'}));
};
</script>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("header")
    ap.add_argument("-o", "--out")
    args = ap.parse_args()
    cid = args.header
    with open(os.path.join(LR.HDR_DIR, f"{cid}.json")) as f:
        raw = bytes.fromhex(json.load(f)["raw"])
    qw, qh, tm_id, ts_id, gt_id, pal_entries, _sp = LR.parse_header(raw)
    tmap, _ = read_payload(tm_id, "lzss")
    tgfx, _ = read_payload(ts_id, "lzss")
    gtld, _ = read_payload(gt_id, "lzss")
    pal = LR.compose_palette(pal_entries)
    words = [tmap[i * 2] | (tmap[i * 2 + 1] << 8) for i in range(qw * qh)]
    tail = tmap[qw * qh * 2:].hex().upper()
    png, _, _ = LR.render(cid)
    apng, ntpl, pcols = render_template_atlas(gtld, tgfx, pal)
    html = PAGE % {
        "cid": cid, "qw": qw, "qh": qh, "ntpl": ntpl, "pcols": pcols,
        "tmid": f"{tm_id:04X}",
        "png": base64.b64encode(png).decode(),
        "apng": base64.b64encode(apng).decode(),
        "map": json.dumps(words, separators=(",", ":")),
        "tail": tail,
    }
    out = args.out or f"/tmp/edit_{cid}.html"
    with open(out, "w") as f:
        f.write(html)
    print(f"{cid}: {qw}x{qh} quads, {ntpl} templates -> {out}")


if __name__ == "__main__":
    main()
