#!/usr/bin/env python3
"""HEADLESS Tier 3: coverage-guided fuzz.

Maintains corpus of (input_file, executed_opcodes) pairs. Each iteration:
  1. Pick random input from corpus
  2. Mutate (insert/delete/swap/modify events)
  3. Run headless → parse B5 opcode coverage from run.log
  4. If new opcodes executed → save mutated input to corpus
  5. If divergence/crash → save to bugs dir
  6. Print stats periodically

Goal: drive 209 unexecuted main VM opcodes + 10 unexecuted anim cmds toward
coverage. Inputs hitting rare paths likely to find rare divergences.

Usage:
  ./fuzz_coverage.py [iterations=10000] [duration=5000]
"""

from __future__ import annotations
import os, sys, re, random, shutil, subprocess, time
from pathlib import Path
from dataclasses import dataclass, field

# ---------- config ----------
ROOT      = Path(__file__).resolve().parent.parent
HEADLESS  = ROOT / "vikings_headless"
CORPUS    = Path("/tmp/headless_corpus")
BUGS      = Path("/tmp/headless_bugs")
CORPUS.mkdir(parents=True, exist_ok=True)
BUGS.mkdir(parents=True, exist_ok=True)

ACTIONS = [
    "LEFT", "RIGHT", "UP", "DOWN", "SPACE", "RETURN", "TAB",
    "E", "S", "D", "F", "ESC", "LCTRL", "LALT", "F10",
    "Q", "Y", "A", "N", "F4", "F5", "F6", "1", "2", "3",
]

# ---------- B5 parser ----------
RE_MAIN_HEADER = re.compile(r"Main VM opcodes.*?(\d+)\s+executed,\s+(\d+)\s+never")
RE_ANIM_HEADER = re.compile(r"Anim cmds.*?(\d+)\s+executed,\s+(\d+)\s+never")
RE_OP_LINE     = re.compile(r"^\s+(op|anim)_([0-9A-Fa-f]+):\s+(\d+)")

def parse_opcode_coverage(log_path: Path) -> dict:
    """Returns {'main': set[int], 'anim': set[int]} of executed opcodes."""
    main = set()
    anim = set()
    if not log_path.exists():
        return {"main": main, "anim": anim}
    with log_path.open() as f:
        for line in f:
            m = RE_OP_LINE.match(line)
            if not m: continue
            kind, hex_, count = m.groups()
            op = int(hex_, 16)
            if int(count) > 0:
                (main if kind == "op" else anim).add(op)
    return {"main": main, "anim": anim}

# ---------- input gen + mutation ----------
@dataclass
class Event:
    frame: int
    kind: str   # "KD" or "KU"
    action: str
    def __str__(self): return f"{self.frame} {self.kind} {self.action}"

def gen_random(seed: int, duration: int = 5000, density: float = 0.005) -> list[Event]:
    rnd = random.Random(seed)
    events: list[Event] = []
    held: dict[str, int] = {}
    for f in range(duration):
        if rnd.random() < density:
            a = rnd.choice(ACTIONS)
            if a in held:
                events.append(Event(f, "KU", a)); del held[a]
            else:
                events.append(Event(f, "KD", a)); held[a] = f
        for act in list(held):
            if f - held[act] > rnd.randint(2, 30):
                events.append(Event(f, "KU", act)); del held[act]
    for act in held:
        events.append(Event(duration, "KU", act))
    return events

def parse_inp(path: Path) -> list[Event]:
    events: list[Event] = []
    with path.open() as f:
        for line in f:
            if line.startswith("#") or not line.strip(): continue
            parts = line.split()
            if len(parts) != 3: continue
            try:
                events.append(Event(int(parts[0]), parts[1], parts[2]))
            except ValueError:
                pass
    return events

def write_inp(path: Path, events: list[Event]) -> None:
    with path.open("w") as f:
        f.write("# coverage-guided fuzz input\n")
        f.write("# Format: <frame> <KD|KU> <ACTION>\n")
        for e in events:
            f.write(f"{e}\n")

def mutate(events: list[Event], rnd: random.Random) -> list[Event]:
    """Apply 1-3 random mutations."""
    out = list(events)
    for _ in range(rnd.randint(1, 3)):
        if not out:
            # bootstrap: insert single random event
            out.append(Event(rnd.randint(0, 1000), "KD", rnd.choice(ACTIONS)))
            continue
        op = rnd.choice(["insert", "delete", "swap_frame", "change_action", "duplicate"])
        if op == "insert":
            idx = rnd.randint(0, len(out))
            f   = rnd.randint(0, max(1, max(e.frame for e in out)))
            kind= rnd.choice(["KD", "KU"])
            out.insert(idx, Event(f, kind, rnd.choice(ACTIONS)))
        elif op == "delete":
            del out[rnd.randint(0, len(out)-1)]
        elif op == "swap_frame":
            i = rnd.randint(0, len(out)-1)
            shift = rnd.randint(-50, 50)
            out[i].frame = max(0, out[i].frame + shift)
        elif op == "change_action":
            i = rnd.randint(0, len(out)-1)
            out[i].action = rnd.choice(ACTIONS)
        elif op == "duplicate":
            i = rnd.randint(0, len(out)-1)
            new_frame = max(0, out[i].frame + rnd.randint(-10, 10))
            out.insert(i+1, Event(new_frame, out[i].kind, out[i].action))
    out.sort(key=lambda e: e.frame)
    return out

# ---------- corpus management ----------
@dataclass
class CorpusEntry:
    path: Path
    main_cov: set = field(default_factory=set)
    anim_cov: set = field(default_factory=set)

