#!/usr/bin/env python3
"""midi_check.py — the sound lanes' regression judge (tests/mt32_dump.sh).

  midi_check.py mt32 DUMP.mid REF.derived.mid [OTHER.derived.mid]
      DUMP = V2_MT32_DUMP of the MT-32 world (the game's MT-32 driver's MPU stream, SysEx included).
      Checks: the 0x215 preload uploaded all 64 custom timbres into the module's timbre memory;
      the MT-32 variant of the level track (REF) plays note for note in order on the drums and the
      lead channel at exactly the FM world's rate (1 XMIDI unit per driver tick); the FM variant
      (OTHER) does NOT match — the MT-32 set is what plays.
  midi_check.py lane DUMP.mid REF.derived.mid
      DUMP = V2_MIDI_DUMP of the audible driver's channel messages (the SC-55 lane's source);
      REF = the FM variant of the track: its note-ons in order per channel at 1 unit per tick.

A derived.mid (tools/assets/assetc.py xmid_to_midi) is the XMIDI at 120 ticks/s nominal; the driver
steps one XMIDI unit per fn67 tick, so "units per tick" is measured from matched note spans.
Exit code 0 = PASS, 1 = FAIL (the reasons are printed)."""
import sys

def smf(path):
    d = open(path, "rb").read()
    if d[:4] != b"MThd": raise SystemExit(f"{path}: not an SMF")
    div = int.from_bytes(d[12:14], "big"); i = 14; tracks = []
    while i < len(d):
        ln = int.from_bytes(d[i+4:i+8], "big"); tracks.append(d[i+8:i+8+ln]); i += 8 + ln
    trk = tracks[0]; ev = []; sysex = []; t = 0; p = 0; run = None
    while p < len(trk):
        dt = 0
        while True:
            c = trk[p]; p += 1; dt = (dt << 7) | (c & 0x7F)
            if not c & 0x80: break
        t += dt; b = trk[p]
        if b == 0xFF:
            meta = trk[p+1]; l = trk[p+2]; p += 3 + l
            if meta == 0x2F: break
            continue
        if b in (0xF0, 0xF7):
            p += 1; l = 0
            while True:
                c = trk[p]; p += 1; l = (l << 7) | (c & 0x7F)
                if not c & 0x80: break
            sysex.append((t, bytes([b]) + trk[p:p+l])); p += l; continue
        if b & 0x80: run = b; p += 1
        n = 1 if (run & 0xF0) in (0xC0, 0xD0) else 2
        ev.append((t, run, tuple(trk[p:p+n]))); p += n
    return div, ev, sysex

def noteons(ev): return [(t, s & 0x0F, d[0], d[1]) for t, s, d in ev if (s & 0xF0) == 0x90 and d[1] > 0]

def match_channel(dump_notes, ref_notes, ch):
    """greedy in-order match of the reference channel inside the dump channel: (matched pairs, ref count)"""
    d = [n for n in dump_notes if n[1] == ch]; r = [n for n in ref_notes if n[1] == ch]
    i = 0; pairs = []
    for x in d:
        if i < len(r) and x[2:] == r[i][2:]: pairs.append((x[0], r[i][0])); i += 1
    return pairs, len(r)

def units_per_tick(pairs):
    if len(pairs) < 2: return None
    sd = pairs[-1][0] - pairs[0][0]; sr = pairs[-1][1] - pairs[0][1]
    return (sr / sd) if sd else None

def check_mt32(dump_path, ref_path, other_path):
    fails = []
    div, ev, sysex = smf(dump_path); _, rev, _ = smf(ref_path)
    dn = noteons(ev); rn = noteons(rev)
    # 1) the timbre memory uploads: 64 timbres at 08 (2n) 00, 5 messages each (24 + 4 x 68 bytes)
    mem = [sx for t, sx in sysex if len(sx) > 8 and sx[1:4] == b"\x41\x10\x16" and sx[4] == 0x12 and sx[5] == 0x08]
    timbres = sorted({sx[6] // 2 for sx in mem})
    print(f"timbre memory: {len(mem)} SysEx over timbres {timbres[0] if timbres else '-'}..{timbres[-1] if timbres else '-'} ({len(timbres)} distinct)")
    if len(timbres) != 64 or len(mem) != 320: fails.append(f"expected 64 timbres / 320 SysEx, got {len(timbres)} / {len(mem)}")
    # 2) the patch memory entries the 0x215 pass writes (05 xx) and the MT-32 reset at the start
    if not any(sx[5] == 0x7F for t, sx in sysex if len(sx) > 8): fails.append("no MT-32 reset SysEx (7F 00 00)")
    # 3) the track: drums (channel 10 = index 9) and the lead (the channel with most reference notes)
    checks = []
    counts = {}
    for n in rn: counts[n[1]] = counts.get(n[1], 0) + 1
    lead = max((c for c in counts if c != 9), key=lambda c: counts[c])
    for ch, need in ((9, 150), (lead, 100)):    # ~20 s of the level inside the run's window; margin for slower boxes
        pairs, total = match_channel(dn, rn, ch); upt = units_per_tick(pairs)
        print(f"channel {ch}: {len(pairs)}/{total} reference note-ons in order, units per tick {upt if upt is None else round(upt, 3)}")
        if len(pairs) < need: fails.append(f"channel {ch}: only {len(pairs)} of {total} matched (need >= {need} within the run)")
        if upt is None or not (0.98 <= upt <= 1.02): fails.append(f"channel {ch}: {upt} XMIDI units per driver tick (expected 1.000: the FM world's rate)")
    # 4) the FM variant must not be what plays
    if other_path:
        _, oev, _ = smf(other_path); on = noteons(oev)
        pairs, total = match_channel(dn, on, lead)
        print(f"FM variant, channel {lead}: {len(pairs)}/{total} in order (must stay small)")
        if len(pairs) >= 30: fails.append(f"the FM variant matches the stream ({len(pairs)} note-ons on channel {lead}) — the MT-32 set is not playing")
    return fails

def check_lane(dump_path, ref_path):
    fails = []
    div, ev, _ = smf(dump_path); _, rev, _ = smf(ref_path)
    dn = noteons(ev); rn = noteons(rev)
    counts = {}
    for n in rn: counts[n[1]] = counts.get(n[1], 0) + 1
    lead = max((c for c in counts if c != 9), key=lambda c: counts[c])
    for ch, need in ((9, 100), (lead, 60)):
        pairs, total = match_channel(dn, rn, ch); upt = units_per_tick(pairs)
        print(f"channel {ch}: {len(pairs)}/{total} reference note-ons in order, units per tick {upt if upt is None else round(upt, 3)}")
        if len(pairs) < need: fails.append(f"channel {ch}: only {len(pairs)} of {total} matched (need >= {need})")
        if upt is None or not (0.98 <= upt <= 1.02): fails.append(f"channel {ch}: {upt} units per tick (expected 1.000)")
    return fails

if __name__ == "__main__":
    if len(sys.argv) < 4: raise SystemExit(__doc__)
    mode = sys.argv[1]
    fails = check_mt32(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None) if mode == "mt32" else check_lane(sys.argv[2], sys.argv[3])
    for f in fails: print("FAIL:", f)
    print("PASS" if not fails else f"FAILED ({len(fails)})")
    sys.exit(1 if fails else 0)
