#!/usr/bin/env python3
# Phase-D verify amplifier: generate password-entry replays for EVERY level.
#
# Passwords live in the EXE image at DS 0x85A5: 37 levels x 4 high-bit-ASCII
# letters (first = "STRT"), ending exactly at rng_seed 0x8639. The title
# screen opens the password prompt on ESC (1041C gate); four letters + RETURN
# (INT9 letter channel, 0x81 confirm) load the level directly.
#
# Output: tests/replays/pw_NN_<PASS>.inp/.frames — part of the canonical
# scenario set (tests/scenarios.sh runs everything in tests/replays/).
import os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT  = os.path.join(ROOT, 'tests', 'replays')
os.makedirs(OUT, exist_ok=True)

ds = open(os.path.join(ROOT, 'ds_static.bin'), 'rb').read()
pw_raw = ds[0x85A5:0x85A5 + 37 * 4]
passwords = [bytes(b & 0x7F for b in pw_raw[i*4:(i+1)*4]).decode('ascii')
             for i in range(37)]

# Recipe v4 — measured with V2_PHASE_LOG on a no-input attract run: the
# password screen is LEVEL 38 (25C9=0x26) and arrives BY ITSELF in the
# attract chain: intro(39) -> title(40) -> password(38, ~f110..f330) ->
# demo level. No SPACE, no ESC: any 0x81 confirm (SPACE/RETURN) on the
# title starts a NEW GAME, and a lingering 0x81 in [28C] is what kept
# confirming the default "STRT" (level 0) in recipes v1-v3. The scene
# polls [28C] per frame (bytecode, anim-seg letters -> op57 writes to
# 0x310..0x316, op_D3 verifies on 0x81) — short presses, one per char.
# The scene's interactive poller (bytecode @85D2: acc='N'/'P'/'Q' + op74
# compare against [28C]) is a MENU: N)ew game, P)assword entry, Q)uit.
# It only wakes when an ACTION-bit key interrupts the attract flow
# (letters carry no action bits — a letters-only replay never wakes it;
# measured: RETURN@238 -> poller polls from f253). 'P' jumps to the
# letter-entry subscene (@868A) with the op57 cursor writes + op_D3
# verify on 0x81. Unmatched [28C] values are NOT consumed by the menu,
# so the wake key's 0x81 lingers harmlessly until 'P' overwrites it.
WAKE_F  = 230    # RETURN: action bit 0x8000 interrupts attract -> menu
P_F     = 258    # press 'P' once the N/P/Q poller is awake (~wake+15)
BASE    = 278    # first password letter, after the subscene opens
HOLD    = 3      # short press: the letter channel is a per-press store
GAP     = 12     # give the scene poll+cursor-advance frames between chars
PLAY    = 1500   # gameplay budget after the level loads
LOAD    = 400    # generous level-load window after RETURN

for i, pw in enumerate(passwords):
    name = f'pw_{i:02d}_{pw}'
    lines = [f'# generated: password entry for slot {i} ("{pw}") — full-level',
             f'# verify coverage (phase D amplifier). The attract chain brings',
             f'# up the password scene (level 38) by itself at ~f110; four',
             f'# letters + RETURN load the level; then {PLAY} frames of play.']
    lines.append(f'{WAKE_F} KD RETURN')
    lines.append(f'{WAKE_F+HOLD} KU RETURN')
    lines.append(f'{P_F} KD P')
    lines.append(f'{P_F+HOLD} KU P')
    f = BASE
    for ch in pw:
        lines.append(f'{f} KD {ch}')
        lines.append(f'{f+HOLD} KU {ch}')
        f += HOLD + GAP
    lines.append(f'{f+20} KD RETURN')
    lines.append(f'{f+20+HOLD} KU RETURN')
    end = f + 20 + HOLD + LOAD + PLAY
    lines.append(f'# end-frame {end}')
    open(os.path.join(OUT, name + '.inp'), 'w').write('\n'.join(lines) + '\n')
    open(os.path.join(OUT, name + '.frames'), 'w').write(str(end) + '\n')

print(f'generated {len(passwords)} replays into {OUT}')
print('passwords:', ' '.join(passwords))
