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

SCRATCH = "/tmp/lv_edit_scratch"
GAME = [None]          # Popen of the running game (windowed)
LOCK = threading.Lock()

SAVE_KINDS = {
    "tilemap": ("tilemaps", "tilemap_u16"),
    "header": ("level_headers", "level_header_stripe"),
}


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
    return len(os.listdir(os.path.join(SCRATCH, ".compiled")))


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
            "<th></th></tr>" + "".join(rows) + "</table>")


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
            if self.path.startswith("/png/"):
                cid = self.path[5:].split("?")[0].upper()
                png, _, _ = LR.render(cid, root=SCRATCH)
                return self._send(200, png, "image/png")
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
            self._err("not found", 404)
        except Exception as e:                                  # noqa: BLE001
            self._err(f"{type(e).__name__}: {e}", 500)

    def api_save(self, body):
        kind = body.get("kind")
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

    def api_play(self):
        p = GAME[0]
        if p is not None and p.poll() is None:
            return self._err(f"game already running (pid {p.pid})", 409)
        env = dict(os.environ)
        env["V2_ASSETS_DIR"] = os.path.join(SCRATCH, ".compiled")
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
        if snap:
            env["V2_LADDER_SNAP"] = str(snap)
            for f in (snap, snap + 1):
                p = f"/tmp/ladder_f{f}.ppm"
                if os.path.exists(p):
                    os.unlink(p)
        r = subprocess.run([os.path.join(LR.ROOT, "vikings"),
                            "--replay", os.path.join(LR.ROOT, replay),
                            f"--max-frames={frames}"],
                           cwd=LR.ROOT, env=env, timeout=600,
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
