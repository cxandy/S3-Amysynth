#pragma once

/* ── Sequencer / audio data model ──────────────────────────────────────────
 * Engine-owned shared model for the step sequencer. Extracted from the display
 * leaf header (display_seq.h) so the model lives in the layer that owns and
 * mutates it (sequencer_core), not in a view component. display_seq.h now
 * includes this header and retains only render-view types (ui_mode_t,
 * display_seq_state_t, the draw prototype). This is also the home for per-step
 * parameter-lock storage widening (see the env/filter/lfo notes below). */

#include "chord_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sequencer dimensions ── */
#define SEQ_TRACKS    4
#define SEQ_STEPS     16          /* default steps for a new layer  */
#define SEQ_MAX_STEPS 32          /* maximum steps supported         */
#define MAX_LAYERS    4           /* compile-time layer limit        */

/* ── Layer type ── */
typedef enum {
    SEQ_LAYER_DRUM    = 0,
    SEQ_LAYER_MELODIC = 1,
} seq_layer_type_t;

/* ── Per-track repeat rate (fires every N bars instead of every bar) ── */
typedef enum {
    SEQ_REPEAT_1  = 1,
    SEQ_REPEAT_2  = 2,
    SEQ_REPEAT_4  = 4,
    SEQ_REPEAT_8  = 8,
} seq_repeat_rate_t;

/* ── Per-step ratchet (sub-trigger count within one step's slot) ──
 * 1 = plain single trigger (the historical/default behaviour). >1 subdivides
 * the step evenly into that many evenly-spaced hits, each with its own short
 * gate — an Elektron-style "ratchet". */
#define SEQ_MAX_RATCHET 4

/* ── Per-step conditional trig (Elektron-style) ──
 * NONE: unconditional — the step fires whenever it is ON, subject only to
 *       probability/ratchet.
 * FILL: fires only once every step_cond_param loops of the layer's pattern
 *       (param clamped 2..8; the first loop after play-start always counts).
 * PREV: fires only if the immediately preceding step on the SAME track
 *       actually sounded on its own last evaluation — chains a run of hits,
 *       any miss breaks the chain for the following step. */
typedef enum {
    SEQ_STEP_COND_NONE = 0,
    SEQ_STEP_COND_FILL = 1,
    SEQ_STEP_COND_PREV = 2,
    SEQ_STEP_COND_COUNT,
} seq_step_cond_type_t;

/* ── Per-step note transform (OP-Z step-component subset, spec 20 §3.1) ──
 * NONE keeps the step's authored pitch verbatim — the zeroed default, a pure
 * no-op that leaves the step on the plain periodic-tag path. Any non-NONE mode
 * offsets the emitted pitch per fire and therefore forces the step onto the
 * decorated one-shot path (see sequencer_core_step_is_decorated): the plain
 * path emits one repeating tag with a fixed pitch and AMY has no
 * per-repetition hook.
 *   RANDOM    re-pitches every fire by +/- a fixed semitone span.
 *   RAMP_UP   walks the pitch up across successive layer loops, then wraps.
 *   RAMP_DOWN walks it down.
 * The transformed pitch is re-snapped to the active scale/chord unless the
 * step's step_quant_bypass bit is set (spec 20 §3.2), in which case it is
 * emitted chromatically. */
typedef enum {
    SEQ_STEP_TRANSFORM_NONE      = 0,
    SEQ_STEP_TRANSFORM_RANDOM    = 1,
    SEQ_STEP_TRANSFORM_RAMP_UP   = 2,
    SEQ_STEP_TRANSFORM_RAMP_DOWN = 3,
    SEQ_STEP_TRANSFORM_COUNT,
} seq_step_transform_t;

/* ── Filter type constants (mirror AMY's FILTER_* values) ── */
#define SEQ_FILTER_NONE  0
#define SEQ_FILTER_LPF   1
#define SEQ_FILTER_BPF   2
#define SEQ_FILTER_HPF   3
#define SEQ_FILTER_LPF24 4

/* ── Per-voice filter state (stored alongside the ADSR envelope) ──
 * enabled=false means FILTER_NONE is sent (bypass); enabled=true uses filter_type.
 * cutoff_hz is in Hz (the same unit as AMY amy_event.filter_freq_coefs — the driver
 * converts to log-freq internally). */
typedef struct {
    uint8_t filter_type;   /* SEQ_FILTER_* — typically SEQ_FILTER_NONE */
    float   cutoff_hz;     /* 65..8000 Hz */
    float   resonance;     /* 0.51..8.0 (Q factor) */
    bool    enabled;       /* false = bypass (FILTER_NONE sent) */
    float   filter_env_amount; /* EG1 -> cutoff depth in octaves, bipolar -8..+8
                                  (negative = inverted/downward sweep). 0.0
                                  (default, memset-zeroed) = inert: cutoff tracks
                                  COEF_CONST only, exactly as before. Non-zero
                                  routes the row's EG1 through
                                  filter_freq_coefs[COEF_EG1] (same convention as
                                  bass_presets.c and arp_core.c:253). */
} seq_filter_t;

