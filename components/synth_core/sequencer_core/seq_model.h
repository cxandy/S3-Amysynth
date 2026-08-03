#pragma once

/* ── Sequencer / audio data model ──────────────────────────────────────────
 * Engine-owned shared model for the step sequencer: it lives in the layer that
 * owns and mutates it (sequencer_core), not in a view component. display_seq.h
 * includes this header and keeps only render-view types. Per-step
 * parameter-lock storage widening belongs here too (see the env/filter/lfo
 * notes below). */

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
 * 1 = plain single trigger (default). >1 subdivides the step into that many
 * evenly-spaced hits, each with its own short gate. */
#define SEQ_MAX_RATCHET 4

/* ── Per-step conditional trig ──
 * NONE: fires whenever the step is ON, subject only to probability/ratchet.
 * FILL: fires once every step_cond_param loops of the layer's pattern (param
 *       clamped 2..8; the first loop after play-start always counts).
 * PREV: fires only if the preceding step on the SAME track sounded on its last
 *       evaluation, so any miss breaks the chain. */
typedef enum {
    SEQ_STEP_COND_NONE = 0,
    SEQ_STEP_COND_FILL = 1,
    SEQ_STEP_COND_PREV = 2,
    SEQ_STEP_COND_COUNT,
} seq_step_cond_type_t;

/* ── Per-step note transform ──
 * NONE is the zeroed default: the authored pitch, on the plain periodic-tag
 * path. Any other mode offsets the pitch per fire and so forces the step onto
 * the decorated one-shot path (sequencer_core_step_is_decorated) - the plain
 * path emits one repeating tag at a fixed pitch and AMY has no per-repetition
 * hook.
 *   RANDOM    re-pitches every fire by +/- a fixed semitone span.
 *   RAMP_UP   walks the pitch up across successive layer loops, then wraps.
 *   RAMP_DOWN walks it down.
 * The transformed pitch re-snaps to the active scale/chord unless the step's
 * step_quant_bypass bit is set, in which case it is emitted chromatically. */
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
#define SEQ_FILTER_NOTCH 5
#define SEQ_FILTER_PHASER 6
#define SEQ_FILTER_COUNT 7   /* first invalid value — range guards use this */

/* ── Per-voice filter state (stored alongside the ADSR envelope) ──
 * enabled=false sends FILTER_NONE (bypass). cutoff_hz is in Hz, the same unit
 * as amy_event.filter_freq_coefs; AMY converts to log-freq internally. */
typedef struct {
    uint8_t filter_type;   /* SEQ_FILTER_* */
    float   cutoff_hz;     /* 65..8000 Hz */
    float   resonance;     /* 0.51..8.0 (Q factor) */
    bool    enabled;       /* false = bypass (FILTER_NONE sent) */
    float   filter_env_amount; /* EG1 -> cutoff depth in octaves, bipolar -8..+8
                                  (negative = downward sweep). 0.0 (memset
                                  default) = inert, cutoff tracks COEF_CONST
                                  only; non-zero routes the row's EG1 through
                                  filter_freq_coefs[COEF_EG1], same convention
                                  as bass_presets.c and arp_core.c. */
    float   feedback;      /* KS string decay, 0..1 (1.0 = lossless infinite
                              sustain; above 1 the KS buffer diverges). Only
                              meaningful on feedback waves, where the filter
                              editor exposes a dedicated FB cursor alongside the
                              still-editable biquad resonance. 0.0 (memset
                              default) = never authored, so apply paths leave
                              AMY's build-time 0.9 default untouched. */
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
    /* APPEND-ONLY past this point: values are persisted in project snapshots.
     * Mirrors the arp's rate range; the fast end is frequency-capped in
     * lfo_rate_to_hz / the software stepper so a tempo-synced LFO can never
     * reach an audible rate. */
    LFO_RATE_1_16,    LFO_RATE_1_32,
    LFO_RATE_1_4T,    LFO_RATE_1_8T, LFO_RATE_1_16T, LFO_RATE_1_32T,
    LFO_RATE_COUNT,
} lfo_rate_t;
typedef enum {
    /* WOBBLE reach: which carrier rails the second-order LFO drives. One
     * persisted byte; 0 keeps the historic depth+rate meaning so zero-init
     * structs and pre-reach snapshot files keep their sound. APPEND-ONLY. */
    WOB_REACH_BOTH  = 0,   /* depth + rate (historic default)          */
    WOB_REACH_DEPTH = 1,   /* depth only (was wob_depth_only = true)   */
    WOB_REACH_RATE  = 2,   /* rate only - no depth breathing           */
    WOB_REACH_COUNT,
} wob_reach_t;
typedef struct {
    bool         enabled;
    lfo_mode_t   mode;
    lfo_wave_t   wave;
    lfo_rate_t   rate;
    uint8_t      depth;    /* 0..100 %, shared across all active targets      */
    uint8_t      targets;  /* bitmask of (1 << lfo_target_t): one AMY mod
                              carrier drives every checked target - osc1's
                              amplitude is the shared depth, each target's
                              COEF_MOD scaled by its own constant.            */
    uint8_t      wob_rate; /* lfo_rate_t of the WOBBLE (second-order) LFO:
                              osc2 modulating the osc1 carrier's depth AND
                              rate via chained mod_source.                    */
    uint8_t      wob_depth;/* 0..100 % of VOICE_WOB_DEPTH_AMP; 0 (zero-init) =
                              wobble off, osc2 dormant. The UI authors this in
                              whole dB of dip below the authored LFO depth
                              (see voice_config.h); the stored unit is
                              unchanged so project snapshots stay compatible. */
    uint8_t      wob_reach;/* wob_reach_t. Persisted as the former
                              wob_depth_only byte: 0 both, 1 depth-only,
                              2 rate-only; older firmware reads 2 as nonzero
                              = depth-only, a benign degrade.                 */
    uint8_t flt_oct_q;     /* FILTER-target swing in quarter-octaves (1..16 =
                              +/-0.25..4.0 oct), independent of the shared
                              depth % - octaves are what the ear and AMY's log2
                              filter rail work in. 0 = legacy sentinel: derive
                              depth% x VOICE_LFO_DEPTH_FILTER so older files
                              keep their sound. Never read raw - resolve via
                              voice_lfo_filter_octaves().                     */
} seq_lfo_t;

