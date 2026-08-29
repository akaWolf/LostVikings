#!/usr/bin/env python3
"""level_inspect.py — interactive HTML inspector for a level (task #103).

Generates a single self-contained .html: the standalone level render as a
data-URI PNG plus a hover layer that shows, per 16x16 quad, the raw tilemap
word (template index & attribute bits) and any spawn-table entries under
the cursor. Pure client-side; open the file in any browser.

Usage:
  python3 tools/assets/level_inspect.py 00CA [-o out.html]
"""
import argparse
import base64
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assetc import read_payload  # noqa: E402
import level_render as LR  # noqa: E402

PAGE = """<!doctype html>
<meta charset="utf-8">
<title>LV level %(cid)s</title>
<style>
 body { margin:0; background:#111; color:#ddd; font:13px monospace; }
 #wrap { position:relative; display:inline-block; }
 #lvl { display:block; image-rendering: pixelated; }
 #hl { position:absolute; border:1px solid #ff4; pointer-events:none;
       width:%(zq)dpx; height:%(zq)dpx; display:none; }
 #tip { position:fixed; background:#000c; border:1px solid #555; padding:6px 8px;
        pointer-events:none; display:none; white-space:pre; z-index:9; }
 #bar { padding:6px 8px; background:#222; position:sticky; top:0; }
</style>
<div id="bar">level %(cid)s — %(qw)dx%(qh)d quads (%(pw)dx%(ph)dpx),
 spawns: %(nsp)d, zoom <select id="z"><option>1</option><option selected>2</option>
 <option>3</option><option>4</option></select>
 <label><input type="checkbox" id="tt"> type tint</label></div>
<div id="wrap">
 <img id="lvl" src="data:image/png;base64,%(png)s" width="%(zw)d">
 <canvas id="ov" style="position:absolute;left:0;top:0;pointer-events:none"></canvas>
 <div id="hl"></div>
</div>
<div id="tip"></div>
<script>
const QW=%(qw)d, QH=%(qh)d, MAP=%(map)s, SPAWNS=%(spawns)s;
const CLASSES=%(classes)s; // cls -> sub_13e52 template record
const ICONS=%(icons)s; // engine-harvested sprites (class_icons.py)
const IIMG={};
for(const k in ICONS){const im=new Image();im.onload=()=>{if(typeof drawTint==='function')drawTint();};im.src='data:image/png;base64,'+ICONS[k].b64; IIMG[k]=im;}
// type labels — verified against the ground-snap physics (sub_1625d):
// passable {0,3,0xC}, solid list {1,2,5,0x20}, platform 4 (one-way snap),
// slopes >= 0x30 (profile via sub_16390). Others: unlabeled yet.
// slope height LUT (DS@0x897C, sub_16390: surface y-in-quad per x&0xF;
// only 0x30..0x35 hold real profiles — 45deg pair + 22.5deg half pairs)
const SLOPES={
 0x30:[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15],
 0x31:[15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0],
 0x32:[0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7],
 0x33:[8,8,9,9,10,10,11,11,12,12,13,13,14,14,15,15],
 0x34:[7,7,6,6,5,5,4,4,3,3,2,2,1,1,0,0],
 0x35:[15,15,14,14,13,13,12,12,11,11,10,10,9,9,8,8]};
function typeName(t){
  // Sources: ground physics sub_1625d; the engine's own FILTER LISTS at
  // ds:0x94CC (FF-terminated type sets walked by the tile-probe family) —
  // climbable = set {3,4,D} (si=0x29), ground-solid = {1,2,5,20} (si=0x2F),
  // hazard family = {10..17} (si=0x5C, also each individually);
  // obs: 10 = lava floor (BBLS), 15 = ship lift-beam base, 14 = large
  // pooled areas on 20 levels (water?).
  if(t===0)return'air'; if(t===1)return'solid';
  if(t===3)return'climbable (ladder)'; if(t===4)return'climbable+platform';
  if(t===0xD)return'climbable (rope?)'; if(t===0xC)return'passable';
  if(t===2||t===5||t===0x20)return'solid*';
  if(t===6)return'solid variant (standable sets {1,2,6,20}/{1,5,6})';
  if(t===7)return'type 7 (filter-only)';
  if(t===0xA)return'solid-ish (sets {1,A}/{1,A,14}/{0,1,5,A,C})';
  if(t===0xB)return'script-checked (?)';
  if(t>=0x10&&t<=0x17)return'hazard/liquid '+(t-0x10)+' (family 10-17)';
  if(t>=0x30&&t<=0x35)return'slope'; return'?';
}
const img=document.getElementById('lvl'), hl=document.getElementById('hl'),
      tip=document.getElementById('tip'), zsel=document.getElementById('z');
let Z=2;
const ov=document.getElementById('ov'), tt=document.getElementById('tt');
function drawSpawnBoxes(c){
  c.imageSmoothingEnabled=false;
  for(const s of SPAWNS){
    const ci=CLASSES[s.cls]||null;
    const w=ci?ci.w:8, h=ci?ci.h:8;
    const x0=s.x-(w>>1), y0=s.y-(h>>1);   // sub_13e52 bbox math
    const ic=ICONS[s.cls], im=IIMG[s.cls];
    if(ic&&im&&im.complete){
      const ix0=s.x-(ic.w>>1), iy0=s.y-(ic.h>>1);
      c.drawImage(im,ix0*Z,iy0*Z,ic.w*Z,ic.h*Z);
    }
    c.strokeStyle=(s.anim&0x800)?'#6f6':(ci&&ci.spr===0xFFFF?'#888':'#f4f');
    c.lineWidth=1;
    c.strokeRect(x0*Z+0.5,y0*Z+0.5,w*Z-1,h*Z-1);
    c.fillStyle='#fff'; c.font=(4*Z+3)+'px monospace';
    c.fillText(s.cls.toString(16).toUpperCase(),x0*Z+Z,y0*Z-Z);
  }
}
function drawTint(){
  ov.width=%(pw)d*Z; ov.height=%(ph)d*Z;
  const c=ov.getContext('2d'); c.clearRect(0,0,ov.width,ov.height);
  drawSpawnBoxes(c);
  if(!tt.checked) return;
  for(let qy=0;qy<QH;qy++) for(let qx=0;qx<QW;qx++){
    const ty=MAP[qy*QW+qx]>>10;
    if(!ty) continue;
    if(SLOPES[ty]){
      // slope: draw the actual surface profile instead of a flat tint
      c.fillStyle='hsla(20,100%%,60%%,0.85)';
      const h=SLOPES[ty];
      for(let x=0;x<16;x++)
        c.fillRect((qx*16+x)*Z,(qy*16+h[x])*Z,Z,Z);
      c.fillStyle='hsla(20,100%%,60%%,0.2)';
      c.fillRect(qx*16*Z,qy*16*Z,16*Z,16*Z);
    } else {
      c.fillStyle=`hsla(${(ty*47)%%360},90%%,55%%,0.42)`;
      c.fillRect(qx*16*Z,qy*16*Z,16*Z,16*Z);
    }
    if(Z>=2){ c.fillStyle='#000'; c.font=(5*Z)+'px monospace';
      c.fillText(ty.toString(16).toUpperCase(),qx*16*Z+2*Z,qy*16*Z+6*Z); }
  }
}
function setz(){ Z=+zsel.value; img.width=%(pw)d*Z;
  hl.style.width=hl.style.height=(16*Z)+'px'; drawTint(); }
zsel.onchange=setz; tt.onchange=drawTint; setz();
img.onmousemove=e=>{
  const r=img.getBoundingClientRect();
  const px=(e.clientX-r.left)/Z, py=(e.clientY-r.top)/Z;
  const qx=Math.floor(px/16), qy=Math.floor(py/16);
  if(qx<0||qy<0||qx>=QW||qy>=QH){hl.style.display='none';tip.style.display='none';return;}
  const w=MAP[qy*QW+qx];
  hl.style.display='block';
  hl.style.left=(qx*16*Z)+'px'; hl.style.top=(qy*16*Z)+'px';
  let t=`quad (${qx},${qy})  word ${w.toString(16).padStart(4,'0').toUpperCase()}`+
        `\\n template ${(w&0x3FF).toString(16).toUpperCase()}  type ${(w>>10).toString(16).toUpperCase()} ${typeName(w>>10)}`;
  for(const s of SPAWNS){
    if(s.x>=qx*16&&s.x<qx*16+16&&s.y>=qy*16&&s.y<qy*16+16){
      t+=`\\n spawn cls=${s.cls.toString(16).toUpperCase()} @(${s.x},${s.y})`+
         ` half=(${s.half_w},${s.half_h}) anim=${s.anim.toString(16).toUpperCase()}`+
         ((s.anim&0x800)?' PERM':'');
      const ci=CLASSES[s.cls];
      if(ci) t+=`\\n   class: ${ci.w}x${ci.h} sub=${ci.sub}`+
        (ci.spr===0xFFFF?' INVISIBLE':(ci.spr===0xFFFE?' pool-sprite':
         ` spr=${ci.spr.toString(16).padStart(4,'0').toUpperCase()}`))+
        ` pc=${ci.pc.toString(16).toUpperCase()} bits=${ci.bits.toString(16).toUpperCase()}`;
    }
  }
  tip.textContent=t; tip.style.display='block';
  tip.style.left=(e.clientX+14)+'px'; tip.style.top=(e.clientY+14)+'px';
};
img.onmouseleave=()=>{hl.style.display='none';tip.style.display='none';};
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
    qw, qh, tm_id, _, _, _, spawns = LR.parse_header(raw)
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
    words = [tmap[i * 2] | (tmap[i * 2 + 1] << 8) for i in range(qw * qh)]
    png, w, h = LR.render(cid)
    html = PAGE % {
        "cid": cid, "qw": qw, "qh": qh,
        "pw": w * 8, "ph": h * 8, "zw": w * 8 * 2, "zq": 32,
        "nsp": len(spawns),
        "png": base64.b64encode(png).decode(),
        "map": json.dumps(words, separators=(",", ":")),
        "spawns": json.dumps(spawns, separators=(",", ":")),
        "classes": json.dumps(classes, separators=(",", ":")),
        "icons": json.dumps(LR.load_class_icons(script_id),
                            separators=(",", ":")),
    }
    out = args.out or f"/tmp/level_{cid}.html"
    with open(out, "w") as f:
        f.write(html)
    print(f"{cid}: {qw}x{qh} quads, {len(spawns)} spawns -> {out}")


if __name__ == "__main__":
    main()
