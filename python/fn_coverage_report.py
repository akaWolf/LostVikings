#!/usr/bin/env python3
"""fn-test coverage report (task #47).

Maps gcov line data of the m2c oracle (src/vikings.exe_seg000.cpp) onto the
original procedures (sub_XXXXX labels). The m2c port emits exactly one
source line per original instruction, prefixed `cs=0x1a2;eip=0x...` — so
line coverage of those lines IS instruction coverage of the original code.

Usage:
  gcov --json-format -o .obj-headless/src src/vikings.exe_seg000.cpp
  python3 python/fn_coverage_report.py vikings.exe_seg000.cpp.gcov.json.gz \
      [--src src/vikings.exe_seg000.cpp] [--min 100] [--csv out.csv]

Output: per-sub_ instruction coverage (executed/total eip lines, %), the
uncovered eip list for partially covered subs, and a class summary. The
procedure classes (unit / L / E / D / SDL) mirror the final classification
table in FN_SELFTEST_STATUS.md.
"""
import argparse, gzip, json, re, sys

# Final classification (FN_SELFTEST_STATUS.md, магистраль #41).
CLASS_L = {  # blocking loops / live cycle — verified via PSNAP/DS-verify/A2/replays
    'sub_10130', 'sub_10138', 'sub_10350', 'sub_100bb', 'sub_1086f',
    'sub_103ca', 'sub_1047c', 'sub_104a1', 'sub_11ba5',
    'sub_10f5d', 'sub_10fa0', 'sub_11080', 'sub_115d2',
}
CLASS_E = {  # DOS/BIOS/AIL/hardware — oracle escapes the isolator by construction
    'sub_1292f', 'sub_12948', 'sub_12989', 'sub_10d96', 'sub_10d9f',
    'sub_10dba', 'sub_12ab8', 'sub_128a9', 'sub_108c8', 'sub_12ef8',
    'sub_16528', 'sub_16546', 'sub_167ff', 'sub_16807', 'sub_1686f',
    'sub_17512', 'sub_1754c', 'sub_17561', 'sub_17749', 'sub_1774f',
    'sub_1775d', 'sub_17912', 'sub_1797b', 'sub_179a8', 'sub_179fb',
}
CLASS_D = {'sub_172d3', 'sub_17337'}                       # dead MDA debug
CLASS_SDL = {'sub_12352', 'sub_176bd', 'sub_177bb'}        # SDL-replaced bodies

EIP_RE = re.compile(r'^cs=0x1a2;eip=0x([0-9a-f]+);')
SUB_RE = re.compile(r'^(sub_[0-9a-f]+):')


def klass(name: str) -> str:
    if name in CLASS_L: return 'L'
    if name in CLASS_E: return 'E'
    if name in CLASS_D: return 'D'
    if name in CLASS_SDL: return 'SDL'
    return 'unit'


def load_gcov(path: str):
    op = gzip.open if path.endswith('.gz') else open
    with op(path, 'rt') as f:
        data = json.load(f)
    counts = {}          # line number -> execution count
    for fentry in data['files']:
        if not fentry['file'].endswith('vikings.exe_seg000.cpp'):
            continue
        for ln in fentry['lines']:
            n = ln['line_number']
            counts[n] = max(counts.get(n, 0), ln['count'])
    if not counts:
        sys.exit('no coverage rows for vikings.exe_seg000.cpp in ' + path)
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gcov_json')
    ap.add_argument('--src', default='src/vikings.exe_seg000.cpp')
    ap.add_argument('--min', type=float, default=100.0,
                    help='print subs with coverage%% < MIN (default: all below 100)')
    ap.add_argument('--csv', help='write full per-sub table as CSV')
    args = ap.parse_args()

    counts = load_gcov(args.gcov_json)
    src = open(args.src, encoding='utf-8', errors='replace').read().splitlines()

    # sub_ label -> [start_line, end_line) over the source; eip lines inside.
    subs = []            # (name, start_line)
    for i, line in enumerate(src, 1):
        m = SUB_RE.match(line)
        if m:
            subs.append((m.group(1), i))
    rows = []
    for idx, (name, start) in enumerate(subs):
        end = subs[idx + 1][1] if idx + 1 < len(subs) else len(src) + 1
        eips = []        # (line_number, eip_hex)
        for n in range(start, end):
            m = EIP_RE.match(src[n - 1])
            if m:
                eips.append((n, m.group(1)))
        total = len(eips)
        if total == 0:
            rows.append((name, 0, 0, 100.0, []))
            continue
        # a line is covered when gcov saw it executed at least once
        missed = [(n, e) for (n, e) in eips if counts.get(n, 0) == 0]
        execd = total - len(missed)
        pct = 100.0 * execd / total
        rows.append((name, total, execd, pct, missed))

    # ---- report ----
    by_class = {}
    for name, total, execd, pct, missed in rows:
        c = klass(name)
        agg = by_class.setdefault(c, [0, 0, 0])   # procs, insns, exec
        agg[0] += 1; agg[1] += total; agg[2] += execd
    print('== class summary (procedures / oracle instructions / executed / %) ==')
    for c in ('unit', 'L', 'E', 'D', 'SDL'):
        if c not in by_class: continue
        p, t, x = by_class[c]
        pc = (100.0 * x / t) if t else 100.0
        print(f'  {c:4} {p:4d} procs  {t:6d} insns  {x:6d} exec  {pc:6.2f}%')
    tt = sum(v[1] for v in by_class.values()); tx = sum(v[2] for v in by_class.values())
    print(f'  ALL  {sum(v[0] for v in by_class.values()):4d} procs  {tt:6d} insns  '
          f'{tx:6d} exec  {100.0 * tx / tt:6.2f}%')

    print(f'\n== unit-class subs below {args.min:.0f}% (uncovered eips listed) ==')
    shown = 0
    for name, total, execd, pct, missed in sorted(rows, key=lambda r: r[3]):
        if klass(name) != 'unit' or pct >= args.min or total == 0:
            continue
        shown += 1
        gaps = ' '.join(e for _, e in missed[:16])
        more = f' +{len(missed) - 16}' if len(missed) > 16 else ''
        print(f'  {name:11} {execd:4d}/{total:<4d} {pct:6.2f}%  missed: {gaps}{more}')
    if not shown:
        print('  (none — every unit-class procedure fully covered)')

    if args.csv:
        with open(args.csv, 'w') as f:
            f.write('sub,class,total_insns,executed,pct,missed_eips\n')
            for name, total, execd, pct, missed in rows:
                f.write(f'{name},{klass(name)},{total},{execd},{pct:.2f},'
                        f'"{" ".join(e for _, e in missed)}"\n')
        print(f'\nCSV: {args.csv}')


if __name__ == '__main__':
    main()
