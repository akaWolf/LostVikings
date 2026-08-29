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
  - TEMPLATE CONSTRUCTOR: compose a quad template from any 4 tileset
    tiles with per-corner h/v flips (1689E draw-word model, bits 0-3
    preserved), edit an existing template or append a NEW one (grows the
    gtld chunk; proven in-game), map/palette redraw via overrides;
  - palette-list and palette-anim forms (stripe grammar sections);
  - undo, EXPORT (tilemap .json/.bin, header .json — assetc-compatible);
  - SERVER mode (edit_server.py): save/pack/play buttons drive the
    scratch assets tree end-to-end without downloads (gtld saves rebuild
    the bg_tilesets png+json pair via the role converter).

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


def render_tile_atlas(tgfx, pal, cols=16):
    """All 8x8 tiles of the tileset as a PNG atlas (16/row), level palette."""
    n = len(tgfx) // 64
    rows = (n + cols - 1) // cols
    stride = cols * 8
    img = bytearray(stride * rows * 8)
    for i in range(n):
        px = tile_decode(tgfx[i * 64:(i + 1) * 64])
        bx, by = (i % cols) * 8, (i // cols) * 8
        for y in range(8):
            img[(by + y) * stride + bx:(by + y) * stride + bx + 8] = \
                px[y * 8:(y + 1) * 8]
    return png_write(stride, rows * 8, bytes(img), pal), n, cols


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
 <option value="spawn">spawn</option><option value="select">select</option></select>
 <span style="color:#888">[S/T/P/W/E, ^Z undo, ^C/^V copy/paste]</span>
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
  <details style="margin-bottom:8px"><summary>level header fields</summary>
   viking spawn x <input id="hd_vx" size="4"> y <input id="hd_vy" size="4"><br>
   spawn code <input id="hd_code" size="3"> anim <input id="hd_anim" size="4">
   pool0 <input id="hd_pool" size="3"> (hex)<br>
   active vk <input id="hd_avk" size="2">
   music type <input id="hd_mt" size="2"> track <input id="hd_trk" size="4"> flag <input id="hd_mf" size="2"><br>
   anim scroll dx <input id="hd_adx" size="4"> dy <input id="hd_ady" size="4"><br>
   level flags <input id="hd_fl" size="2"> (hex; bit0 HUD)<br>
   chunks: map <input id="hd_tm" size="4"> tiles <input id="hd_ts" size="4">
   tpls <input id="hd_gt" size="4"> (hex; applies after save+reload)<br>
   dims: %(qw)dx%(qh)d quads (resize: level resize section)
  </details>
  <details style="margin-bottom:8px"><summary>tile pixels (tile <span id="txsel">-</span>)</summary>
   <canvas id="tgrid" style="display:inline-block;vertical-align:top"></canvas>
   <canvas id="tsw" style="display:inline-block;margin-left:6px"></canvas><br>
   color <span id="txcol">0</span>
   <label><input type="checkbox" id="txmask"> mask layer</label>
   <button id="txload">load sel tile</button>
   <button id="txnew">add new tile</button>
   <span style="color:#888">(LMB paint, RMB pick; mask: LMB opaque, RMB clear)</span>
  </details>
  <details style="margin-bottom:8px"><summary>template editor (tpl <span id="tesel">-</span>)</summary>
   corners (click one, then click a tile below):<br>
   <div id="tecorners"></div>
   preview: <canvas id="tprev" style="display:inline-block;vertical-align:middle"></canvas>
   <button id="teload">load sel tpl</button>
   <button id="teapply">apply to sel</button>
   <button id="teadd">add as new</button><br>
   tiles (%(ntiles)d):<canvas id="tiles"></canvas>
  </details>
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
const ST_REST="%(strest)s", HDRRAW="%(hdrraw)s";
const HEADB=Uint8Array.from("%(sthead)s".match(/../g)||[], h=>parseInt(h,16));
const PALCHUNKS=%(palchunks)s; // palette chunk payloads (hex), all levels
let PAL_EN=%(palen)d;
const PAL_LIST=%(pallist)s, PAL_ANIMS=%(palanims)s;
const BANKS=%(banks)s, ACHUNKS=%(achunks)s;
const GT_ID="%(gtid)s", NTILES=%(ntiles)d, TCOLS=%(tcols)d;
let GTLD=%(gtld)s;   // draw words, 4 per template (1689E: bits 15..6 tile
                     // byte offset, bit4 hflip, bit5 vflip, bits 0-3 kept)
const GT_REST="%(gtrest)s", GTLDHEX="%(gtldhex)s";
const tilesImg=new Image(); tilesImg.src="data:image/png;base64,%(tpng)s";
const TS_ID="%(tsid)s", TGFX_TAIL="%(tgfxtail)s", PALRGB=%(palrgb)s;
const TGFX=Uint8Array.from(atob("%(tgfxb64)s"), c=>c.charCodeAt(0));
const TMASK_ID="%(tmid_mask)s", TMASK_TAIL="%(tmasktail)s";
const TMASK_SRC=Uint8Array.from(atob("%(tmaskb64)s"), c=>c.charCodeAt(0));
let MASKS=Array.from({length:NTILES},(_,i)=>TMASK_SRC.slice(i*8,(i+1)*8));
// mask bit(tx,ty) = byte (tx&3)*2+(ty>>2), bit 7-((ty&3)*2+(tx>>2));
// 1 = opaque (assetc TileMasks, v2_render_tile_masked model)
const mbit=(t,x,y)=>(MASKS[t][(x&3)*2+(y>>2)]>>(7-((y&3)*2+(x>>2))))&1;
const mbitw=(t,x,y,v)=>{
  const mb=(x&3)*2+(y>>2), bit=7-((y&3)*2+(x>>2));
  if(v) MASKS[t][mb]|=(1<<bit); else MASKS[t][mb]&=~(1<<bit);
};
let TILES=Array.from({length:NTILES},(_,i)=>TGFX.slice(i*64,(i+1)*64));
// pixel(x,y) = t[(x&3)*16 + y*2 + (x>>2)]  (verified sub_1689e model)
const tpix=(t,x,y)=>TILES[t][(x&3)*16+y*2+(x>>2)];
const tpixw=(t,x,y,v)=>{ TILES[t][(x&3)*16+y*2+(x>>2)]=v; };
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
  const ov=OVERRIDE.get(t);
  if(ov) mc.drawImage(ov,0,0,16,16,qx*16*Z,qy*16*Z,16*Z,16*Z);
  else mc.drawImage(atlas,(t%%PCOLS)*16,((t/PCOLS)|0)*16,16,16,
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
  if(SELRECT){
    c.strokeStyle='#48f'; c.lineWidth=2; c.setLineDash([6,4]);
    c.strokeRect(SELRECT.x0*16*Z+1,SELRECT.y0*16*Z+1,
                 (SELRECT.x1-SELRECT.x0+1)*16*Z-2,(SELRECT.y1-SELRECT.y0+1)*16*Z-2);
    c.setLineDash([]);
  }
  if(pasteQ>=0&&CLIP){
    const qx=pasteQ%%QW, qy=(pasteQ-qx)/QW;
    c.strokeStyle='#fa0'; c.lineWidth=2; c.setLineDash([3,3]);
    c.strokeRect(qx*16*Z+1,qy*16*Z+1,CLIP.w*16*Z-2,CLIP.h*16*Z-2);
    c.setLineDash([]);
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
  if(pasting&&CLIP){
    const q=quadAt(e); if(q<0) return;
    applyPaste(q); return;
  }
  if(tool.value==='spawn'){
    const i=spawnNear(e); SPSEL=i;
    if(i>=0){ hist.push({t:'sp',i:i,rec:Object.assign({},SPAWNS[i])}); spDrag=true; }
    spForm(); drawSpawns();
    return;
  }
  if(tool.value==='select'){
    const q=quadAt(e); if(q<0) return;
    selStart=q; SELRECT=null; rectStart=rectCur=-1;
    selCur=q; updateSel(); drawSpawns();
    return;
  }
  if(tool.value==='stamp'||tool.value==='type'){
    const q=quadAt(e); if(q<0) return;
    rectStart=rectCur=q; drawSpawns();
  }
};
let selStart=-1, selCur=-1, SELRECT=null, CLIP=null, pasting=false, pasteQ=-1;
function updateSel(){
  if(selStart<0||selCur<0) return;
  const ax=selStart%%QW, ay=(selStart-ax)/QW, bx=selCur%%QW, by=(selCur-bx)/QW;
  SELRECT={x0:Math.min(ax,bx),y0:Math.min(ay,by),
           x1:Math.max(ax,bx),y1:Math.max(ay,by)};
}
function copySel(){
  if(!SELRECT){ stat.textContent=' select a region first (E tool)'; return; }
  const w=SELRECT.x1-SELRECT.x0+1, h=SELRECT.y1-SELRECT.y0+1;
  const words=[];
  for(let y=SELRECT.y0;y<=SELRECT.y1;y++)
    for(let x=SELRECT.x0;x<=SELRECT.x1;x++) words.push(MAP[y*QW+x]);
  CLIP={gt:GT_ID,w:w,h:h,words:words};
  try{ localStorage.setItem('lv_clip', JSON.stringify(CLIP)); }catch(_e){}
  stat.textContent=` copied ${w}x${h}`;
}
function startPaste(){
  if(!CLIP){
    try{ CLIP=JSON.parse(localStorage.getItem('lv_clip')||'null'); }catch(_e){}
  }
  if(!CLIP){ stat.textContent=' clipboard empty'; return; }
  if(CLIP.gt!==GT_ID){
    stat.textContent=` clip is from another world (gtld ${CLIP.gt}) — template indices would be garbage here`;
    return;
  }
  pasting=true; pasteQ=-1;
  stat.textContent=` pasting ${CLIP.w}x${CLIP.h} — click to place, Esc cancels`;
}
function applyPaste(q){
  const qx=q%%QW, qy=(q-qx)/QW;
  const ch=[];
  for(let y=0;y<CLIP.h;y++) for(let x=0;x<CLIP.w;x++){
    const dx=qx+x, dy=qy+y;
    if(dx>=QW||dy>=QH) continue;
    const dq=dy*QW+dx, nv=CLIP.words[y*CLIP.w+x];
    if(MAP[dq]!==nv){ ch.push([dq,MAP[dq]]); MAP[dq]=nv; EDITED.add(dq); redrawQuad(dq); }
  }
  if(ch.length) hist.push({t:'rect',ch:ch});
  pasting=false; pasteQ=-1; drawSpawns();
  stat.textContent=` pasted (${ch.length} quads changed)`;
}
window.onmouseup=()=>{
  spDrag=false;
  selStart=-1;
  if(rectStart>=0&&rectCur>=0){ applyRect(); rectStart=rectCur=-1; drawSpawns(); }
};
window.onkeydown=e=>{
  if(e.key==='Escape'){
    placing=false; pasting=false; pasteQ=-1;
    rectStart=rectCur=-1; SELRECT=null; drawSpawns();
    return;
  }
  const tag=(e.target&&e.target.tagName)||'';
  if(tag==='INPUT'||tag==='TEXTAREA'||tag==='SELECT') return;
  if((e.ctrlKey||e.metaKey)&&e.key.toLowerCase()==='z'){
    e.preventDefault(); document.getElementById('undo').click(); return;
  }
  if((e.ctrlKey||e.metaKey)&&e.key.toLowerCase()==='c'){
    e.preventDefault(); copySel(); return;
  }
  if((e.ctrlKey||e.metaKey)&&e.key.toLowerCase()==='v'){
    e.preventDefault(); startPaste(); return;
  }
  const tools={s:'stamp',t:'type',p:'pick',w:'spawn',e:'select'};
  const k=e.key.toLowerCase();
  if(tools[k]){ tool.value=tools[k]; }
};
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
  const n=Math.max(NT,ntplNow());
  pal.width=PCOLS*16; pal.height=Math.ceil(n/PCOLS)*16+4;
  pc.imageSmoothingEnabled=false; pc.drawImage(atlas,0,0);
  for(const [t,cv] of OVERRIDE)
    pc.drawImage(cv,(t%%PCOLS)*16,((t/PCOLS)|0)*16);
  pc.strokeStyle='#ff4';
  pc.strokeRect((SEL%%PCOLS)*16+0.5,((SEL/PCOLS)|0)*16+0.5,15,15);
}
lvl.onload=()=>{ drawAll(); };
atlas.onload=()=>{ drawPal(); };
document.getElementById('z').onchange=e=>{ Z=+e.target.value; drawAll(); };
pal.onclick=e=>{
  const r=pal.getBoundingClientRect();
  const tx=((e.clientX-r.left)/16)|0, ty=((e.clientY-r.top)/16)|0;
  const t=ty*PCOLS+tx;
  if(t<ntplNow()){ SEL=t; selt.textContent=t.toString(16).toUpperCase(); drawPal(); teLoad(t); }
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
  if(selStart>=0&&e.buttons&1){
    const q=quadAt(e); if(q>=0){ selCur=q; updateSel(); drawSpawns(); }
    return;
  }
  if(pasting){
    const q=quadAt(e); if(q>=0&&q!==pasteQ){ pasteQ=q; drawSpawns(); }
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
  else if(h.t==='tile'){ TILES[h.i]=h.px; MASKS[h.i]=h.mk;
    if(TXSEL===h.i) drawTgrid(); tileTouched(h.i); }
  else if(h.t==='pal'){ PAL_LIST.length=0; PAL_LIST.push(...h.pl);
    PAL_ANIMS.length=0; PAL_ANIMS.push(...h.pa); PAL_EN=h.en;
    penInp.value=PAL_EN.toString(16).toUpperCase(); drawPalForms();
    if(LIVE_PAL) palLive(); }
  else if(h.t==='head'){ HEADB.set(h.bytes); headForm(); }
  else if(h.t==='gtld'){ for(let k=0;k<4;k++) GTLD[h.i*4+k]=h.words[k];
    OVERRIDE.set(h.i, renderTplCanvas(h.i)); drawPal();
    for(let q=0;q<QW*QH;q++) if((MAP[q]&0x3FF)===h.i) redrawQuad(q); }
  else if(h.t==='gtldadd'){ GTLD.length-=4; OVERRIDE.delete(ntplNow());
    if(SEL>=ntplNow()) SEL=0; drawPal(); }
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
  let hex='';
  HEADB.forEach(v=>{hex+=b2(v);});
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
// ---- tile pixel editor ----
const TILE_DIRTY=new Set();
let TXSEL=0, TXCOL=0;
const tgrid=document.getElementById('tgrid'), tsw=document.getElementById('tsw');
function tileToCanvas(t, scale){
  const c=document.createElement('canvas'); c.width=8*scale; c.height=8*scale;
  const g=c.getContext('2d');
  for(let y=0;y<8;y++) for(let x=0;x<8;x++){
    const [r,gg,b]=PALRGB[tpix(t,x,y)];
    g.fillStyle=`rgb(${r},${gg},${b})`;
    g.fillRect(x*scale,y*scale,scale,scale);
  }
  return c;
}
function drawTgrid(){
  document.getElementById('txsel').textContent=TXSEL.toString(16).toUpperCase();
  tgrid.width=8*16; tgrid.height=8*16;
  const g=tgrid.getContext('2d');
  g.drawImage(tileToCanvas(TXSEL,16),0,0);
  if(document.getElementById('txmask').checked){
    // mask overlay: transparent pixels hatched magenta
    g.fillStyle='rgba(255,0,255,0.55)';
    for(let y=0;y<8;y++) for(let x=0;x<8;x++)
      if(!mbit(TXSEL,x,y)) g.fillRect(x*16,y*16,16,16);
  }
  g.strokeStyle='#333';
  for(let i=0;i<=8;i++){
    g.beginPath(); g.moveTo(i*16,0); g.lineTo(i*16,128); g.stroke();
    g.beginPath(); g.moveTo(0,i*16); g.lineTo(128,i*16); g.stroke();
  }
}
document.getElementById('txmask').onchange=()=>drawTgrid();
function drawSwatch(){
  tsw.width=16*8; tsw.height=16*8;
  const g=tsw.getContext('2d');
  for(let i=0;i<256;i++){
    const [r,gg,b]=PALRGB[i];
    g.fillStyle=`rgb(${r},${gg},${b})`;
    g.fillRect((i%%16)*8,((i/16)|0)*8,8,8);
  }
  g.strokeStyle='#fff';
  g.strokeRect((TXCOL%%16)*8+0.5,((TXCOL/16)|0)*8+0.5,7,7);
  document.getElementById('txcol').textContent=TXCOL.toString(16).toUpperCase();
}
tsw.onclick=e=>{
  const r=tsw.getBoundingClientRect();
  TXCOL=(((e.clientY-r.top)/8)|0)*16+(((e.clientX-r.left)/8)|0)&0xFF;
  drawSwatch();
};
function tileTouched(t){
  TILE_DIRTY.add(t);
  // refresh the tile atlas cell + every template using this tile + map quads
  const g=tiles.getContext('2d');
  g.drawImage(tileToCanvas(t,2),(t%%TCOLS)*16,((t/TCOLS)|0)*16);
  const touched=[];
  for(let tp=0;tp<ntplNow();tp++)
    for(let k=0;k<4;k++)
      if(((GTLD[tp*4+k]&0xFFC0)>>6)===t){ touched.push(tp); break; }
  for(const tp of touched){
    OVERRIDE.set(tp, renderTplCanvasPix(tp));
    for(let q=0;q<QW*QH;q++) if((MAP[q]&0x3FF)===tp) redrawQuad(q);
  }
  if(touched.length) drawPal();
  stat.textContent=` edits:${EDITED.size} sp:${SPEDIT.size} tiles:${TILE_DIRTY.size}`;
}
let painting=false;
function tgridPaint(e){
  const r=tgrid.getBoundingClientRect();
  const x=((e.clientX-r.left)/16)|0, y=((e.clientY-r.top)/16)|0;
  if(x<0||y<0||x>7||y>7) return;
  if(document.getElementById('txmask').checked){
    const v=(e.buttons&2)?0:1;
    if(mbit(TXSEL,x,y)===v) return;
    mbitw(TXSEL,x,y,v);
    TILE_DIRTY.add(TXSEL); drawTgrid();
    return;
  }
  if(e.buttons&2){ TXCOL=tpix(TXSEL,x,y); drawSwatch(); return; }
  if(tpix(TXSEL,x,y)===TXCOL) return;
  tpixw(TXSEL,x,y,TXCOL);
  drawTgrid(); tileTouched(TXSEL);
}
tgrid.onmousedown=e=>{
  painting=true;
  // one undo entry per stroke (pixels + mask snapshot of this tile)
  hist.push({t:'tile',i:TXSEL,
             px:TILES[TXSEL].slice(),mk:MASKS[TXSEL].slice()});
  tgridPaint(e); e.preventDefault();
};
tgrid.onmousemove=e=>{ if(painting) tgridPaint(e); };
tgrid.oncontextmenu=e=>e.preventDefault();
window.addEventListener('mouseup',()=>{ painting=false; });
document.getElementById('txload').onclick=()=>{
  // load the tile of the current TE corner (template editor selection)
  TXSEL=((TE.cells[TE.cur].dw|0)&0xFFC0)>>6;
  drawTgrid();
};
document.getElementById('txnew').onclick=()=>{
  if(TILES.length>=1023){ stat.textContent=' tile limit 1023 (10-bit offset)'; return; }
  TILES.push(new Uint8Array(64));
  MASKS.push(new Uint8Array(8).fill(0xFF));   // new tile: fully opaque
  TXSEL=TILES.length-1;
  const rows=Math.ceil(TILES.length/TCOLS);
  if(tiles.height<rows*16){
    const old=document.createElement('canvas');
    old.width=tiles.width; old.height=tiles.height;
    old.getContext('2d').drawImage(tiles,0,0);
    tiles.height=rows*16;
    const g=tiles.getContext('2d'); g.imageSmoothingEnabled=false;
    g.drawImage(old,0,0);
  }
  TILE_DIRTY.add(TXSEL);
  drawTgrid(); tileTouched(TXSEL);
};
function buildTilesetB64(){
  const out=new Uint8Array(TILES.length*64);
  TILES.forEach((t,i)=>out.set(t,i*64));
  let bin=''; out.forEach(v=>{bin+=String.fromCharCode(v);});
  return {chunk:TS_ID, b64:btoa(bin), tail:TGFX_TAIL};
}
function buildMasksB64(){
  const out=new Uint8Array(MASKS.length*8);
  MASKS.forEach((m,i)=>out.set(m,i*8));
  let bin=''; out.forEach(v=>{bin+=String.fromCharCode(v);});
  return {chunk:TMASK_ID, b64:btoa(bin), tail:TMASK_TAIL};
}
// tileset self-check: untouched tiles must equal the source bytes
{
  let ok=(TILES.length*64===TGFX.length);
  if(ok) for(let i=0;i<TGFX.length;i++)
    if(TILES[i>>6][i&63]!==TGFX[i]){ ok=false; break; }
  globalThis.__tiles_ok=ok;
  if(!ok) console.error('tileset mirror MISMATCH');
  let mok=(MASKS.length*8===TMASK_SRC.length);
  if(mok) for(let i=0;i<TMASK_SRC.length;i++)
    if(MASKS[i>>3][i&7]!==TMASK_SRC[i]){ mok=false; break; }
  globalThis.__tmask_ok=mok;
  if(!mok) console.error('tile masks mirror MISMATCH');
}
// ---- template constructor ----
const OVERRIDE=new Map();   // tpl -> 16x16 canvas (edited templates)
function ntplNow(){ return GTLD.length/4; }
function renderTplCanvasPix(t){
  // from LIVE tile bytes (edited tiles included), not the baked PNG atlas
  const c=document.createElement('canvas'); c.width=16; c.height=16;
  const g=c.getContext('2d');
  for(let k=0;k<4;k++){
    const dw=GTLD[t*4+k];
    const idx=(dw&0xFFC0)>>6, hf=(dw>>4)&1, vf=(dw>>5)&1;
    const dx=(k&1)*8, dy=(k>>1)*8;
    if(idx>=TILES.length) continue;
    for(let y=0;y<8;y++) for(let x=0;x<8;x++){
      const sx=hf?7-x:x, sy=vf?7-y:y;
      const [r,gg,b]=PALRGB[tpix(idx,sx,sy)];
      g.fillStyle=`rgb(${r},${gg},${b})`;
      g.fillRect(dx+x,dy+y,1,1);
    }
  }
  return c;
}
const renderTplCanvas=renderTplCanvasPix;
// GTLD word order per template: e[0],e[2] = top row, e[4],e[6] = bottom
// (sub_173C7) -> word index k: 0=TL 1=TR 2=BL 3=BR.
const TE={cur:0, cells:[{},{},{},{}]};
function teSync(){
  for(let k=0;k<4;k++){
    const dw=TE.cells[k].dw|0;
    const el=document.getElementById('tec'+k);
    el.style.outline=(k===TE.cur)?'2px solid #ff4':'1px solid #444';
    el.querySelector('input.ti').value=((dw&0xFFC0)>>6).toString(16).toUpperCase();
    el.querySelector('input.hf').checked=!!(dw&0x10);
    el.querySelector('input.vf').checked=!!(dw&0x20);
  }
  const pv=document.getElementById('tprev');
  pv.width=32; pv.height=32;
  const g=pv.getContext('2d'); g.imageSmoothingEnabled=false;
  for(let k=0;k<4;k++){
    const dw=TE.cells[k].dw|0;
    const idx=(dw&0xFFC0)>>6, hf=(dw>>4)&1, vf=(dw>>5)&1;
    const sx=(idx%%TCOLS)*8, sy=((idx/TCOLS)|0)*8;
    const dx=(k&1)*16, dy=(k>>1)*16;
    g.save();
    g.translate(dx+(hf?16:0), dy+(vf?16:0));
    g.scale(hf?-2:2, vf?-2:2);
    g.drawImage(tilesImg, sx,sy,8,8, 0,0,8,8);
    g.restore();
  }
}
function teInitCells(){
  const box=document.getElementById('tecorners');
  const names=['TL','TR','BL','BR'];
  box.innerHTML='';
  for(let k=0;k<4;k++){
    const d=document.createElement('span');
    d.id='tec'+k;
    d.style.cssText='display:inline-block;margin:2px;padding:2px';
    d.innerHTML=`${names[k]} <input class="ti" size="3">`+
      `<label><input class="hf" type="checkbox">h</label>`+
      `<label><input class="vf" type="checkbox">v</label>`;
    d.onclick=()=>{ TE.cur=k; teSync(); };
    const ti=d.querySelector('input.ti');
    ti.onchange=()=>{ const idx=Math.min(NTILES-1,parseInt(ti.value,16)||0);
      TE.cells[k].dw=(TE.cells[k].dw&0x3F)|(idx<<6); teSync(); };
    d.querySelector('input.hf').onchange=e2=>{
      TE.cells[k].dw=(TE.cells[k].dw&~0x10)|(e2.target.checked?0x10:0); teSync(); };
    d.querySelector('input.vf').onchange=e2=>{
      TE.cells[k].dw=(TE.cells[k].dw&~0x20)|(e2.target.checked?0x20:0); teSync(); };
    box.appendChild(d);
  }
}
teInitCells(); drawTgrid(); drawSwatch();
function teLoad(t){
  document.getElementById('tesel').textContent=t.toString(16).toUpperCase();
  for(let k=0;k<4;k++) TE.cells[k].dw=GTLD[t*4+k];
  teSync();
}
function teWrite(t){
  for(let k=0;k<4;k++) GTLD[t*4+k]=TE.cells[k].dw&0xFFFF;
  OVERRIDE.set(t, renderTplCanvas(t));
  drawPal();
  for(let q=0;q<QW*QH;q++) if((MAP[q]&0x3FF)===t) redrawQuad(q);
  stat.textContent=` edits:${EDITED.size} sp:${SPEDIT.size} tpl:${OVERRIDE.size}`;
}
document.getElementById('teload').onclick=()=>teLoad(SEL);
document.getElementById('teapply').onclick=()=>{
  hist.push({t:'gtld',i:SEL,words:GTLD.slice(SEL*4,SEL*4+4)});
  teWrite(SEL);
};
document.getElementById('teadd').onclick=()=>{
  if(ntplNow()>=1024){ stat.textContent=' template limit 1024 (10-bit index)'; return; }
  const t=ntplNow();
  GTLD.push(0,0,0,0);
  hist.push({t:'gtldadd'});
  teWrite(t); SEL=t;
  selt.textContent=t.toString(16).toUpperCase();
  document.getElementById('tesel').textContent=selt.textContent;
  drawPal();
};
const tiles=document.getElementById('tiles');
tilesImg.onload=()=>{
  tiles.width=TCOLS*8*2; tiles.height=Math.ceil(NTILES/TCOLS)*8*2;
  const g=tiles.getContext('2d'); g.imageSmoothingEnabled=false;
  g.drawImage(tilesImg,0,0,tiles.width,tiles.height);
};
tiles.onclick=e=>{
  const r=tiles.getBoundingClientRect();
  const tx=((e.clientX-r.left)/16)|0, ty=((e.clientY-r.top)/16)|0;
  const idx=ty*TCOLS+tx;
  if(idx>=NTILES) return;
  TE.cells[TE.cur].dw=(TE.cells[TE.cur].dw&0x3F)|(idx<<6);
  TE.cur=(TE.cur+1)&3;  // convenience: advance to the next corner
  teSync();
};
function buildGtldWords(){ return {chunk:GT_ID, words:GTLD, rest:GT_REST}; }
// gtld self-check: untouched words must rebuild the chunk hex
{
  let hx='';
  for(const w of GTLD) hx+=w2(w);
  hx+=GT_REST.toLowerCase();
  globalThis.__gtld_ok = (hx===GTLDHEX.toLowerCase());
  if(!globalThis.__gtld_ok) console.error('gtld reserialize MISMATCH');
}
// ---- palette list / palette anims forms ----
// (edits apply to PAL_LIST/PAL_ANIMS directly; the level PNG is baked at
// page build time, so color changes show after save+pack+reload)
const plist=document.getElementById('plist'), panims=document.getElementById('panims');
const penInp=document.getElementById('pen');
penInp.value=PAL_EN.toString(16).toUpperCase();
function pushPalUndo(){
  hist.push({t:'pal',
    pl:PAL_LIST.map(e=>Object.assign({},e)),
    pa:PAL_ANIMS.map(e=>Object.assign({},e,{frames:e.frames.slice()})),
    en:PAL_EN});
}
penInp.onchange=()=>{ pushPalUndo(); PAL_EN=parseInt(penInp.value,16)||0; };
function drawPalForms(){
  plist.innerHTML='';
  PAL_LIST.forEach((e,i)=>{
    const d=document.createElement('div');
    d.innerHTML=`chunk <input size="4" value="${e.chunk.toString(16).padStart(4,'0').toUpperCase()}">`+
      ` start <input size="3" value="${e.start}"> <button>del</button>`;
    const [ci,si]=d.querySelectorAll('input');
    ci.onchange=()=>{ pushPalUndo(); e.chunk=parseInt(ci.value,16)||0; palLive(); };
    si.onchange=()=>{ pushPalUndo(); e.start=(+si.value)&0xFF; palLive(); };
    d.querySelector('button').onclick=()=>{ pushPalUndo(); PAL_LIST.splice(i,1); drawPalForms(); palLive(); };
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
    ri.onchange=()=>{ pushPalUndo(); e.reload=(+ri.value)&0xFF; if(!e.reload){e.reload=1;ri.value=1;} };
    si.onchange=()=>{ pushPalUndo(); e.start=(+si.value)&0xFF; };
    ei.onchange=()=>{ pushPalUndo(); e.end=(+ei.value)&0xFF; };
    fi.onchange=()=>{ pushPalUndo(); e.frames=fi.value.trim()?fi.value.trim().split(/\\s+/).map(v=>parseInt(v,16)&0xFFFF):[]; };
    d.querySelector('button').onclick=()=>{ pushPalUndo(); PAL_ANIMS.splice(i,1); drawPalForms(); };
    panims.appendChild(d);
  });
}
document.getElementById('pladd').onclick=()=>{ pushPalUndo(); PAL_LIST.push({chunk:0,start:0}); drawPalForms(); };
document.getElementById('paadd').onclick=()=>{ pushPalUndo(); PAL_ANIMS.push({reload:8,start:0,end:0,frames:[]}); drawPalForms(); };
drawPalForms();
// ---- level header (stripe head) form ----
// stripe offsets = DS offset - 0x25B3 (v2_ds_layout names):
// +0/2 anim scroll dx/dy, +4 music type (b), +5 track (w), +6 flag (b,
// overlaps track hi), +7 active vk (b), +8/A viking spawn X/Y, +C code,
// +E anim, +10 pool0, +1C level flags (b), +2E/30/32 chunk ids.
const hg16=o=>HEADB[o]|(HEADB[o+1]<<8);
const hp16=(o,v)=>{HEADB[o]=v&0xFF;HEADB[o+1]=(v>>8)&0xFF;};
const HDF=[
  ['hd_vx',0x08,2,10],['hd_vy',0x0A,2,10],['hd_code',0x0C,2,16],
  ['hd_anim',0x0E,2,16],['hd_pool',0x10,2,16],['hd_avk',0x07,1,16],
  ['hd_mt',0x04,1,16],['hd_trk',0x05,2,16],['hd_mf',0x06,1,16],
  ['hd_adx',0x00,2,10],['hd_ady',0x02,2,10],['hd_fl',0x1C,1,16],
  ['hd_tm',0x2E,2,16],['hd_ts',0x30,2,16],['hd_gt',0x32,2,16]];
function headForm(){
  for(const [id,o,w,base] of HDF){
    const v=(w===2)?hg16(o):HEADB[o];
    document.getElementById(id).value=(base===16)?v.toString(16).toUpperCase():v;
  }
}
for(const [id,o,w,base] of HDF){
  document.getElementById(id).onchange=e2=>{
    hist.push({t:'head',bytes:HEADB.slice()});
    const v=(base===16)?(parseInt(e2.target.value,16)||0):((+e2.target.value)|0);
    if(w===2) hp16(o,v&0xFFFF); else HEADB[o]=v&0xFF;
  };
}
headForm();
// ---- live palette (JS mirror of compose_palette: entries into a 768B
// buffer at start*3, then black out colors 16k, 6-bit <<2|>>4) ----
let LIVE_PAL=false;
function composePalJS(){
  const buf=new Uint8Array(768);
  let missing=null;
  for(const e of PAL_LIST){
    const hx=PALCHUNKS[e.chunk.toString(16).padStart(4,'0').toUpperCase()];
    if(hx===undefined){ missing=e.chunk; continue; }
    for(let i=0;i<hx.length/2;i++){
      const o=e.start*3+i;
      if(o<768) buf[o]=parseInt(hx.substr(i*2,2),16);
    }
  }
  for(let k=1;k<16;k++){ buf[k*48]=0; buf[k*48+1]=0; buf[k*48+2]=0; }
  for(let i=0;i<256;i++)
    PALRGB[i]=[(buf[i*3]<<2)|(buf[i*3]>>4),
               (buf[i*3+1]<<2)|(buf[i*3+1]>>4),
               (buf[i*3+2]<<2)|(buf[i*3+2]>>4)];
  return missing;
}
// self-check: recomposing the UNTOUCHED palette list must reproduce the
// python-baked PALRGB exactly (compose_palette mirror proof)
{
  const baked=PALRGB.map(c=>c.slice());
  const miss=composePalJS();
  let ok=(miss===null);
  if(ok) for(let i=0;i<256;i++)
    for(let k=0;k<3;k++) if(PALRGB[i][k]!==baked[i][k]){ ok=false; break; }
  globalThis.__pal_ok=ok;
  if(!ok) console.error('live palette compose MISMATCH vs baked');
  for(let i=0;i<256;i++) PALRGB[i]=baked[i];
}
function palLive(){
  const missing=composePalJS();
  LIVE_PAL=true;
  for(let t=0;t<ntplNow();t++) OVERRIDE.set(t, renderTplCanvasPix(t));
  for(let q=0;q<QW*QH;q++) redrawQuad(q);
  drawPal(); drawSpawns(); drawSwatch(); drawTgrid();
  stat.textContent=missing!==null
    ? ` palette: chunk ${missing.toString(16).toUpperCase()} not in the page set — save+reload to see it`
    : ' palette recomposed live';
}
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
  await api('/api/save',{kind:'gtld',chunk:GT_ID,data:buildGtldWords()});
  await api('/api/save',{kind:'tileset',chunk:TS_ID,data:buildTilesetB64()});
  await api('/api/save',{kind:'tilemask',chunk:TMASK_ID,data:buildMasksB64()});
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
    tpng, ntiles, tcols = render_tile_atlas(tgfx, pal)
    tgfx_tail = tgfx[ntiles * 64:]
    # masks chunk = tileset chunk + 1 (engine formula: v2_load_level_data
    # loads word_2AAC3+1 into the GS mask segment)
    tmask = LR.open_payload(ts_id + 1, "lzss", root)
    tmask_tail = tmask[ntiles * 8:]
    gtld_words = [gtld[i * 2] | (gtld[i * 2 + 1] << 8)
                  for i in range(ntpl * 4)]
    gt_rest = gtld[ntpl * 8:].hex()
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
        "gtid": f"{gt_id:04X}",
        "tpng": base64.b64encode(tpng).decode(),
        "ntiles": ntiles, "tcols": tcols,
        "gtld": json.dumps(gtld_words, separators=(",", ":")),
        "gtrest": gt_rest, "gtldhex": gtld.hex(),
        "tsid": f"{ts_id:04X}", "tmid_mask": f"{ts_id + 1:04X}",
        "tmaskb64": base64.b64encode(tmask[:ntiles * 8]).decode(),
        "tmasktail": tmask_tail.hex(),
        "tgfxb64": base64.b64encode(tgfx[:ntiles * 64]).decode(),
        "tgfxtail": tgfx_tail.hex(),
        "palrgb": json.dumps([list(c) for c in pal],
                             separators=(",", ":")),
        "palchunks": json.dumps(
            {f"{c:04X}": d.hex() for c, d in LR.pal_chunk_union().items()},
            separators=(",", ":")),
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
