// M1 call-parity channel (docs2/VERIFICATION_GAPS_ANALYSIS.md, task #65).
//
// Per-procedure CALL counters on both sides of the mirror:
//   * the orig side bumps v2_cc_orig_hit(id) via an m2c inline at the
//     procedure entry;
//   * every v2 mirror bumps v2_cc_v2_hit(id) at its entry.
// Counters are CUMULATIVE. v2_record_orig_phase_snap() snapshots the orig
// vector next to the DS snapshot; v2_compare_phase_snap() compares the v2
// vector against the snapshotted one at the same logical point — a lost or
// extra call anywhere in the phase shows up as a per-id delta even when the
// call leaves no DS trace (SFX, vsync waits, renders).
//
// First wave: the mirrored procedures whose calls are invisible to the
// state-diff channels (class A/D of the analysis). sub_12352 is deliberately
// NOT counted: its v2 mirror runs only in V2_ONLY (legal count skew).
#pragma once
#include <stdint.h>

enum V2CallCountId {
    CC_10130 = 0,   // vsync wait
    CC_16775,       // page flip
    CC_1183D,       // HUD item render
    CC_118AD,       // HUD selector render
    CC_120D1,       // selector commit (prev<-cur + 3 renders)
    CC_11F47,       // pause nearby-viking gates
    CC_11F93,       // place carried item
    CC_121B9,       // pop selected item
    CC_121F6,       // take-next item
    CC_1DE05,       // bg latch / dirty-tile redraw (seg003)
    CC_1DD9C,       // late sprite pass (seg003)
    CC_1C8F1,       // flagged-tile FS update (seg003)
    CC_1CD7D,       // sprite dirty-rect render (seg003; 1CD7B falls in)
    CC_165AA,       // page rotate
    CC_16661,       // scroll tracker
    CC_108C8,       // audio tick
    CC_COUNT
};

#ifdef __cplusplus
extern "C" {
#endif
void v2_cc_orig_hit(int id);
void v2_cc_v2_hit(int id);
#ifdef __cplusplus
}
#endif