def load_corpus() -> list[CorpusEntry]:
    """Load existing corpus inputs + initial seed from empty + scenarios."""
    entries: list[CorpusEntry] = []
    # Bootstrap from project replays
    for inp in (ROOT / "tests/replays").glob("*.inp"):
        entries.append(CorpusEntry(path=inp))
    # Add previously-saved corpus
    for inp in CORPUS.glob("*.inp"):
        entries.append(CorpusEntry(path=inp))
    return entries

# ---------- runner ----------
def run_headless(inp_path: Path, seed: int, max_frames: int) -> tuple[int, Path]:
    """Run headless on input. Returns (exit_code, dump_dir_path)."""
    dump_dir = Path(f"/tmp/cov_run_{os.getpid()}_{seed}")
    if dump_dir.exists():
        shutil.rmtree(dump_dir)
    dump_dir.mkdir()
    log = dump_dir / "run.log"
    with log.open("w") as f:
        p = subprocess.run(
            [str(HEADLESS),
             f"--replay-input={inp_path}",
             f"--max-frames={max_frames}",
             f"--dump-dir={dump_dir}",
             f"--seed={seed}"],
            stdout=f, stderr=subprocess.STDOUT, timeout=60,
        )
    return p.returncode, dump_dir

# ---------- main loop ----------
def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    duration   = int(sys.argv[2]) if len(sys.argv) > 2 else 5000

    if not HEADLESS.exists():
        print(f"FAIL: {HEADLESS} not built. Run: HEADLESS=1 make -j$(nproc)")
        sys.exit(2)

    rnd = random.Random()
    corpus = load_corpus()

    # Seed corpus from B5 output of first runs
    print(f"Bootstrapping coverage from {len(corpus)} corpus inputs...")
    global_main: set = set()
    global_anim: set = set()
    for i, entry in enumerate(corpus):
        ec, dump = run_headless(entry.path, seed=i, max_frames=duration)
        cov = parse_opcode_coverage(dump / "run.log")
        entry.main_cov = cov["main"]
        entry.anim_cov = cov["anim"]
        global_main |= cov["main"]
        global_anim |= cov["anim"]
        if ec == 1:
            print(f"  bootstrap: {entry.path.name} → divergence (saved)")
            shutil.copytree(dump, BUGS / f"bootstrap_{entry.path.stem}_{int(time.time())}",
                            dirs_exist_ok=True)
        shutil.rmtree(dump)
    print(f"Initial coverage: main={len(global_main)}/216  anim={len(global_anim)}/27")
    print(f"Corpus size: {len(corpus)}")
    print()

    # Coverage-guided loop
    bugs = 0
    new_cov_finds = 0
    started = time.time()
    for it in range(iterations):
        if not corpus:
            # Empty corpus → generate from random seed
            events = gen_random(rnd.randint(1, 1<<30), duration)
        else:
            seed_entry = rnd.choice(corpus)
            events = mutate(parse_inp(seed_entry.path), rnd)

        # Write mutated input
        mutated_path = Path(f"/tmp/cov_mut_{os.getpid()}.inp")
        write_inp(mutated_path, events)

        seed = rnd.randint(1, 1<<30)
        try:
            ec, dump = run_headless(mutated_path, seed, duration)
        except subprocess.TimeoutExpired:
            print(f"[{it}] TIMEOUT — skip")
            continue

        cov = parse_opcode_coverage(dump / "run.log")
        new_main = cov["main"] - global_main
        new_anim = cov["anim"] - global_anim

        if ec == 1:
            # Divergence found → save
            bugs += 1
            bug_dir = BUGS / f"cov_bug_{int(time.time())}_{seed}"
            shutil.copytree(dump, bug_dir, dirs_exist_ok=True)
            shutil.copy(mutated_path, bug_dir / "replay.inp")
            print(f"[{it}] BUG → {bug_dir}")
        elif ec == 4:
            bugs += 1
            bug_dir = BUGS / f"cov_crash_{int(time.time())}_{seed}"
            shutil.copytree(dump, bug_dir, dirs_exist_ok=True)
            shutil.copy(mutated_path, bug_dir / "replay.inp")
            print(f"[{it}] CRASH → {bug_dir}")

        if new_main or new_anim:
            # New coverage → add to corpus
            new_cov_finds += 1
            corpus_path = CORPUS / f"cov_{int(time.time())}_{seed}.inp"
            shutil.copy(mutated_path, corpus_path)
            entry = CorpusEntry(path=corpus_path, main_cov=cov["main"], anim_cov=cov["anim"])
            corpus.append(entry)
            global_main |= cov["main"]
            global_anim |= cov["anim"]
            print(f"[{it}] NEW COV: +{len(new_main)} main +{len(new_anim)} anim "
                  f"(total main={len(global_main)}/216 anim={len(global_anim)}/27)")

        shutil.rmtree(dump)

        # Periodic stats
        if (it + 1) % 100 == 0:
            elapsed = time.time() - started
            rate = (it+1) / elapsed
            print(f"  [{it+1}/{iterations}] rate={rate:.1f}/s  "
                  f"corpus={len(corpus)}  cov={len(global_main)}/216  bugs={bugs}  "
                  f"new_cov_finds={new_cov_finds}")

    print()
    print(f"=== Done: {iterations} iterations ===")
    print(f"  Final coverage: main={len(global_main)}/216  anim={len(global_anim)}/27")
    print(f"  Corpus size: {len(corpus)}  Bugs: {bugs}")
    print(f"  Bugs dir: {BUGS}/")

if __name__ == "__main__":
    main()