/* Target-set helpers: the single mod-source oscillator can feed any subset of
 * targets at once (AMY sums the COEF_MOD contributions), so the target is a
 * set, not a scalar. */
#define LFO_TGT_BIT(t)     ((uint8_t)(1u << (t)))
#define LFO_TGT_ALL        ((uint8_t)((1u << LFO_TARGET_COUNT) - 1u))
#define LFO_HAS_TGT(l, t)  (((l)->targets & LFO_TGT_BIT(t)) != 0)

/* ── ADSR envelope (one AMY EG0 breakpoint set) ──
 * Stored as concrete ms/percent so it survives patch changes and stays
 * runtime-editable. Scoped PER ROW inside seq_layer_t's voice_params_t block;
 * extending to per-step means widening the storage and updating the single
 * accessor seq_layer_env(), with no other call-site changes. */
typedef struct {
    uint32_t attack_ms;
    uint32_t decay_ms;
    uint8_t  sustain_pct;   /* 0..100, sustain level as a percentage */
    uint32_t release_ms;
    uint8_t  eg_type;       /* AMY ENVELOPE_* (NORMAL/LINEAR/DX7/TRUE_EXP)   */
} seq_env_t;

/* ── Per-voice parameter block (shared voice-config layer) ──
 * Embedded by every engine's state: melodic layers (per track), the arp and the
 * drone. Bundles the runtime-editable env/EG1/filter/LFO with their
 * deferred-authority flags - the patch owns a parameter until the user commits,
 * then our copy wins - plus the output trim. ALWAYS initialise with
 * voice_params_init_defaults() (voice_config.h): it is the single place
 * amp_trim gets its unity default, so a memset-zeroed block is a silent
 * voice. */
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
    voice_params_t vp[SEQ_TRACKS];       /* per-row voice parameters (EG0, EG1,
                                            filter, LFO, authored flags, output
                                            trim). Initialise each row with
                                            voice_params_init_defaults() in
                                            sequencer_core_add_layer.            */
    uint8_t   repeat_rate[SEQ_TRACKS];   /* SEQ_REPEAT_*: fires every N bars   */
    bool      mute[SEQ_TRACKS];          /* true = track produces no note-ons */
    bool      solo[SEQ_TRACKS];          /* true = only soloed tracks in this
                                             layer are audible; overrides mute
                                             on the soloed track(s) themselves */
    bool      chord_mode;                /* false = scale quantizer (default) */
    uint8_t   chord_root;                /* chromatic 0-11 (C=0)              */
    chord_type_t chord_type;
    uint8_t   swing_pct;                 /* 0..SEQ_SWING_MAX: odd 16th steps are
                                            delayed by this % of one step at
                                            emit time. 0 = straight (memset
                                            default).                          */
    uint8_t   gate_pct;                  /* melodic note-hold as % of the step
                                            (10..100), the NoteFX GATE control;
                                            drum layers use SEQ_GATE_DRUM
                                            instead. MUST be initialised to
                                            SEQ_MELODIC_GATE_DEFAULT_PCT in
                                            add_layer - memset 0 silences every
                                            melodic note.                      */
    uint16_t  portamento_ms;             /* glide between step pitches (0..100
                                            ms, NoteFX Glide), 0 = off. AMY
                                            portamento_alpha; re-pushed on every
                                            voice rebuild since osc reset
                                            clears it.                         */
    uint8_t   groove_pct;                /* NoteFX GROOVE: how much of the
                                            accent/humanize velocity curve
                                            applies (0..100, 0 = flat 1.0),
                                            scaled at emit time in
                                            sequencer_step_velocity(). MUST be
                                            initialised to 100 in add_layer -
                                            memset 0 flattens dynamics.        */
    uint8_t  synth_id[SEQ_TRACKS];   /* one AMY synth per row, melodic and drum
                                        alike                                   */
    uint16_t patch;                  /* melodic: timbre shared by the layer's
                                        rows. Drums use track_patch[]; `patch`
                                        mirrors track_patch[0] as a display
                                        fallback only.                          */
    uint16_t track_patch[SEQ_TRACKS];/* drum layer: per-track timbre. Unused by
                                        melodic layers.                         */
    uint16_t track_pcm_preset[SEQ_TRACKS]; /* drum layer, PCM engine: UI mirror
                                        of the core's per-track selection,
                                        refreshed each frame by
                                        seq_view_signature(). Drives the row
                                        label and name banner.                  */
    uint32_t synth_flags;            /* shared flags across the layer's rows  */
    uint8_t  num_voices;             /* per-synth voice count                 */
    uint8_t  step_page;                              /* display page 0|1 (32-step) */

    /* ── Per-step probability / ratchet / conditional trig ──
     * A step with prob==100 && ratchet==1 && cond_type==NONE is "plain" and
     * keeps the always-on repeating AMY sequence tag at no extra cost. Any
     * other combination makes it "decorated": the periodic tag is left cleared
     * and sequencer_core_service_tick() decides per loop-iteration whether and
     * how it fires. MUST be initialised to prob=100, ratchet=1 in
     * sequencer_core_add_layer - memset's 0% probability would silence every
     * step. cond_type=0 (NONE) is the correct zeroed default. */
    uint8_t  step_prob[SEQ_TRACKS][SEQ_MAX_STEPS];       /* 0..100 %, trigger probability */
    uint8_t  step_ratchet[SEQ_TRACKS][SEQ_MAX_STEPS];    /* 1..SEQ_MAX_RATCHET sub-hits    */
    uint8_t  step_cond_type[SEQ_TRACKS][SEQ_MAX_STEPS];  /* seq_step_cond_type_t           */
    uint8_t  step_cond_param[SEQ_TRACKS][SEQ_MAX_STEPS]; /* FILL: loop divisor 2..8        */
    /* ── Per-step note transform ──
     * Both are neutral at 0: TRANSFORM_NONE keeps the authored pitch on the
     * plain path, and quant_bypass=0 re-snaps a transformed pitch to the scale.
     * A zeroed layer needs no init. */
    uint8_t  step_transform[SEQ_TRACKS][SEQ_MAX_STEPS];    /* seq_step_transform_t (0=NONE)  */
    uint8_t  step_quant_bypass[SEQ_TRACKS][SEQ_MAX_STEPS]; /* 1 = skip scale snap on transform */

    /* ── Per-step micro-timing / velocity / ratchet taper ──
     * 0 is each field's neutral value (on-grid, unchanged velocity, flat
     * ratchet), so add_layer's memset suffices - unlike step_prob/step_ratchet,
     * whose neutral values are 100/1 and must be initialised explicitly. */
    int8_t   step_nudge[SEQ_TRACKS][SEQ_MAX_STEPS];        /* signed ticks, +-SEQ_STEP_NUDGE_MAX; plain-step only */
    int8_t   step_velocity_adj[SEQ_TRACKS][SEQ_MAX_STEPS]; /* signed percentage points added to velocity (0=none) */
    int8_t   step_ratchet_taper[SEQ_TRACKS][SEQ_MAX_STEPS];/* %-per-sub-hit velocity decay across a ratchet (0=flat) */
} seq_layer_t;

#ifdef __cplusplus
}
#endif
