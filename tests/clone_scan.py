#!/usr/bin/env python3
"""M4 clone scan (docs2/VERIFICATION_GAPS_ANALYSIS.md): поиск дублей
реализаций в v2-коде — пар функций/фрагментов с почти одинаковым профилем
(мультимножество DS_-констант, OBJ_-колонок и hex-вызовов).

Класс C слепых зон: одна orig-процедура, несколько v2-воплощений — фикс
попадает в одну копию (№44/№49-история; 11cbb имел ТРИ копии, 120d1 — две).

Выход: пары с коэффициентом Жаккара >= порога, отсортированные по убыванию.
Пары «зеркало ↔ его fn-test-обёртка» и крошечные профили (<6 фич) опущены.
"""
import re, os, sys
from itertools import combinations

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = ['src/sdl/v2_vm.cpp', 'src/sdl/render_v2.cpp', 'src/sdl/v2_render_funcs.cpp',
         'src/sdl/render.cpp']
THRESH = float(sys.argv[1]) if len(sys.argv) > 1 else 0.75

FEAT = re.compile(r'\b(DS_[A-Z0-9_]+|OBJ_[A-Z0-9_]+|LUT_[A-Z0-9_]+'
                  r'|v2_[a-z0-9_]*_[0-9a-fA-F]{4,5}(?=\s*\()|fx::[a-z_]+)')
FN   = re.compile(r'^(?:extern "C"\s+)?(?:static\s+)?(?:inline\s+)?'
                  r'[a-zA-Z_][\w:<>*&\s]*\b([a-zA-Z_]\w*)\s*\([^;]*$')

def bodies():
    out = {}
    for path in FILES:
        p = os.path.join(ROOT, path)
        if not os.path.exists(p): continue
        src = open(p, encoding='utf-8', errors='replace').read().split('\n')
        starts = [(i, m.group(1)) for i, ln in enumerate(src)
                  if (m := FN.match(ln)) and not ln.rstrip().endswith(';')
                  and '=' not in ln.split('(')[0]]
        for k, (i, name) in enumerate(starts):
            end = starts[k+1][0] if k+1 < len(starts) else len(src)
            feats = []
            for ln in src[i+1:end]:
                feats += FEAT.findall(ln.split('//')[0])
            out.setdefault(name, []).append((path, i+1, feats))
    return out

def jaccard(a, b):
    sa, sb = set(a), set(b)
    if not sa or not sb: return 0.0
    return len(sa & sb) / len(sa | sb)

def main():
    bs = bodies()
    flat = [(n, path, ln, f) for n, insts in bs.items()
            for (path, ln, f) in insts if len(set(f)) >= 6]
    pairs = []
    for (n1, p1, l1, f1), (n2, p2, l2, f2) in combinations(flat, 2):
        if n1 == n2: continue
        if n1.startswith('v2_fntest_') or n2.startswith('v2_fntest_'): continue
        j = jaccard(f1, f2)
        if j >= THRESH:
            pairs.append((j, n1, f"{p1}:{l1}", n2, f"{p2}:{l2}",
                          len(set(f1)), len(set(f2))))
    pairs.sort(reverse=True)
    for j, n1, w1, n2, w2, s1, s2 in pairs:
        print(f"{j:0.2f}  {n1} ({w1}, {s1}f)  <->  {n2} ({w2}, {s2}f)")
    print(f"\n[clone-scan] candidates={len(pairs)} (threshold {THRESH})")

if __name__ == '__main__':
    main()
