#!/usr/bin/env python3
"""level_edit.py — quad-map editor page (task #103 v1-v2, #106 v3).

Generates the HTML editor for one level:
  - the level rendered on a canvas (same verified pipeline as
    level_render.py), 16px quad grid, minimap navigation;
  - a template palette (every 8-byte gs_tiledata entry rendered as its
    16x16 quad) — click to select; stamp / set-TYPE apply by click OR by
    dragging a rectangle (single undo entry per rectangle);
  - SPAWN editing: drag markers to move, side form for every record
    field (x/y/half_w/half_h/class/anim/pool — sub_13bbd layout),
    click-to-place ADD, DELETE (the stripe tail is terminator-scanned,
    parse_stripe grammar, so record count may change freely);
  - undo, EXPORT (tilemap .json/.bin, header .json — assetc-compatible);
  - SERVER mode (edit_server.py): save/pack/play buttons drive the
    scratch assets tree end-to-end without downloads.

Usage:
  python3 tools/assets/level_edit.py 00CA [-o out.html]
"""
import argparse
import base64
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assetc import tile_decode, png_write  # noqa: E402
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
 #bar button, #bar input, #bar select, #right button, #right input {
        font:inherit; background:#333; color:#ddd; border:1px solid #555; }
 #srv button { background:#264; }
 #main { display:flex; flex:1; min-height:0; }
 #left { overflow:auto; flex:1; }
 #right { width:300px; flex:none; overflow:auto; background:#181818;
          border-left:1px solid #333; padding:6px; }
 canvas { image-rendering: pixelated; display:block; }
 #pal { cursor:crosshair; }
 #map { cursor:crosshair; }
 #mini { border:1px solid #333; cursor:pointer; margin-bottom:8px; }
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
 <span id="srv" style="display:none">
  | <button id="srvsave">save</button>
  <button id="srvpack">pack</button>
  <button id="srvplay">play</button>
  <button id="srvall">save+pack+play</button>
  <span id="srvstat"></span>
 </span>
 <span id="stat"></span>
</div>
<div id="main">
 <div id="left"><div style="position:relative">
  <canvas id="map"></canvas>
  <canvas id="sov" style="position:absolute;left:0;top:0;pointer-events:none"></canvas>
 </div></div>
 <div id="right">
  <canvas id="mini"></canvas>
  <div id="spf" style="margin-bottom:8px;border-bottom:1px solid #333;padding-bottom:6px">
   spawn <span id="spidx">-</span> / <span id="spn">%(nsp)d</span><br>
   x <input id="sp_x" size="4"> y <input id="sp_y" size="4"><br>
   half_w <input id="sp_p1" size="3"> half_h <input id="sp_p2" size="3"><br>
   cls <input id="sp_cls" size="3"> anim <input id="sp_anim" size="4">
   pool <input id="sp_pool" size="3"> (hex)<br>
   <span id="spinfo" style="color:#8bc"></span><br>
   <button id="spapply">apply</button>
   <button id="spadd">add (click map)</button>
   <button id="spdel">delete</button>
  </div>
  <details style="margin-bottom:8px"><summary>palette list (%(npal)d) — reload after save+pack</summary>
   <div id="plist"></div>
   <button id="pladd">add entry</button>
  </details>
  <details style="margin-bottom:8px"><summary>palette anims (%(npanim)d)</summary>
   en <input id="pen" size="4"><br>
   <div id="panims"></div>
   <button id="paadd">add anim</button>
  </details>
  templates (%(ntpl)d):<canvas id="pal"></canvas></div>
