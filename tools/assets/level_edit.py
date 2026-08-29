#!/usr/bin/env python3
"""level_edit.py — quad-map editor page (task #103, editor v1).

Generates a self-contained HTML editor for one level:
  - the level rendered on a canvas (from the same verified pipeline as
    level_render.py), 16px quad grid;
  - a template palette (every 8-byte gs_tiledata entry rendered as its
    16x16 quad) — click to select, click the map to stamp;
  - per-quad TYPE (map word bits 10-15) editing;
  - SPAWN editing (v2): drag markers to move, edit all record fields
    (x/y/p1/p2/class/anim/pool — sub_13bbd layout) in a side form;
    in-place only (no add/delete: the stripe tail after the table is
    not fully mapped yet, so record count stays fixed);
  - undo, and EXPORT of the edited tilemap as assetc-compatible JSON
    (assets/tilemaps/<id>.json replacement), a raw .bin, or the level
    HEADER json (assets/level_headers/<id>.json replacement) with the
    edited spawn records spliced back into the raw stripe.

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
 <option value="type">set type</option><option value="pick">pick</option>
 <option value="spawn">spawn</option></select>
 type <input id="tyval" size="2" value="0"> (hex)
 | sel tpl <span id="selt">0</span>
 <button id="undo">undo</button>
 <button id="expjson">export .json</button>
 <button id="expbin">export .bin</button>
 <button id="exphdr">export header .json</button>
 zoom <select id="z"><option>1</option><option selected>2</option>
 <option>3</option></select>
 <span id="stat"></span>
</div>
<div id="main">
 <div id="left"><div style="position:relative">
  <canvas id="map"></canvas>
  <canvas id="sov" style="position:absolute;left:0;top:0;pointer-events:none"></canvas>
 </div></div>
 <div id="right">
  <div id="spf" style="margin-bottom:8px;border-bottom:1px solid #333;padding-bottom:6px">
   spawn <span id="spidx">-</span> / %(nsp)d<br>
   x <input id="sp_x" size="4"> y <input id="sp_y" size="4"><br>
   p1 <input id="sp_p1" size="4"> p2 <input id="sp_p2" size="4"><br>
   cls <input id="sp_cls" size="3"> anim <input id="sp_anim" size="4">
   pool <input id="sp_pool" size="3"> (hex)<br>
   <span id="spinfo" style="color:#8bc"></span><br>
   <button id="spapply">apply</button>
  </div>
  templates (%(ntpl)d):<canvas id="pal"></canvas></div>
</div>
<div id="tip"></div>
<script>
const QW=%(qw)d, QH=%(qh)d, NT=%(ntpl)d, PCOLS=%(pcols)d, TM_ID="%(tmid)s";
const MAP=%(map)s, TAIL="%(tail)s";
const SPAWNS=%(spawns)s, SPOFFS=%(spoffs)s, HID="%(hid)s";
const CLASSES=%(classes)s; // cls -> sub_13e52 template record
const ICONS=%(icons)s; // engine-harvested sprites (class_icons.py)
const IIMG={};
for(const k in ICONS){const im=new Image();im.onload=()=>{if(typeof drawSpawns==='function')drawSpawns();};im.src='data:image/png;base64,'+ICONS[k].b64; IIMG[k]=im;}
const HDRRAW="%(hdrraw)s", HDRJSON=%(hdrjson)s;
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
  drawSpawns();
}
const sov=document.getElementById('sov');
let SPSEL=-1, spDrag=false;
const SPEDIT=new Set();
function drawSpawns(){
  sov.width=QW*16*Z; sov.height=QH*16*Z;
  const c=sov.getContext('2d');
  c.imageSmoothingEnabled=false;
  SPAWNS.forEach((s,i)=>{
    const x=s.x*Z, y=s.y*Z;
    const ci=CLASSES[s.cls]||null;
    const ic=ICONS[s.cls], im=IIMG[s.cls];
    if(ic&&im&&im.complete){
      c.drawImage(im,(s.x-(ic.w>>1))*Z,(s.y-(ic.h>>1))*Z,ic.w*Z,ic.h*Z);
    }
    if(ci){  // sub_13e52 bbox: X0 = x-(w>>1)
      const x0=(s.x-(ci.w>>1))*Z, y0=(s.y-(ci.h>>1))*Z;
      c.strokeStyle=(i===SPSEL)?'#ff4':(ci.spr===0xFFFF?'#888':'#4cf');
      c.lineWidth=1;
      c.strokeRect(x0+0.5,y0+0.5,ci.w*Z-1,ci.h*Z-1);
    }
    c.strokeStyle=(i===SPSEL)?'#ff4':((s.anim&0x800)?'#6f6':'#f6f');
    c.lineWidth=(i===SPSEL)?2:1;
    c.beginPath();
    c.moveTo(x-4*Z,y); c.lineTo(x+4*Z,y);
    c.moveTo(x,y-4*Z); c.lineTo(x,y+4*Z);
    c.stroke();
    c.fillStyle='#fff'; c.font=(4*Z+4)+'px monospace';
    c.fillText(s.cls.toString(16).toUpperCase(),x+2*Z,y-2*Z);
  });
}
const spX=document.getElementById('sp_x'), spY=document.getElementById('sp_y'),
      spP1=document.getElementById('sp_p1'), spP2=document.getElementById('sp_p2'),
      spCls=document.getElementById('sp_cls'), spAnim=document.getElementById('sp_anim'),
      spPool=document.getElementById('sp_pool');
function spForm(){
  document.getElementById('spidx').textContent=SPSEL<0?'-':SPSEL;
  if(SPSEL<0) return;
  const s=SPAWNS[SPSEL];
  spX.value=s.x; spY.value=s.y; spP1.value=s.p1; spP2.value=s.p2;
  spCls.value=s.cls.toString(16).toUpperCase();
  spAnim.value=s.anim.toString(16).toUpperCase();
  spPool.value=s.pool.toString(16).toUpperCase();
  const ci=CLASSES[s.cls];
  document.getElementById('spinfo').textContent = ci ?
    `class: ${ci.w}x${ci.h} sub=${ci.sub} `+
    (ci.spr===0xFFFF?'INVISIBLE':(ci.spr===0xFFFE?'pool-sprite':
     'spr='+ci.spr.toString(16).padStart(4,'0').toUpperCase()))+
    ` bits=${ci.bits.toString(16).toUpperCase()}` : 'class: ?';
}
function spawnNear(e){
  const r=map.getBoundingClientRect();
  const px=(e.clientX-r.left)/Z, py=(e.clientY-r.top)/Z;
  let best=-1, bd=100;
  SPAWNS.forEach((s,i)=>{
    const d=(s.x-px)*(s.x-px)+(s.y-py)*(s.y-py);
    if(d<bd){bd=d;best=i;}
  });
  return best;
}
map.onmousedown=e=>{
  if(tool.value!=='spawn') return;
  const i=spawnNear(e); SPSEL=i;
  if(i>=0){ hist.push({t:'sp',i:i,rec:Object.assign({},SPAWNS[i])}); spDrag=true; }
  spForm(); drawSpawns();
};
window.onmouseup=()=>{ spDrag=false; };
document.getElementById('spapply').onclick=()=>{
  if(SPSEL<0) return;
  const s=SPAWNS[SPSEL];
  hist.push({t:'sp',i:SPSEL,rec:Object.assign({},s)});
  s.x=(+spX.value)|0; s.y=(+spY.value)|0;
  s.p1=(+spP1.value)|0; s.p2=(+spP2.value)|0;
  s.cls=parseInt(spCls.value,16)||0; s.anim=parseInt(spAnim.value,16)||0;
  s.pool=parseInt(spPool.value,16)||0;
  SPEDIT.add(SPSEL); drawSpawns();
  stat.textContent=` edits:${EDITED.size} sp:${SPEDIT.size}`;
};
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
  if(tool.value==='spawn') return;
  const q=quadAt(e); if(q<0) return;
  if(tool.value==='pick'){ SEL=MAP[q]&0x3FF;
    selt.textContent=SEL.toString(16).toUpperCase();
    tyval.value=(MAP[q]>>10).toString(16).toUpperCase(); drawPal(); return; }
  hist.push({t:'map',q:q,w:MAP[q]});
  if(tool.value==='stamp'){
    MAP[q]=(MAP[q]&0xFC00)|SEL;
  } else {
    const ty=parseInt(tyval.value,16)||0;
    MAP[q]=(MAP[q]&0x3FF)|((ty&0x3F)<<10);
  }
  EDITED.add(q); redrawQuad(q);
  stat.textContent=` edits:${EDITED.size} sp:${SPEDIT.size}`;
};
map.onmousemove=e=>{
  if(spDrag&&SPSEL>=0){
    const r=map.getBoundingClientRect();
    SPAWNS[SPSEL].x=Math.max(0,Math.min(QW*16-1,((e.clientX-r.left)/Z)|0));
    SPAWNS[SPSEL].y=Math.max(0,Math.min(QH*16-1,((e.clientY-r.top)/Z)|0));
    SPEDIT.add(SPSEL); spForm(); drawSpawns();
    stat.textContent=` edits:${EDITED.size} sp:${SPEDIT.size}`;
    return;
  }
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
  if(h.t==='map'){ MAP[h.q]=h.w; redrawQuad(h.q); }
  else { SPAWNS[h.i]=h.rec; if(SPSEL===h.i) spForm(); drawSpawns(); }
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
document.getElementById('exphdr').onclick=()=>{
  const hx=HDRRAW.toLowerCase();
  const arr=new Uint8Array(hx.length/2);
  for(let i=0;i<arr.length;i++) arr[i]=parseInt(hx.substr(i*2,2),16);
  SPAWNS.forEach((s,i)=>{
    const o=SPOFFS[i];
    const put=(off,v)=>{ arr[o+off]=v&0xFF; arr[o+off+1]=(v>>8)&0xFF; };
    put(0,s.x); put(2,s.y); put(4,s.p1); put(6,s.p2);
    put(8,s.cls); put(10,s.anim); put(12,s.pool);
  });
  let hex='';
  arr.forEach(v=>{ hex+=v.toString(16).padStart(2,'0'); });
  const js=Object.assign({},HDRJSON,{raw:hex});
  dl(HID+'.json', new Blob([JSON.stringify(js,null,1)],{type:'application/json'}));
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
    qw, qh, tm_id, ts_id, gt_id, pal_entries, spawns = LR.parse_header(raw)
    # byte offsets of the spawn records inside the stripe (in-place editing)
    sp_offsets = [0x43 + i * 0x0E for i in range(len(spawns))]
    classes = {}
    script_id = LR.script_for_header(int(cid, 16))
    if script_id is not None:
        script_raw, _ = read_payload(script_id, "lzss")
        for sp in spawns:
            if sp["cls"] not in classes:
                rec = LR.class_record(script_raw, sp["cls"])
                if rec:
                    classes[sp["cls"]] = rec
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
        "nsp": len(spawns),
        "tmid": f"{tm_id:04X}",
        "png": base64.b64encode(png).decode(),
        "apng": base64.b64encode(apng).decode(),
        "map": json.dumps(words, separators=(",", ":")),
        "tail": tail,
        "spawns": json.dumps(spawns, separators=(",", ":")),
        "classes": json.dumps(classes, separators=(",", ":")),
        "icons": json.dumps(LR.load_class_icons(script_id),
                            separators=(",", ":")),
        "spoffs": json.dumps(sp_offsets, separators=(",", ":")),
        "hdrraw": raw.hex().upper(),
        "hdrjson": json.dumps({k: v for k, v in json.load(
            open(os.path.join(LR.HDR_DIR, f"{cid}.json"))).items()
            if k != "raw"}, separators=(",", ":")),
        "hid": cid,
    }
    out = args.out or f"/tmp/edit_{cid}.html"
    with open(out, "w") as f:
        f.write(html)
    print(f"{cid}: {qw}x{qh} quads, {ntpl} templates -> {out}")


if __name__ == "__main__":
    main()
