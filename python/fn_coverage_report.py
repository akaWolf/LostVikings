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
# Additional exact zone boundaries (no heuristics): m2c's own independent-
# procedure markers and loc_ labels that are real inter-procedure CALL
# targets (CALL(_groupN, m2c::kloc_XXXX) sites). Without them a sub_'s zone
# swallows the neighbouring loc_-entry bodies and tail-JMP dead space.
PROC_RE = re.compile(r'^seg000_[0-9a-f]+_proc:')
CALL_KLOC_RE = re.compile(r'CALL\(_group[0-9]+,m2c::kloc_([0-9a-f]+)')


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
    branches = {}        # line number -> list of edge counts (merged by max)
    for fentry in data['files']:
        if not fentry['file'].endswith('vikings.exe_seg000.cpp'):
            continue
        for ln in fentry['lines']:
            n = ln['line_number']
            counts[n] = max(counts.get(n, 0), ln['count'])
            br = [b.get('count', 0) for b in ln.get('branches', [])]
            if br:
                prev = branches.get(n)
                if prev is None or len(prev) != len(br):
                    branches[n] = br
                else:
                    branches[n] = [max(a, b) for a, b in zip(prev, br)]
    if not counts:
        sys.exit('no coverage rows for vikings.exe_seg000.cpp in ' + path)
    return counts, branches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gcov_json')
    ap.add_argument('--src', default='src/vikings.exe_seg000.cpp')
    ap.add_argument('--min', type=float, default=100.0,
                    help='print subs with coverage%% < MIN (default: all below 100)')
    ap.add_argument('--csv', help='write full per-sub table as CSV')
    ap.add_argument('--live', help='second gcov json (units+replays merged) — adds a unit+live column')
    args = ap.parse_args()

    counts, branches = load_gcov(args.gcov_json)
    live_counts = None
    if args.live:
        live_counts, _ = load_gcov(args.live)
    src = open(args.src, encoding='utf-8', errors='replace').read().splitlines()

    # Zone boundaries: sub_ labels + m2c proc markers + called-into loc_
    # labels. A sub_'s zone ends at the NEXT boundary of any kind, so the
    # neighbouring loc_-entry bodies stop inflating its instruction count.
    kloc_targets = set()
    for line in src:
        for m in CALL_KLOC_RE.finditer(line):
            kloc_targets.add('loc_' + m.group(1))
    boundaries = []      # (line, name or None) — None = cut-only marker
    subs = []            # (name, start_line)
    for i, line in enumerate(src, 1):
        m = SUB_RE.match(line)
        if m:
            subs.append((m.group(1), i))
            boundaries.append((i, m.group(1)))
            continue
        if PROC_RE.match(line):
            boundaries.append((i, None))
            continue
        lm = re.match(r'^(loc_[0-9a-f]+):', line)
        if lm and lm.group(1) in kloc_targets:
            boundaries.append((i, lm.group(1)))
    boundary_lines = sorted(b[0] for b in boundaries)
    import bisect
    def zone_end(start):
        j = bisect.bisect_right(boundary_lines, start)
        return boundary_lines[j] if j < len(boundary_lines) else len(src) + 1
    rows = []
    loc_rows = []        # called-into loc_ entries, reported separately
    for (bl, bname) in boundaries:
        if bname is None or not bname.startswith('loc_'):
            continue
        end_l = zone_end(bl)
        eips_l = []
        for n in range(bl, end_l):
            m = EIP_RE.match(src[n - 1])
            if m:
                eips_l.append((n, m.group(1)))
        if eips_l:
            missed_l = [(n, e) for (n, e) in eips_l if counts.get(n, 0) == 0]
            loc_rows.append((bname, len(eips_l), len(eips_l) - len(missed_l), missed_l))
    for idx, (name, start) in enumerate(subs):
        end = zone_end(start)
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
        lexecd = execd
        if live_counts is not None:
            lexecd = total - sum(1 for (n, e) in eips if live_counts.get(n, 0) == 0)
        rows.append((name, total, execd, pct, missed, lexecd, eips))

    # ---- report ----
    by_class = {}
    for name, total, execd, pct, missed, lexecd, eips in rows:
        c = klass(name)
        agg = by_class.setdefault(c, [0, 0, 0, 0])   # procs, insns, exec, live-exec
        agg[0] += 1; agg[1] += total; agg[2] += execd; agg[3] += lexecd
    hdr = '== class summary (procs / insns / unit-exec / %'
    hdr += ' / +live %) ==' if live_counts is not None else ') =='
    print(hdr)
    for c in ('unit', 'L', 'E', 'D', 'SDL'):
        if c not in by_class: continue
        p, t, x, lx = by_class[c]
        pc = (100.0 * x / t) if t else 100.0
        line = f'  {c:4} {p:4d} procs  {t:6d} insns  {x:6d} exec  {pc:6.2f}%'
        if live_counts is not None:
            line += f'  | +live {100.0 * lx / t if t else 100.0:6.2f}%'
        print(line)
    tt = sum(v[1] for v in by_class.values()); tx = sum(v[2] for v in by_class.values())
    tlx = sum(v[3] for v in by_class.values())
    line = (f'  ALL  {sum(v[0] for v in by_class.values()):4d} procs  {tt:6d} insns  '
            f'{tx:6d} exec  {100.0 * tx / tt:6.2f}%')
    if live_counts is not None:
        line += f'  | +live {100.0 * tlx / tt:6.2f}%'
    print(line)

    # ---- branch coverage: only asm-branch lines (J(Jcc)/LOOP macros) ----
    BR_RE = re.compile(r'\bJ\(J[A-Z]+|\bR\(LOOP|\bJ\(LOOP')
    br_total = br_full = br_half = br_zero = 0
    half_list = []
    for name, total, execd, pct, missed, lexecd, eips in rows:
        for (n, e) in eips:
            if not BR_RE.search(src[n - 1]):
                continue
            br = branches.get(n)
            if not br or len(br) < 2:
                continue
            br_total += 1
            taken = sum(1 for b in br if b > 0)
            if taken == len(br):
                br_full += 1
            elif taken == 0:
                br_zero += 1
            else:
                br_half += 1
                half_list.append((name, e))
    if br_total:
        print(f'\n== branch coverage (Jcc/LOOP lines with edge data) ==')
        print(f'  total {br_total}  both-edges {br_full} ({100.0*br_full/br_total:.2f}%)  '
              f'one-edge {br_half}  none {br_zero}')
        by_sub = {}
        for name, e in half_list:
            by_sub.setdefault(name, []).append(e)
        print(f'  one-edge branches by sub (top 20):')
        for name, es in sorted(by_sub.items(), key=lambda kv: -len(kv[1]))[:20]:
            print(f'    {name:11} {len(es):3d}: {" ".join(es[:8])}{" +" + str(len(es)-8) if len(es) > 8 else ""}')

    print(f'\n== unit-class subs below {args.min:.0f}% (uncovered eips listed) ==')
    shown = 0
    for name, total, execd, pct, missed, lexecd, eips in sorted(rows, key=lambda r: r[3]):
        if klass(name) != 'unit' or pct >= args.min or total == 0:
            continue
        shown += 1
        gaps = ' '.join(e for _, e in missed[:16])
        more = f' +{len(missed) - 16}' if len(missed) > 16 else ''
        print(f'  {name:11} {execd:4d}/{total:<4d} {pct:6.2f}%  missed: {gaps}{more}')
    if not shown:
        print('  (none — every unit-class procedure fully covered)')

    if loc_rows:
        print('\n== called-into loc_ entries (separate zones) ==')
        for name, total, execd, missed in loc_rows:
            pct = 100.0 * execd / total
            gaps = ' '.join(e for _, e in missed[:10])
            print(f'  {name:11} {execd:4d}/{total:<4d} {pct:6.2f}%  missed: {gaps}')

    if args.csv:
        with open(args.csv, 'w') as f:
            f.write('sub,class,total_insns,executed,pct,missed_eips\n')
            for name, total, execd, pct, missed, lexecd, eips in rows:
                f.write(f'{name},{klass(name)},{total},{execd},{pct:.2f},'
                        f'"{" ".join(e for _, e in missed)}"\n')
        print(f'\nCSV: {args.csv}')


if __name__ == '__main__':
    main()
