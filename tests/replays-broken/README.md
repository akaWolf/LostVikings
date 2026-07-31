# Quarantined replays

Scenarios that reproduce known infrastructure bugs; kept OUT of
tests/replays/ so scenarios.sh and the coverage cycle don't hang.

(empty — synth_title_f10 returned to tests/replays/ after #58 was fixed:
the "hang" was exit()'s static-destructor chain blocking forever in
pthread_cond_destroy while the v2 game thread still waited on the CV;
title-screen F10 is a DIRECT DOS quit in the original, no prompt. The
INT21/4C model now stops the worker threads, flushes gcov and _exit()s.)
