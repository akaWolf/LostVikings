# Quarantined replays

Scenarios that reproduce known infrastructure bugs; kept OUT of
tests/replays/ so scenarios.sh and the coverage cycle don't hang.

- synth_title_f10.inp — task #58: the F10 quit-prompt opened FROM THE
  TITLE (level 0x2C, [288AC]=0x8000) deadlocks the headless run inside
  the sub_104a1 prompt loop (frame counter stops, --max-frames can never
  fire). ESC-pause from the title and F10 inside the demo level do NOT
  hang. Re-add to tests/replays/ once #58 is fixed.
