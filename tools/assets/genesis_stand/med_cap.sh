#!/usr/bin/env bash
# Genesis ground-truth stand, step 2: play an attract-patched ROM in Mednafen
# (software renderer, no audio) under Xvfb and grab the X root every 0.5 s.
# Keys never reach Mednafen under Xvfb (SDL2 + XTEST), so the flow is
# input-free: boot -> title -> attract = the scene (~25 s in). Mednafen runs
# at roughly 1/3 speed under the software path: read times as ~3x the game's.
#   nix-shell -p mednafen xorg-server imagemagick --run "bash med_cap.sh ROM TAG SECONDS"
# -> cap_TAG/f_NNN.png (1024x768; the game window sits at +220+160, 584x448)
ROM=$1; TAG=$2; SECS=${3:-70}
S=${STAND_DIR:-/tmp/lv_genesis_stand}
export HOME=$S/mhome MEDNAFEN_HOME=$S/mhome SDL_AUDIODRIVER=dummy DISPLAY=:79
mkdir -p $S/mhome $S/cap_$TAG; rm -f $S/cap_$TAG/*
Xvfb :79 -screen 0 1024x768x24 -ac > /dev/null 2>&1 & XV=$!; sleep 1
mednafen -video.driver softfb -video.fs 0 -md.xscale 2 -md.yscale 2 "$ROM" > $S/cap_$TAG/mednafen.log 2>&1 & ME=$!; sleep 3
N=$(python3 -c "print(int($SECS/0.5))")
for i in $(seq 0 $N); do import -window root $S/cap_$TAG/f_$(printf %03d $i).png 2>/dev/null; sleep 0.42; done
kill -INT $ME; sleep 2; kill $XV 2>/dev/null
echo "captured $(ls $S/cap_$TAG/f_*.png | wc -l)"
