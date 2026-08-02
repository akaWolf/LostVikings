#!/usr/bin/env python3
"""M3 call-order audit (docs2/VERIFICATION_GAPS_ANALYSIS.md): статическая
сверка последовательности вызовов orig sub_X против её v2-зеркала.

Не доказательство эквивалентности — детектор несоответствий уровня №49/№50:
лишние/пропущенные/переставленные вызовы зеркалируемых процедур. Диффы =
кандидаты на ручную сверку (условные вызовы дают легальные расхождения).

Использование:
  python3 tests/call_order_audit.py            # отчёт по всем спаренным телам
  python3 tests/call_order_audit.py sub_11cbb  # одна пара

TODO доводки (детектор → чистый отчёт):
  * транзитивно разворачивать вызовы v2-хелперов БЕЗ hex-суффикса
    (v2_pause_switch_viking и т.п. — фаза C выносит такие; сейчас их
    внутренние 120d1/11f47-вызовы теряются => ложные delete);
  * маппинг fx::play_sfx*/stop_* → sub_177bb/sub_17912-семья;
  * пары draw+vga (v2_draw_hud_item + v2_vga_hud_item_1183d) считать ОДНИМ
    вызовом sub_1183d;
  * whitelist легальных расхождений (1cd7b↔1cd7d bp-параметр; 17561/17512
    AIL-порт-отрезы #61; инлайн-эквиваленты с комментом "equivalent");
  * закомментированные PORT-строки orig (`//cs=...J(JMP(sub_16775))` под
    SDL-заменой) считать частью orig-последовательности — иначе корректный
    v2-вызов выглядит лишним (ложный insert, пример: sub_11439);
  * `J(JMP(loc_XXXX))`, где loc_ — вход процедуры с v2-зеркалом (174e9/174bf
    Y-муверы), считать вызовом (ложный insert в sub_1064b).
"""
import re, sys, os
from difflib import SequenceMatcher

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORIG_FILES = ['src/vikings.exe_seg000.cpp', 'src/vikings.exe_seg003.cpp']
V2_FILES   = ['src/sdl/v2_vm.cpp', 'src/sdl/v2_render_funcs.cpp']

def load(p):
    return open(os.path.join(ROOT, p), encoding='latin-1').read().split('\n')

# loc_-входы, имеющие v2-зеркала (JMP на них = вызов): Y-муверы и др.
LOC_MIRRORED = {'loc_174e9': 'sub_174e9', 'loc_174bf': 'sub_174bf',
                'loc_17496': 'sub_17496', 'loc_1746c': 'sub_1746c'}

# ---- 1. orig: sub_X → последовательность CALL/CALLF-таргетов -------------
def parse_orig():
    bodies = {}
    for path in ORIG_FILES:
        src = load(path)
        cur = None
        for ln in src:
            m = re.match(r'^(sub_[0-9a-f]+):', ln)
            if m:
                cur = m.group(1); bodies.setdefault(cur, []); continue
            if re.match(r'^(seg00\d_[0-9a-f]+_proc|loc_[0-9a-f]+_far):', ln):
                cur = None; continue           # far-проки — отдельные зоны
            if cur is None: continue
            # PORT-замены: закомментированный J(...) под SDL-вставкой — это
            # ЖИВОЕ orig-поведение (перенесено в замену), считаем вызовом.
            code = ln
            if ln.lstrip().startswith('//cs='):
                code = ln.lstrip()[2:]
            else:
                code = ln.split('//')[0]
            for cm in re.finditer(r'J\((?:CALL|CALLF)\((sub_[0-9a-f]+)', code):
                bodies[cur].append(cm.group(1))
            jm = re.search(r'J\(JMP\((sub_[0-9a-f]+)\)\)', code)
            if jm: bodies[cur].append(jm.group(1))   # tail call == call, для сверки
            lm = re.search(r'J\(JMP\((loc_[0-9a-f]+)\)\)', code)
            if lm and lm.group(1) in LOC_MIRRORED:
                bodies[cur].append(LOC_MIRRORED[lm.group(1)])
    return bodies

# ---- 2. v2: v2_*_HEX → последовательность вызовов v2_*_HEX ---------------
V2NAME = re.compile(r'\bv2_[a-z0-9_]*_([0-9a-fA-F]{4,5})\b')
V2ANY  = re.compile(r'\b(v2_[a-z0-9_]+)\s*\(')

