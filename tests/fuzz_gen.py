#!/usr/bin/env python3
# HEADLESS fuzz input generator — produces random replay file from seed.
#
# Usage: python3 fuzz_gen.py <seed> [duration_frames=10000] [density=0.005]

import sys
import random

ACTIONS = [
    "LEFT", "RIGHT", "UP", "DOWN", "SPACE", "RETURN", "TAB",
    "E", "S", "D", "F", "ESC", "LCTRL", "LALT", "F10",
    "Q", "Y", "A", "N", "F4", "F5", "F6", "1", "2", "3",
]

def gen(seed: int, duration: int = 10000, density: float = 0.005) -> None:
    random.seed(seed)
    print(f"# Fuzz replay — seed={seed} duration={duration} density={density}")
    print(f"# Format: <frame> <KD|KU> <ACTION>  (frame = v2_dbg_pre_vm_iter)")

    held: dict[str, int] = {}  # action → frame_pressed
    for f in range(duration):
        if random.random() < density:
            a = random.choice(ACTIONS)
            if a in held:
                print(f"{f} KU {a}")
                del held[a]
            else:
                print(f"{f} KD {a}")
                held[a] = f
        # auto-release after random hold duration
        for act in list(held.keys()):
            if f - held[act] > random.randint(2, 30):
                print(f"{f} KU {act}")
                del held[act]
    # cleanup any still-held
    for act in held:
        print(f"{duration} KU {act}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: fuzz_gen.py <seed> [duration] [density]", file=sys.stderr)
        sys.exit(2)
    seed = int(sys.argv[1])
    dur  = int(sys.argv[2]) if len(sys.argv) > 2 else 10000
    den  = float(sys.argv[3]) if len(sys.argv) > 3 else 0.005
    gen(seed, dur, den)