</div>
<div id="tip"></div>
<script>
const SERVER=%(server)d;
const QW=%(qw)d, QH=%(qh)d, NT=%(ntpl)d, PCOLS=%(pcols)d, TM_ID="%(tmid)s";
const MAP=%(map)s, TAIL="%(tail)s";
const SPAWNS=%(spawns)s, HID="%(hid)s";
const CLASSES=%(classes)s; // cls -> sub_13e52 template record
const ICONS=%(icons)s; // engine-harvested sprites (class_icons.py)
const IIMG={};
for(const k in ICONS){const im=new Image();im.onload=()=>{if(typeof drawSpawns==='function')drawSpawns();};im.src='data:image/png;base64,'+ICONS[k].b64; IIMG[k]=im;}
const HDRJSON=%(hdrjson)s;
const ST_HEAD="%(sthead)s", ST_REST="%(strest)s", HDRRAW="%(hdrraw)s";
let PAL_EN=%(palen)d;
const PAL_LIST=%(pallist)s, PAL_ANIMS=%(palanims)s;
const BANKS=%(banks)s, ACHUNKS=%(achunks)s;
const lvl=new Image(); lvl.src="data:image/png;base64,%(png)s";
const atlas=new Image(); atlas.src="data:image/png;base64,%(apng)s";
const map=document.getElementById('map'), pal=document.getElementById('pal'),
      tip=document.getElementById('tip'), tool=document.getElementById('tool'),
      tyval=document.getElementById('tyval'), selt=document.getElementById('selt'),
      stat=document.getElementById('stat'), left=document.getElementById('left'),
      mini=document.getElementById('mini');
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
  drawSpawns(); drawMini();
}
// ---- minimap ----
const MW=280, MK=MW/(QW*16), MH=Math.max(24,Math.round(QH*16*MK));
function drawMini(){
  mini.width=MW; mini.height=MH;
  const c=mini.getContext('2d');
  c.imageSmoothingEnabled=false;
  c.drawImage(lvl,0,0,MW,MH);
  c.strokeStyle='#ff4'; c.lineWidth=1;
  const vx=left.scrollLeft/(16*Z)*16*MK, vy=left.scrollTop/(16*Z)*16*MK;
  const vw=left.clientWidth/(16*Z)*16*MK, vh=left.clientHeight/(16*Z)*16*MK;
  c.strokeRect(vx+0.5,vy+0.5,Math.min(vw,MW-1),Math.min(vh,MH-1));
}
left.onscroll=()=>drawMini();
mini.onmousedown=e=>{
  const r=mini.getBoundingClientRect();
  const px=(e.clientX-r.left)/MK, py=(e.clientY-r.top)/MK;
  left.scrollLeft=px*Z-left.clientWidth/2;
  left.scrollTop=py*Z-left.clientHeight/2;
};
// ---- spawn layer ----
const sov=document.getElementById('sov');
let SPSEL=-1, spDrag=false, placing=false;
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
  if(rectStart>=0&&rectCur>=0){
    const a=rectStart, b=rectCur;
    const ax=a%%QW, ay=(a-ax)/QW, bx=b%%QW, by=(b-bx)/QW;
    const x0=Math.min(ax,bx), y0=Math.min(ay,by);
    const x1=Math.max(ax,bx), y1=Math.max(ay,by);
    c.strokeStyle='#4f4'; c.lineWidth=2;
    c.strokeRect(x0*16*Z+1,y0*16*Z+1,(x1-x0+1)*16*Z-2,(y1-y0+1)*16*Z-2);
  }
}
const spX=document.getElementById('sp_x'), spY=document.getElementById('sp_y'),
      spP1=document.getElementById('sp_p1'), spP2=document.getElementById('sp_p2'),
      spCls=document.getElementById('sp_cls'), spAnim=document.getElementById('sp_anim'),
      spPool=document.getElementById('sp_pool');
