#pragma once

/* Public and standard headers — order mirrors original sequencer_core.c includes */
#include "sequencer_core.h"
#include "custompatches/bass_presets.h"
#include "custompatches/fm_presets.h"
#include "custompatches/fm_voice.h"
#include "arp_core.h"
#include "amy.h"
#include "amy_helpers.h"
#include "sequencer.h"
#include "quantizer.h"
#include "seq_clamp.h"
#include "seq_defaults.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>
#include "freertos/semphr.h"
#include "amy_fx.h"   /* synth_ui_fx_reassert_global() — avoids u8g2/display headers */
#include "patch_cycle.h"

/* Private config — included after sdkconfig.h so CONFIG_* are resolved first */
#include "seq_core_config.h"

/* ── Logging tag — static per TU, one copy each, no ODR conflict ────── */
static const char * const TAG = "seq_core";

/* ── Heap integrity check, gated by Kconfig ─────────────────────────── */
#if CONFIG_AMYSYNTH_HEAP_CHECK
#define CORE_HEAP_CHECK(where) do { \
    if (!heap_caps_check_integrity_all(true)) { \
        ESP_LOGE(TAG, "HEAP CORRUPT detected at: %s", where); \
    } else { \
        ESP_LOGI(TAG, "HEAP OK at: %s", where); \
    } \
} while (0)
#else
#define CORE_HEAP_CHECK(where) do { (void)(where); } while (0)
#endif

/* ── External dependency ─────────────────────────────────────────────── */
extern uint32_t sequencer_ticks(void);

/* ── Private types ───────────────────────────────────────────────────── */

/* Global chord progression (internal representation) */
typedef struct {
    uint8_t      root;           /* chromatic pitch class 0-11 */
    chord_type_t chord_type;
    uint8_t      duration_bars;  /* 1 / 2 / 4 / 8 / 16 */
} chord_prog_entry_t;

typedef struct {
    chord_prog_entry_t entries[CHORD_PROG_MAX_ENTRIES];
    uint8_t            count;
    uint8_t            current;
    uint32_t           entry_start_bar; /* bars_elapsed when current entry began */
    bool               enabled;
} chord_progression_t;

/* ── Shared state — defined (non-static) in the owning .c file ───────── */

/* Owned by seq_core_state.c */
extern seq_layer_t    s_layers[];
extern uint8_t        s_num_layers;
extern bool           s_playing;
extern uint8_t        s_next_melodic_synth;
extern uint16_t       s_melodic_patch;
extern volatile bool  s_layers_mutating;   /* guards delete_layer's compaction vs. the tick */

/* Owned by seq_core_engine.c */
extern uint8_t        s_cached_step[];
extern uint8_t        s_track_source_note[][SEQ_TRACKS];
extern uint32_t       s_bar_baseline;

/* Owned by seq_core_synth.c */
extern seq_drum_engine_t s_drum_engine;

/* Owned by seq_core_tempo.c */
extern uint16_t       s_bpm;
extern quantizer_state_t s_quantizer;

/* Owned by seq_core_editors.c */
extern float          s_lfo_phase[][SEQ_TRACKS];
extern float          s_lfo_hz[][SEQ_TRACKS];
extern float          s_lfo_rnd[][SEQ_TRACKS];
extern uint32_t       s_lfo_rng_state;

/* Owned by seq_core_progression.c */
extern chord_progression_t s_prog;
extern volatile bool  s_prog_apply_pending;

/* Owned by seq_core_trig.c */
extern uint32_t s_layer_loop_count[];              /* MAX_LAYERS   */
extern uint8_t  s_layer_last_step[];                /* MAX_LAYERS, 0xFF = unseen */
extern bool     s_track_last_played[][SEQ_TRACKS];  /* MAX_LAYERS x SEQ_TRACKS */

/* ── Private function declarations — all defined non-static in owning .c ─ */

/* From seq_core_engine.c */
void     sequencer_emit_step(uint8_t layer_idx, uint8_t track, uint8_t step);
void     sequencer_emit_clear_tag(uint32_t tag);
void     sequencer_resync_layer(uint8_t layer_idx);
void     sequencer_clear_layer_tags(uint8_t layer_idx);
void     sequencer_refresh_melodic_layers(bool preview);
uint32_t sequencer_bars_elapsed(void);

/* From seq_core_synth.c */
void      sequencer_configure_synth(uint8_t layer_idx);
void      sequencer_kill_synth_voices(uint8_t synth_id);
seq_env_t *seq_layer_env(uint8_t layer_idx, uint8_t track);
seq_env_t *seq_layer_env1(uint8_t layer_idx, uint8_t track);

/* From seq_core_editors.c */
void sequencer_configure_melodic_envelope_track(uint8_t layer_idx, uint8_t track);
void sequencer_configure_melodic_envelope1_track(uint8_t layer_idx, uint8_t track);
void sequencer_configure_melodic_filter_track(uint8_t layer_idx, uint8_t track);
void sequencer_configure_melodic_lfo(uint8_t layer_idx);
void melodic_lfo_refresh_native_freq(void);

/* From seq_core_tempo.c */
uint16_t sequencer_clamp_bpm(uint16_t b);
void     sequencer_push_tempo(uint16_t b);
/* lfo_rate_to_hz is already declared in the public sequencer_core.h */
float    lfo_next_rand(void);
void     lfo_push_target_neutral(uint8_t synth_id, lfo_target_t target);

/* From seq_core_progression.c */
void chord_progression_apply_current(void);

/* From seq_core_engine.c — shared with seq_core_trig.c so ratchet sub-hits use
 * the exact same accent/jitter velocity curve as the plain periodic path. */
float sequencer_step_velocity(const seq_layer_t *layer, uint8_t track, uint8_t step);

/* From seq_core_engine.c — shared with seq_core_trig.c so the decorated-step
 * ratchet path respects mute/solo the same way the plain periodic path does. */
bool sequencer_track_audible(const seq_layer_t *layer, uint8_t track);

/* From seq_core_engine.c — shared with seq_core_trig.c so the per-step
 * note-transform can re-clamp to the layer's note bounds and re-snap a
 * transformed pitch through the exact same chord/scale quantizer the plain
 * per-track resolve uses (spec 20 §3.1/§3.2). resolve applies the scale/chord
 * snap; clamp only bounds the note (used when step_quant_bypass is set). */
uint8_t sequencer_clamp_layer_note(const seq_layer_t *layer, uint8_t note);
uint8_t sequencer_resolve_track_note(const seq_layer_t *layer, uint8_t source_note);

/* From seq_core_trig.c — per-step probability/ratchet/conditional-trig engine.
 * sequencer_core_step_is_decorated() is consulted by sequencer_emit_step()
 * (seq_core_engine.c) to decide whether a step still uses the plain
 * always-on periodic AMY tag, or is left cleared for the trig engine to
 * one-shot schedule instead. */
bool sequencer_core_step_is_decorated(const seq_layer_t *layer, uint8_t track, uint8_t step);
void sequencer_core_trig_reset(uint8_t layer_idx);   /* called on play-start, one layer   */
void sequencer_core_trig_clear_all(uint8_t layer_idx); /* called on pause, one layer       */
void sequencer_core_trig_reset_all(void);            /* called on layer add/delete        */