/* ── LFO (per-track tempo-synced software modulator) ── */
typedef enum { LFO_MODE_FREE = 0, LFO_MODE_RETRIG = 1 } lfo_mode_t;
typedef enum {
    LFO_WAVE_SINE = 0, LFO_WAVE_TRIANGLE,
    LFO_WAVE_SAW_UP,   LFO_WAVE_SAW_DOWN,
    LFO_WAVE_SQUARE,   LFO_WAVE_RANDOM,
    LFO_WAVE_COUNT,
} lfo_wave_t;
typedef enum {
    LFO_TARGET_FILTER = 0, LFO_TARGET_AMP,
    LFO_TARGET_PITCH,      LFO_TARGET_PAN,
    LFO_TARGET_SCAN,       /* AMY `duty`: wavetable cycle-scan position when
                               wave=WAVETABLE, pulse width when wave=PULSE */
    LFO_TARGET_COUNT,
} lfo_target_t;
typedef enum {
    LFO_RATE_1_8 = 0, LFO_RATE_1_4,  LFO_RATE_1_2,
    LFO_RATE_1BAR,    LFO_RATE_2BAR, LFO_RATE_4BAR,
    LFO_RATE_COUNT,
} lfo_rate_t;
typedef struct {
    bool         enabled;
    lfo_mode_t   mode;
    lfo_wave_t   wave;
    lfo_rate_t   rate;
    uint8_t      depth;    /* 0..100 % */
    lfo_target_t target;
} seq_lfo_t;

/* ── ADSR envelope (one AMY EG0 breakpoint set) ──
 * Stored as concrete ms/percent so it survives patch changes and can be edited
 * at runtime by the graph UI. Currently scoped PER ROW (per track) inside
 * seq_layer_t's voice_params_t block. To extend to per-step later, widen the
 * storage and update the single accessor seq_layer_env() — no other call site
 * changes. */
typedef struct {
    uint32_t attack_ms;
    uint32_t decay_ms;
    uint8_t  sustain_pct;   /* 0..100, sustain level as a percentage */
    uint32_t release_ms;
    uint8_t  eg_type;       /* AMY ENVELOPE_* (NORMAL/LINEAR/DX7/TRUE_EXP)   */
} seq_env_t;

/* ── Per-voice parameter block (shared voice-config layer) ──
 * One block embedded by every engine's state: the melodic layers (per track,
 * below), the arp (s_arp), and the drone (s_d). Bundles the runtime-editable
 * env/EG1/filter/LFO with their deferred-authority flags ("patch owns it
 * until the user commits, then our copy wins") and the per-target output
 * trim. Initialise with voice_params_init_defaults() (voice_config.h) — the
 * single place amp_trim gets its non-zero unity default, so a memset-zeroed
 * block never ships a silent voice. */
typedef struct {
    seq_env_t    env;             /* ADSR (EG0)                             */
    seq_env_t    env1;            /* second envelope (EG1)                  */
    seq_filter_t filter;
    seq_lfo_t    lfo;
    bool         env_authored;
    bool         env1_authored;
    bool         filter_authored;
    bool         lfo_authored;
    float        amp_trim;        /* output trim 0..1, unity default        */
} voice_params_t;

