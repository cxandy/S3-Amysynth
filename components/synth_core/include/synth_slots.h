#pragma once

/* ── AMY synth slot map - single source of truth ─────────────────────────
 * Static consumers pack the bottom of the slot space; the melodic sequencer
 * is an open-ended arena on top. Growing melodic capacity is one edit:
 * raise SYNTH_SLOT_COUNT (the real ceiling is AMY's osc pool, max_oscs).
 *
 * Contract:
 * - Slot 0 is reserved as a sentinel: sequencer row lookups use synth id 0
 *   for "no such row" (seq_core_synth.c). Never assign it to a consumer.
 * - Static slots (1..SEQ_MEL_SYNTH_BASE-1) are compile-time fixed, never
 *   overlap, and are identical across targets.
 * - Melodic allocation runs [SEQ_MEL_SYNTH_BASE, SEQ_MAX_SYNTH]; the
 *   static-vs-melodic predicate is a single `slot >= SEQ_MEL_SYNTH_BASE`.
 * - amy_cfg.max_synths (main.c) must be SYNTH_SLOT_COUNT - it is, by
 *   construction, the only other place this map surfaces. */

#define SEQ_ARP_SYNTH          1    /* standalone arpeggiator                */
#define DRONE_SYNTH_MAIN       2    /* stutter-house drone, chord voices     */
#define DRONE_SYNTH_SUB        3    /* stutter-house drone, sub tone         */
#define DRONE_STD_SYNTH_MAIN   4    /* normal (free-running) drone, chord    */
#define DRONE_STD_SYNTH_SUB    5    /* normal drone, sub tone                */
#define SEQ_DRUM_SYNTH_BASE    6    /* 4 drum tracks: 6..9                   */
#define LIVE_SYNTH             10   /* live-play (BLE/USB MIDI) voice        */
#define SEQ_MEL_SYNTH_BASE     11   /* first melodic slot; arena from here up */

#define SYNTH_SLOT_COUNT       63   /* == amy_cfg.max_synths; THE polyphony knob */
#define SEQ_MAX_SYNTH          (SYNTH_SLOT_COUNT - 1)   /* melodic ceiling   */
