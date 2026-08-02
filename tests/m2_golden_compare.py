#!/usr/bin/env python3
"""M2 golden-трейс (docs2/VERIFICATION_GAPS_ANALYSIS.md, задача #65): сверка
V-потоков двух прогонов ОДНОГО реплея — default-режим (эталон) и V2_ONLY.

Канал ловит класс B (V2_ONLY-пути без orig-пары): v2-зеркала, которые в
standalone-режиме вызываются не так (чаще/реже/в другом порядке), чем в
default, где рядом бежит orig и каждый счётчик заякорен CALLPAR'ом.

Прогоны: V2_CC_TRACE=all + --replay-input=<one.inp>; формат строк
"V<id> <cumcount> f<frame>" (O-строки есть только в default — игнорируются).

Сравнение:
  1) жёсткое: последовательность id V-хитов (порядок вызовов) байт-в-байт;
  2) если (1) чисто, но кадровые метки съехали — отдельный отчёт (нумерация
     кадров default/V2_ONLY может легально дрейфовать, см. №32).

Использование: python3 tests/m2_golden_compare.py <default.log> <v2only.log>
Выход 0 = потоки идентичны (кадровый дрейф допустим, печатается предупреждение).
"""
import sys, re

def load_v(path):
    seq = []          # [(id, frame)]
    rx = re.compile(r'^V(\d+) (\d+) f(-?\d+)$')
    with open(path) as fh:
        for ln in fh:
            m = rx.match(ln.strip())
            if m:
                seq.append((int(m.group(1)), int(m.group(3))))
    return seq

def trim_to_full_frames(seq, last_common):
    """Обрезать хвост: оставить хиты кадров < last_common (последний кадр
    каждой стороны может быть неполным — стоп-критерии default (headless-hook
    внутри кадра) и V2_ONLY (конец главного цикла) режут в разных точках)."""
    return [x for x in seq if x[1] < last_common]

def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a = load_v(sys.argv[1])   # default (эталон)
    b = load_v(sys.argv[2])   # V2_ONLY
    last_common = min(a[-1][1], b[-1][1])
    a = trim_to_full_frames(a, last_common)
    b = trim_to_full_frames(b, last_common)
    print(f"[m2] сравнение по полным кадрам < f{last_common}")
    ids_a = [x[0] for x in a]
    ids_b = [x[0] for x in b]
    n = min(len(ids_a), len(ids_b))
    if ids_a[:n] == ids_b[:n] and len(ids_a) == len(ids_b):
        drift = sum(1 for (ia, fa), (ib, fb) in zip(a, b) if fa != fb)
        print(f"[m2] V-потоки ИДЕНТИЧНЫ: {len(a)} хитов; кадровый дрейф у {drift} хитов"
              + ("" if drift == 0 else " (легально при разной нумерации кадров)"))
        return 0
    # первая точка расхождения
    for i in range(n):
        if ids_a[i] != ids_b[i]:
            lo = max(0, i - 3)
            print(f"[m2] РАСХОЖДЕНИЕ порядка на хите #{i}:")
            print(f"  default: ...{ids_a[lo:i]} -> {ids_a[i]} (f{a[i][1]})")
            print(f"  v2only:  ...{ids_b[lo:i]} -> {ids_b[i]} (f{b[i][1]})")
            break
    else:
        i = n
        print(f"[m2] ХВОСТОВОЕ расхождение: default={len(ids_a)} хитов, v2only={len(ids_b)}")
        longer, name = (a, 'default') if len(a) > len(b) else (b, 'v2only')
        print(f"  лишние в {name} начиная с #{n}: {[x[0] for x in longer[n:n+8]]}")
    # суммарные счётчики по id — где именно разъехалось
    from collections import Counter
    ca, cb = Counter(ids_a), Counter(ids_b)
    diff_ids = sorted(set(ca) | set(cb))
    print("[m2] per-id totals (default vs v2only):")
    for d in diff_ids:
        mark = '' if ca.get(d, 0) == cb.get(d, 0) else '   <<< DIFF'
        print(f"  id {d:2d}: {ca.get(d,0):7d} {cb.get(d,0):7d}{mark}")
    return 1

if __name__ == '__main__':
    sys.exit(main())
