#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Global FX state ────────────────────────────────────────────────────────
 * Cached mirror of the values last pushed to AMY's global effect bus.
 * AMY exposes no getters, so we keep our own copy for menu display.
 * All pushes go through the shared amy_helpers mutex (amy_event is ~800 B;
 * never allocate it on a task stack). */

/* Sentinel for the extended FX params below. While a field holds this value
 * its fx_push_* leaves the matching amy_event field at AMY_UNSET, so AMY keeps
 * its own factory default and the mix is unchanged until the user edits that
 * param. Chosen out of every valid range (incl. negative echo tone). */
#define FX_PARAM_UNSET  INT16_MIN

typedef struct {
    int8_t  eq_low_db;     /* -15..+15 dB */
    int8_t  eq_mid_db;
    int8_t  eq_high_db;
    uint8_t echo_level;    /* 0..100 -> 0..1 */
    uint8_t chorus_level;  /* 0..100 -> 0..1 */
    uint8_t reverb_level;  /* 0..100 -> 0..1 */
    /* Extended global-FX params. Each holds FX_PARAM_UNSET until the user
     * dials it in; while unset the matching amy_event field is left at its
     * AMY_UNSET default, so AMY keeps its own boot value. Percent fields map
     * /100 to AMY's 0..1 float; others as noted. */
    int16_t echo_delay_ms;   /* 0..743 ms;   unset -> AMY 500 ms          */
    int16_t echo_feedback;   /* 0..99 (%);   unset -> AMY 0 (one repeat)  */
    int16_t echo_tone;       /* -99..99 (% filter coef); unset -> AMY 0   */
    int16_t reverb_liveness; /* 0..100 (%);  unset -> AMY 0.85            */
    int16_t reverb_damping;  /* 0..100 (%);  unset -> AMY 0.5             */
    int16_t reverb_xover_hz; /* 500..8000 Hz; unset -> AMY 3000 Hz        */
    int16_t chorus_rate;     /* centi-Hz (0.01 Hz); unset -> AMY 0.5 Hz   */
    int16_t chorus_depth;    /* 0..100 (%);  unset -> AMY 0.5             */
    /* When false, loading a synth patch must NOT change the global FX: every
     * AMY built-in Juno patch string ends with `x<eq>k<chorus>` commands that
     * write the single global EQ/chorus (amy_global.*), so without this guard a
     * preset change on any one synth (sequencer row, arp, drone) re-skins the
     * whole mix's FX. The patch-load sites call synth_ui_fx_reassert_global()
     * which, when this is false, re-imposes these cached user values right after
     * the patch's FX deltas, making presets effectively per-synth (timbre only).
     * When true, the most-recently-loaded preset's FX is allowed to apply
     * globally (the original behaviour). */
    bool    presets_alter_global;
} fx_state_t;

/* Live FX cache — written by the menu handler in synth_ui.c, read by the
 * fx_push_* functions below.  Declared non-static so synth_ui.c can update
 * individual fields directly before calling the corresponding fx_push_*. */
extern fx_state_t s_fx;

/* Push individual effect bands to AMY's global event bus. */
void fx_push_eq(void);
void fx_push_echo(void);
void fx_push_chorus(void);
void fx_push_reverb(void);

/* Re-impose the cached global FX (EQ/echo/chorus/reverb) after a synth patch
 * load. Every AMY built-in Juno patch ends with global EQ/chorus commands, so
 * loading a preset onto any synth would otherwise re-skin the whole mix's FX.
 * The sequencer/arp/drone patch-load paths call this immediately after loading;
 * it is a no-op while the user has enabled the "Preset FX" menu toggle (i.e.
 * deliberately letting presets drive the global FX). */
void synth_ui_fx_reassert_global(void);

/* Master output volume (0..2.0, unity=1.0).  Written to amy_global.volume[]
 * on every change.  2× headroom allows boosting quiet sources. */
void  amy_fx_set_master_volume(float v);   /* clamps 0..2 and pushes to AMY */
float amy_fx_get_master_volume(void);      /* returns current cached value    */

#ifdef __cplusplus
}
#endif
