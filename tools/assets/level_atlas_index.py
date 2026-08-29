#!/usr/bin/env python3
"""level_atlas_index.py — regenerate build/levels_atlas/index.html.

Cards in GAME LEVEL ORDER (the 42-entry table ds:0x940C in exe_static.bin,
see level_render.level_tables), each linking the inspector and the editor.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import level_render as LR  # noqa: E402

OUT = os.path.join(LR.ROOT, "build", "levels_atlas")

HEAD = ("<!doctype html><meta charset=\"utf-8\"><title>LV levels atlas</title>"
        "<style>body{background:#111;color:#ddd;font:14px monospace;margin:12px}"
        ".g{display:flex;flex-wrap:wrap;gap:12px}"
        ".c{display:block;width:320px;color:#8cf}"
        ".c img{width:320px;image-rendering:pixelated;border:1px solid #333;display:block}"
        ".c .t{padding:4px 2px}.c a{color:#8cf;text-decoration:none}"
        ".c a.e{color:#fc6;margin-left:8px}</style>")


def main():
    cards = []
    seen = set()
    pws = LR.level_passwords()
    order = LR.level_tables()
    listing = {fn[:-5] for fn in os.listdir(LR.HDR_DIR) if fn.endswith(".json")}
    seq = [(i, f"{hc:04X}", f"{sc:04X}" if sc != 0xFFFF else "-")
           for i, (hc, sc) in enumerate(order)]
    # any header not in the level table (none known) would append at the end
    seq += [(None, cid, "-") for cid in sorted(listing)
            if cid not in {c for _, c, _ in seq}]
    for idx, cid, script in seq:
        if cid in seen or cid not in listing:
            continue
        seen.add(cid)
        with open(os.path.join(LR.HDR_DIR, f"{cid}.json")) as f:
            hj = json.load(f)
        qw, qh = hj["width"], hj["height"]
        png = f"level_{cid}.png"
        has_png = os.path.exists(os.path.join(OUT, png))
        has_ins = os.path.exists(os.path.join(OUT, f"inspect_{cid}.html"))
        has_edt = os.path.exists(os.path.join(OUT, f"edit_{cid}.html"))
        lvl = f"lvl {idx}" if idx is not None else "—"
        if idx is not None and idx < len(pws):
            lvl += f" [{pws[idx]}]"
        img = (f'<img loading="lazy" src="{png}">' if has_png
               else '<div style="width:320px;height:60px;border:1px solid #333">no render</div>')
        links = []
        if has_ins:
            links.append(f'<a href="inspect_{cid}.html">inspect</a>')
        if has_edt:
            links.append(f'<a class="e" href="edit_{cid}.html">edit</a>')
        cards.append(
            f'<div class="c">{img}<div class="t">{cid} — {lvl}, {qw}×{qh}'
            f' quads, lvs {script}<br>{" ".join(links)}</div></div>')
    html = (HEAD + f"<h2>Lost Vikings — атлас уровней ({len(cards)})</h2>"
            '<div class="g">' + "".join(cards) + "</div>")
    out = os.path.join(OUT, "index.html")
    with open(out, "w") as f:
        f.write(html)
    print(f"{len(cards)} cards -> {out}")


if __name__ == "__main__":
    main()