/* ── Per-layer data (display + audio shared) ── */
typedef struct {
    seq_layer_type_t type;
    uint8_t  num_steps;                              /* 16 or 32               */
    uint8_t  num_tracks;                             /* = SEQ_TRACKS           */
    bool     grid[SEQ_TRACKS][SEQ_MAX_STEPS];        /* step on/off state      */
    uint8_t  step_note[SEQ_TRACKS][SEQ_MAX_STEPS];   /* per-step MIDI pitch    */
    uint8_t  track_base_note[SEQ_TRACKS];            /* current base note      */
    voice_params_t vp[SEQ_TRACKS];       /* per-row voice parameters: ADSR (EG0),
                                            EG1 (typically routed to the filter by
                                            whichever patch/coef setup targets
                                            COEF_EG1), filter (bypass by default),
                                            LFO — each with its deferred-authority
                                            flag — plus the per-track output trim
                                            folded into step velocity at emit
                                            time. Initialise each row with
                                            voice_params_init_defaults() in
                                            sequencer_core_add_layer.            */
    uint8_t   repeat_rate[SEQ_TRACKS];   /* SEQ_REPEAT_* — fires every N bars */
    bool      mute[SEQ_TRACKS];          /* true = track produces no note-ons */
    bool      solo[SEQ_TRACKS];          /* true = only soloed tracks in this
                                             layer are audible; overrides mute
                                             on the soloed track(s) themselves */
    bool      chord_mode;                /* false = scale quantizer (default) */
    uint8_t   chord_root;                /* chromatic 0-11 (C=0)              */
    chord_type_t chord_type;
    uint8_t   swing_pct;                 /* 0..SEQ_SWING_MAX shuffle: odd 16th
                                            steps are delayed by this % of one
                                            step at emit time. 0 = straight
                                            (default; memset-zeroed in
                                            sequencer_core_add_layer).        */
    uint8_t  synth_id[SEQ_TRACKS];   /* one AMY synth per row (both melodic and
                                        drum layers: each track has its own slot */
    uint16_t patch;                  /* shared timbre across the layer's rows
                                        (melodic). Drums use track_patch[] instead;
                                        `patch` mirrors track_patch[0] for display
                                        fallback only.                          */
    uint16_t track_patch[SEQ_TRACKS];/* per-track timbre (drum layer): each drum
                                        track loads its own AMY patch. Unused by
                                        melodic layers (they share `patch`).     */
    uint16_t track_pcm_preset[SEQ_TRACKS]; /* per-track PCM ROM preset (drum layer,
                                        PCM engine): UI mirror of the core's
                                        selection, refreshed each frame by
                                        seq_view_signature(). Drives the row
                                        label and name banner when drum_pcm.  */
    uint32_t synth_flags;            /* shared flags across the layer's rows  */
    uint8_t  num_voices;             /* per-synth voice count                 */
    uint8_t  step_page;                              /* display page 0|1 (32-step) */

    /* ── Per-step probability / ratchet / conditional trig ──
     * A step with prob==100 && ratchet==1 && cond_type==NONE is "plain" and
     * keeps using the original always-on repeating AMY sequence tag (zero
     * extra cost). Any other combination makes the step "decorated": the
     * periodic tag is left cleared and sequencer_core_service_tick() (called
     * once per AMY sequencer tick) decides per loop-iteration whether/how it
     * fires. MUST be initialised to prob=100, ratchet=1 in
     * sequencer_core_add_layer — memset zeroes them, and 0% probability would
     * silence every step by default. cond_type=0 (NONE) is the correct zeroed
     * default and needs no explicit init. */
    uint8_t  step_prob[SEQ_TRACKS][SEQ_MAX_STEPS];       /* 0..100 %, trigger probability */
    uint8_t  step_ratchet[SEQ_TRACKS][SEQ_MAX_STEPS];    /* 1..SEQ_MAX_RATCHET sub-hits    */
    uint8_t  step_cond_type[SEQ_TRACKS][SEQ_MAX_STEPS];  /* seq_step_cond_type_t           */
    uint8_t  step_cond_param[SEQ_TRACKS][SEQ_MAX_STEPS]; /* FILL: loop divisor 2..8        */
    /* ── Per-step OP-Z note transform (spec 20 §3.1/§3.2) ──
     * Both default to 0 = current behaviour: SEQ_STEP_TRANSFORM_NONE keeps the
     * authored pitch and leaves the step on the plain path; step_quant_bypass=0
     * re-snaps a transformed pitch to the scale. A zeroed layer needs no init. */
    uint8_t  step_transform[SEQ_TRACKS][SEQ_MAX_STEPS];    /* seq_step_transform_t (0=NONE)  */
    uint8_t  step_quant_bypass[SEQ_TRACKS][SEQ_MAX_STEPS]; /* 1 = skip scale snap on transform */

    /* ── Per-step micro-timing / velocity / ratchet taper (patch-06) ──
     * All three default to 0, which is each field's neutral value: 0 nudge =
     * on-grid, 0 velocity offset = unchanged, 0 taper = flat ratchet. The
     * memset(0) in sequencer_core_add_layer therefore yields byte-identical
     * behaviour with NO explicit init — unlike step_prob/step_ratchet, whose
     * neutral values are 100/1 and must be initialised there. */
    int8_t   step_nudge[SEQ_TRACKS][SEQ_MAX_STEPS];        /* signed ticks, +-SEQ_STEP_NUDGE_MAX; plain-step only */
    int8_t   step_velocity_adj[SEQ_TRACKS][SEQ_MAX_STEPS]; /* signed percentage points added to velocity (0=none) */
    int8_t   step_ratchet_taper[SEQ_TRACKS][SEQ_MAX_STEPS];/* %-per-sub-hit velocity decay across a ratchet (0=flat) */
} seq_layer_t;

#ifdef __cplusplus
}
#endif