def v2_raw_bodies():
    """Тела ВСЕХ v2_*-функций: имя → список сырых вызовов
    (('hex', sub_key) | ('helper', v2_name) | ('sfx', sub_key))."""
    raw = {}
    fn_start = re.compile(
        r'^(?:extern "C"\s+)?(?:static\s+)?(?:inline\s+)?'
        r'[a-zA-Z_][\w:<>*&\s]*\b([a-zA-Z_]\w*)\s*\([^;]*$')
    for path in V2_FILES:
        src = load(path)
        starts = []          # ЛЮБАЯ функция — граница тела (не только *_HEX)
        for i, ln in enumerate(src):
            m = fn_start.match(ln)
            if m and not ln.rstrip().endswith(';') and '=' not in ln.split('(')[0]:
                starts.append((i, m.group(1)))
        for k, (i, name) in enumerate(starts):
            end = starts[k+1][0] if k+1 < len(starts) else len(src)
            calls = []
            for ln in src[i+1:end]:
                code = ln.split('//')[0]
                if re.search(r'fx::(play_sfx|play_xmidi)', code):
                    calls.append(('sfx', 'sub_177bb'))
                if re.search(r'fx::stop_all_sfx', code):
                    calls.append(('sfx', 'sub_17912'))
                for cm in V2ANY.finditer(code):
                    fn = cm.group(1)
                    if fn == name or fn.startswith('v2_fntest_'):
                        continue
                    hm = re.match(r'v2_[a-z0-9_]+_([0-9a-fA-F]{4,5})$', fn)
                    if hm:
                        a = hm.group(1).lower()
                        calls.append(('hex', 'sub_1' + a if len(a) == 4 else 'sub_' + a))
                    else:
                        calls.append(('helper', fn))
            raw[name] = calls
    return raw

def flatten(name, raw, depth=0, seen=None):
    """Транзитивно развернуть helper-вызовы (не-hex v2-функции) до sub_-ключей."""
    if seen is None: seen = set()
    out = []
    for kind, tgt in raw.get(name, []):
        if kind in ('hex', 'sfx'):
            out.append(tgt)
        elif kind == 'helper' and depth < 4 and tgt not in seen and tgt in raw:
            out.extend(flatten(tgt, raw, depth + 1, seen | {name}))
    return out

def v2_bodies():
    raw = v2_raw_bodies()
    bodies = {}
    for name in raw:
        if name.startswith('v2_fntest_'):
            continue
        am = re.match(r'v2_[a-z0-9_]+_([0-9a-fA-F]{4,5})$', name)
        if not am: continue
        addr = am.group(1).lower()
        calls = flatten(name, raw)
        # draw+vga-пары (v2_draw_* без hex + v2_vga_*_1183d) дают одиночный
        # hex-вызов — пары уже не двоятся; дубль-защита не требуется.
        key = 'sub_1' + addr if len(addr) == 4 else 'sub_' + addr
        bodies.setdefault(key, []).append((name, calls))
    return bodies

def diff_pair(orig_seq, v2_seq):
    sm = SequenceMatcher(a=orig_seq, b=v2_seq, autojunk=False)
    out = []
    for tag, a0, a1, b0, b1 in sm.get_opcodes():
        if tag == 'equal': continue
        out.append((tag, orig_seq[a0:a1], v2_seq[b0:b1]))
    return out

def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    orig = parse_orig()
    v2   = v2_bodies()
    pairs = sorted(set(orig) & set(v2))
    if only: pairs = [p for p in pairs if p == only]
    n_diff = 0
    for p in pairs:
        oseq = [c for c in orig[p]]
        if not oseq: continue
        for (v2name, vseq) in v2[p]:
            d = diff_pair(oseq, vseq)
            if not d: continue
            n_diff += 1
            print(f"=== {p} ↔ {v2name}")
            print(f"  orig ({len(oseq)}): {' '.join(s.replace('sub_','') for s in oseq)}")
            print(f"  v2   ({len(vseq)}): {' '.join(s.replace('sub_','') for s in vseq)}")
            for tag, a, b in d:
                print(f"  {tag:8s} orig={[s.replace('sub_','') for s in a]} v2={[s.replace('sub_','') for s in b]}")
    print(f"\n[call-order-audit] paired={len(pairs)} with-diffs={n_diff}")

if __name__ == '__main__':
    main()
