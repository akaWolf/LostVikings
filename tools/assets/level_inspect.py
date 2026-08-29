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
// type labels — verified against the ground-snap physics (sub_1625d):
// passable {0,3,0xC}, solid list {1,2,5,0x20}, platform 4 (one-way snap),
// slopes >= 0x30 (profile via sub_16390). Others: unlabeled yet.
function typeName(t){
  // climbable family {3,4,0xD} — the level bytecode checks them together
  // (18-54 uses across all lvs); 4 is also the one-way platform snap.
  if(t===0)return'air'; if(t===1)return'solid';
  if(t===3)return'climbable (ladder)'; if(t===4)return'climbable+platform';
  if(t===0xD)return'climbable (rope?)'; if(t===0xC)return'passable';
  if(t===2||t===5||t===0x20)return'solid*';
  if(t>=0x30)return'slope'; return'?';
}
const img=document.getElementById('lvl'), hl=document.getElementById('hl'),
      tip=document.getElementById('tip'), zsel=document.getElementById('z');
let Z=2;
const ov=document.getElementById('ov'), tt=document.getElementById('tt');
function drawTint(){
  ov.width=%(pw)d*Z; ov.height=%(ph)d*Z;
  const c=ov.getContext('2d'); c.clearRect(0,0,ov.width,ov.height);
  if(!tt.checked) return;
  for(let qy=0;qy<QH;qy++) for(let qx=0;qx<QW;qx++){
    const ty=MAP[qy*QW+qx]>>10;
    if(!ty) continue;
    c.fillStyle=`hsla(${(ty*47)%%360},90%%,55%%,0.42)`;
    c.fillRect(qx*16*Z,qy*16*Z,16*Z,16*Z);
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
    if(s.x>=qx*16&&s.x<qx*16+16&&s.y>=qy*16&&s.y<qy*16+16)
      t+=`\\n spawn cls=${s.cls.toString(16).toUpperCase()} @(${s.x},${s.y})`+
         ` p=(${s.p1},${s.p2}) anim=${s.anim.toString(16).toUpperCase()}`+
         ((s.anim&0x800)?' PERM':'');
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
    }
    out = args.out or f"/tmp/level_{cid}.html"
    with open(out, "w") as f:
        f.write(html)
    print(f"{cid}: {qw}x{qh} quads, {len(spawns)} spawns -> {out}")


if __name__ == "__main__":
    main()
