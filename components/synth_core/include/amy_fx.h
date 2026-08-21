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

/* Sentinel for the extended FX params. While a field holds it, fx_push_*
 * leaves the matching amy_event field at AMY_UNSET so AMY keeps its factory
 * default. Chosen outside every valid range, incl. negative echo tone. */
#define FX_PARAM_UNSET  INT16_MIN

typedef struct {
    int8_t  eq_low_db;     /* -15..+15 dB */
    int8_t  eq_mid_db;
    int8_t  eq_high_db;
    uint8_t echo_level;    /* 0..100 -> 0..1 */
    uint8_t chorus_level;  /* 0..100 -> 0..1 */
    uint8_t reverb_level;  /* 0..100 -> 0..1 */
    /* Extended global-FX params, FX_PARAM_UNSET until dialed in. Percent
     * fields map /100 to AMY's 0..1 float; others as noted. */
    int16_t echo_delay_ms;   /* 0..743 ms;   unset -> AMY 500 ms          */
    int16_t echo_feedback;   /* 0..99 (%);   unset -> AMY 0 (one repeat)  */
    int16_t echo_tone;       /* -99..99 (% filter coef); unset -> AMY 0   */
    int16_t reverb_liveness; /* 0..100 (%);  unset -> AMY 0.85            */
    int16_t reverb_damping;  /* 0..100 (%);  unset -> AMY 0.5             */
    int16_t reverb_xover_hz; /* 500..8000 Hz; unset -> AMY 3000 Hz        */
    int16_t chorus_rate;     /* centi-Hz (0.01 Hz); unset -> AMY 0.5 Hz   */
    int16_t chorus_depth;    /* 0..100 (%);  unset -> AMY 0.5             */
    /* Per-bus distortion, used at global scope (bus 0 - everything renders
     * there today). Concrete defaults, no sentinels: bus_reset()'s values
     * are known, unlike the factory FX above. */
    uint8_t bus_dist_type;   /* stage mask: bit0 CLIP, bit1 FOLD, bit2 CRUSH;
                                0 = OFF; mapped onto AMY's enables on push */
    uint8_t bus_dist_drive;  /* 1..16 pre-gain (fold depth for FOLD)       */
    uint8_t bus_dist_bits;   /* 1..24 CRUSH bit depth; 24 = no-op          */
    uint8_t bus_dist_rate;   /* 1..64 CRUSH sample-hold length in samples  */
    uint8_t bus_dist_mix;    /* 0..100 -> 0..1 wet/dry                     */
    /* False: loading a patch must NOT change the global FX. Every built-in
     * Juno patch string ends with `x<eq>k<chorus>` commands writing the single
     * global EQ/chorus, so without this guard a preset change on any synth
     * re-skins the whole mix. Patch-load sites call
     * synth_ui_fx_reassert_global(), which re-imposes these cached values
     * right after the patch's FX deltas, making presets timbre-only.
     * True: the most-recently-loaded preset's FX applies globally. */
    bool    presets_alter_global;
} fx_state_t;

/* Live FX cache: synth_ui.c's menu handler updates fields directly, then calls
 * the matching fx_push_* below. */
extern fx_state_t s_fx;

/* Push individual effect bands to AMY's global event bus. */
void fx_push_eq(void);
void fx_push_echo(void);
void fx_push_chorus(void);
void fx_push_reverb(void);
void fx_push_dist(void);

/* Re-impose the cached global FX after a patch load, since every built-in Juno
 * patch ends with global EQ/chorus commands that would re-skin the whole mix.
 * The sequencer/arp/drone patch-load paths call this right after loading; it
 * is a no-op while the "Preset FX" toggle is on. */
void synth_ui_fx_reassert_global(void);

/* Master output volume (0..2.0, unity=1.0), written to amy_global.volume[].
 * The 2x headroom allows boosting quiet sources. */
void  amy_fx_set_master_volume(float v);   /* clamps 0..2 and pushes to AMY */
float amy_fx_get_master_volume(void);      /* returns current cached value    */

#ifdef __cplusplus
}
#endif