function spCount(){ document.getElementById('spn').textContent=SPAWNS.length; }
function spForm(){
  document.getElementById('spidx').textContent=SPSEL<0?'-':SPSEL;
  spCount();
  if(SPSEL<0) return;
  const s=SPAWNS[SPSEL];
  spX.value=s.x; spY.value=s.y; spP1.value=s.half_w; spP2.value=s.half_h;
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
// ---- rectangle stamp/type ----
let rectStart=-1, rectCur=-1;
function applyRect(){
  const a=rectStart, b=rectCur;
  const ax=a%%QW, ay=(a-ax)/QW, bx=b%%QW, by=(b-bx)/QW;
  const x0=Math.min(ax,bx), y0=Math.min(ay,by);
  const x1=Math.max(ax,bx), y1=Math.max(ay,by);
  const ch=[];
  const ty=parseInt(tyval.value,16)||0;
  for(let y=y0;y<=y1;y++) for(let x=x0;x<=x1;x++){
    const q=y*QW+x, old=MAP[q];
    MAP[q]=(tool.value==='stamp') ? (old&0xFC00)|SEL
                                  : (old&0x3FF)|((ty&0x3F)<<10);
    if(MAP[q]!==old){ ch.push([q,old]); EDITED.add(q); redrawQuad(q); }
  }
  if(ch.length) hist.push({t:'rect',ch:ch});
  stat.textContent=` edits:${EDITED.size} sp:${SPEDIT.size}`;
}
map.onmousedown=e=>{
  if(placing){
    const r=map.getBoundingClientRect();
    const px=Math.max(0,Math.min(QW*16-1,((e.clientX-r.left)/Z)|0));
    const py=Math.max(0,Math.min(QH*16-1,((e.clientY-r.top)/Z)|0));
    const base=(SPSEL>=0)?SPAWNS[SPSEL]:{half_w:8,half_h:8,cls:0x1D,anim:0x20,pool:0};
    const s=Object.assign({},base,{x:px,y:py});
    SPAWNS.push(s); SPSEL=SPAWNS.length-1;
    hist.push({t:'spadd'});
    SPEDIT.add(SPSEL); placing=false;
    stat.textContent=` edits:${EDITED.size} sp:${SPEDIT.size} n=${SPAWNS.length}`;
    spForm(); drawSpawns();
    return;
  }
  if(tool.value==='spawn'){
    const i=spawnNear(e); SPSEL=i;
    if(i>=0){ hist.push({t:'sp',i:i,rec:Object.assign({},SPAWNS[i])}); spDrag=true; }
    spForm(); drawSpawns();
    return;
  }
  if(tool.value==='stamp'||tool.value==='type'){
    const q=quadAt(e); if(q<0) return;
    rectStart=rectCur=q; drawSpawns();
  }
};
window.onmouseup=()=>{
  spDrag=false;
  if(rectStart>=0&&rectCur>=0){ applyRect(); rectStart=rectCur=-1; drawSpawns(); }
};
window.onkeydown=e=>{ if(e.key==='Escape'){ placing=false; rectStart=rectCur=-1; drawSpawns(); } };
document.getElementById('spadd').onclick=()=>{
  placing=true;
  stat.textContent=' click the map to place the new spawn (Esc cancels)';
};
document.getElementById('spdel').onclick=()=>{
  if(SPSEL<0) return;
  hist.push({t:'spdel',i:SPSEL,rec:SPAWNS[SPSEL]});
  SPAWNS.splice(SPSEL,1); SPSEL=-1;
  spForm(); drawSpawns();
  stat.textContent=` edits:${EDITED.size} sp:- n=${SPAWNS.length}`;
};
document.getElementById('spapply').onclick=()=>{
  if(SPSEL<0) return;
  const s=SPAWNS[SPSEL];
  hist.push({t:'sp',i:SPSEL,rec:Object.assign({},s)});
  s.x=(+spX.value)|0; s.y=(+spY.value)|0;
  s.half_w=(+spP1.value)|0; s.half_h=(+spP2.value)|0;
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
  if(tool.value!=='pick') return;
  const q=quadAt(e); if(q<0) return;
  SEL=MAP[q]&0x3FF;
  selt.textContent=SEL.toString(16).toUpperCase();
  tyval.value=(MAP[q]>>10).toString(16).toUpperCase(); drawPal();
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
  if(rectStart>=0){
    const q=quadAt(e); if(q>=0){ rectCur=q; drawSpawns(); }
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
  else if(h.t==='rect'){ for(const [q,w] of h.ch){ MAP[q]=w; redrawQuad(q); } }
  else if(h.t==='spadd'){ SPAWNS.pop(); if(SPSEL>=SPAWNS.length)SPSEL=-1; spForm(); drawSpawns(); }
  else if(h.t==='spdel'){ SPAWNS.splice(h.i,0,h.rec); spForm(); drawSpawns(); }
  else { SPAWNS[h.i]=h.rec; if(SPSEL===h.i) spForm(); drawSpawns(); }
};
function dl(name, blob){
  const a=document.createElement('a');
  a.href=URL.createObjectURL(blob); a.download=name; a.click();
}
function buildTilemapJson(){
  const rows=[];
  for(let y=0;y<QH;y++){
    const r=[];
    for(let x=0;x<QW;x++) r.push(MAP[y*QW+x].toString(16).padStart(4,'0').toUpperCase());
    rows.push(r.join(' '));
  }
  return {format:"tilemap_u16", chunk:TM_ID, width:QW, height:QH,
          tail:TAIL, rows:rows};
}
const w2=v=>((v&0xFF).toString(16).padStart(2,'0')+((v>>8)&0xFF).toString(16).padStart(2,'0'));
const b2=v=>(v&0xFF).toString(16).padStart(2,'0');
function serializeStripe(){
  // JS mirror of level_render.serialize_stripe — every tail section is
  // terminator-scanned by the engine, so counts may change freely
  let hex=ST_HEAD.toLowerCase();
  for(const s of SPAWNS)
    hex+=w2(s.x)+w2(s.y)+w2(s.half_w)+w2(s.half_h)+w2(s.cls)+w2(s.anim)+w2(s.pool);
  hex+='ffff';
  for(const e of PAL_LIST) hex+=w2(e.chunk)+b2(e.start);
  hex+='ffff';
  hex+=w2(PAL_EN);
  for(const e of PAL_ANIMS){
    hex+=b2(e.reload)+b2(e.start)+b2(e.end);
    for(const f of e.frames) hex+=w2(f);
    hex+='ffff';
  }
  hex+='00';
  for(const e of BANKS) hex+=w2(e.chunk)+e.pad.toLowerCase();
  hex+='ffff';
  for(const e of ACHUNKS) hex+=w2(e.chunk)+e.pad.toLowerCase();
  hex+=ST_REST.toLowerCase();
  return hex;
}
function buildHeaderJson(){
  return Object.assign({},HDRJSON,{raw:serializeStripe()});
}
// live grammar invariant: the untouched page must reserialize byte-exact
globalThis.__stripe_ok = (serializeStripe()===HDRRAW.toLowerCase());
if(!globalThis.__stripe_ok){
  console.error('stripe reserialize MISMATCH — export disabled');
  document.title='!! STRIPE MISMATCH !! '+document.title;
}
document.getElementById('expjson').onclick=()=>{
  dl(TM_ID+'.json', new Blob([JSON.stringify(buildTilemapJson(),null,1)],{type:'application/json'}));
};
document.getElementById('exphdr').onclick=()=>{
  dl(HID+'.json', new Blob([JSON.stringify(buildHeaderJson(),null,1)],{type:'application/json'}));
};
document.getElementById('expbin').onclick=()=>{
  const b=new Uint8Array(MAP.length*2+TAIL.length/2);
  MAP.forEach((w,i)=>{ b[i*2]=w&0xFF; b[i*2+1]=w>>8; });
  for(let i=0;i<TAIL.length;i+=2)
    b[MAP.length*2+i/2]=parseInt(TAIL.substr(i,2),16);
  dl(TM_ID+'.bin', new Blob([b],{type:'application/octet-stream'}));
};
// ---- palette list / palette anims forms ----
// (edits apply to PAL_LIST/PAL_ANIMS directly; the level PNG is baked at
// page build time, so color changes show after save+pack+reload)
const plist=document.getElementById('plist'), panims=document.getElementById('panims');
const penInp=document.getElementById('pen');
penInp.value=PAL_EN.toString(16).toUpperCase();
penInp.onchange=()=>{ PAL_EN=parseInt(penInp.value,16)||0; };
function drawPalForms(){
  plist.innerHTML='';
  PAL_LIST.forEach((e,i)=>{
    const d=document.createElement('div');
    d.innerHTML=`chunk <input size="4" value="${e.chunk.toString(16).padStart(4,'0').toUpperCase()}">`+
      ` start <input size="3" value="${e.start}"> <button>del</button>`;
    const [ci,si]=d.querySelectorAll('input');
    ci.onchange=()=>{ e.chunk=parseInt(ci.value,16)||0; };
    si.onchange=()=>{ e.start=(+si.value)&0xFF; };
    d.querySelector('button').onclick=()=>{ PAL_LIST.splice(i,1); drawPalForms(); };
    plist.appendChild(d);
  });
  panims.innerHTML='';
  PAL_ANIMS.forEach((e,i)=>{
    const d=document.createElement('div');
    d.style.borderTop='1px solid #333';
    d.innerHTML=`reload <input size="2" value="${e.reload}">`+
      ` colors <input size="3" value="${e.start}">-<input size="3" value="${e.end}"><br>`+
      `frames(hex) <input size="18" value="${e.frames.map(f=>f.toString(16).toUpperCase()).join(' ')}">`+
      ` <button>del</button>`;
    const [ri,si,ei,fi]=d.querySelectorAll('input');
    ri.onchange=()=>{ e.reload=(+ri.value)&0xFF; if(!e.reload){e.reload=1;ri.value=1;} };
    si.onchange=()=>{ e.start=(+si.value)&0xFF; };
    ei.onchange=()=>{ e.end=(+ei.value)&0xFF; };
    fi.onchange=()=>{ e.frames=fi.value.trim()?fi.value.trim().split(/\\s+/).map(v=>parseInt(v,16)&0xFFFF):[]; };
    d.querySelector('button').onclick=()=>{ PAL_ANIMS.splice(i,1); drawPalForms(); };
    panims.appendChild(d);
  });
}
document.getElementById('pladd').onclick=()=>{ PAL_LIST.push({chunk:0,start:0}); drawPalForms(); };
document.getElementById('paadd').onclick=()=>{ PAL_ANIMS.push({reload:8,start:0,end:0,frames:[]}); drawPalForms(); };
drawPalForms();
// ---- server mode ----
const srvstat=document.getElementById('srvstat');
async function api(path, body){
  const r=await fetch(path,{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify(body||{})});
  const js=await r.json().catch(()=>({}));
  if(!r.ok||js.ok===false) throw new Error(js.error||r.status);
  return js;
}
async function srvSave(){
  srvstat.textContent='saving...';
  await api('/api/save',{kind:'tilemap',chunk:TM_ID,data:buildTilemapJson()});
  await api('/api/save',{kind:'header',chunk:HID,data:buildHeaderJson()});
  srvstat.textContent='saved';
}
async function srvPack(){
  srvstat.textContent='packing...';
  const js=await api('/api/pack');
  srvstat.textContent='packed '+js.packed;
}
async function srvPlay(){
  srvstat.textContent='launching...';
  const js=await api('/api/play');
  srvstat.textContent='game pid '+js.pid;
}
function wrap(f){ return ()=>f().catch(e=>{srvstat.textContent='ERR '+e.message;}); }
if(SERVER){
  document.getElementById('srv').style.display='';
  document.getElementById('srvsave').onclick=wrap(srvSave);
  document.getElementById('srvpack').onclick=wrap(srvPack);
  document.getElementById('srvplay').onclick=wrap(srvPlay);
  document.getElementById('srvall').onclick=wrap(async()=>{
    await srvSave(); await srvPack(); await srvPlay();
  });
}
</script>
"""


def render_page(cid, server=False, root=None):
    """Build the editor HTML for level header chunk `cid`.
    root: open assets tree to read the CURRENT content from (the server's
    scratch copy) — hand edits win over the archive, same rule as
    assetc.pack. None = pristine archive content."""
    raw = LR.header_raw(cid, root)
    qw, qh, tm_id, ts_id, gt_id, pal_entries, spawns = LR.parse_header(raw)
    # the page holds the FULL stripe structure (parse_stripe grammar,
    # 44/44 byte-exact roundtrip) and reserializes it on export — spawn
    # count, palette list and palette animations may all change freely
    st = LR.parse_stripe(raw)
    assert LR.serialize_stripe(st) == raw, "stripe grammar mismatch"
    spawns = st["spawns"]
    classes = {}
    script_id = LR.script_for_header(int(cid, 16))
    if script_id is not None:
        script_raw = LR.open_payload(script_id, "lzss", root)
        for sp in spawns:
            if sp["cls"] not in classes:
                rec = LR.class_record(script_raw, sp["cls"])
                if rec:
                    classes[sp["cls"]] = rec
    tmap = LR.open_payload(tm_id, "lzss", root)
    tgfx = LR.open_payload(ts_id, "lzss", root)
    gtld = LR.open_payload(gt_id, "lzss", root)
    pal = LR.compose_palette(pal_entries, root)
    words = [tmap[i * 2] | (tmap[i * 2 + 1] << 8) for i in range(qw * qh)]
    tail = tmap[qw * qh * 2:].hex().upper()
    png, _, _ = LR.render(cid, root)
    apng, ntpl, pcols = render_template_atlas(gtld, tgfx, pal)
    base = root if root is not None else os.path.join(LR.ROOT, "assets")
    with open(os.path.join(base, "level_headers", f"{cid}.json")) as f:
        hdr_named = {k: v for k, v in json.load(f).items() if k != "raw"}
    return PAGE % {
        "cid": cid, "qw": qw, "qh": qh, "ntpl": ntpl, "pcols": pcols,
        "nsp": len(spawns),
        "npal": len(st["pal_list"]), "npanim": len(st["pal_anims"]),
        "server": 1 if server else 0,
        "tmid": f"{tm_id:04X}",
        "png": base64.b64encode(png).decode(),
        "apng": base64.b64encode(apng).decode(),
        "map": json.dumps(words, separators=(",", ":")),
        "tail": tail,
        "spawns": json.dumps(spawns, separators=(",", ":")),
        "classes": json.dumps(classes, separators=(",", ":")),
        "icons": json.dumps(LR.load_class_icons(script_id),
                            separators=(",", ":")),
        "sthead": st["head"], "strest": st["rest"],
        "palen": st["pal_anim_en"],
        "pallist": json.dumps(st["pal_list"], separators=(",", ":")),
        "palanims": json.dumps(st["pal_anims"], separators=(",", ":")),
        "banks": json.dumps(st["sprite_banks"], separators=(",", ":")),
        "achunks": json.dumps(st["anim_chunks"], separators=(",", ":")),
        "hdrraw": raw.hex(),
        "hdrjson": json.dumps(hdr_named, separators=(",", ":")),
        "hid": cid,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("header")
    ap.add_argument("-o", "--out")
    args = ap.parse_args()
    cid = args.header
    html = render_page(cid)
    out = args.out or f"/tmp/edit_{cid}.html"
    with open(out, "w") as f:
        f.write(html)
    print(f"{cid}: -> {out}")


if __name__ == "__main__":
    main()
