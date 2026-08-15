#pragma once

/* Shared voice-config layer: leaf utilities, canonical constants, the WAVE
 * voice skeleton and the native-LFO topology appliers used by every engine. */

#include "seq_model.h"     /* seq_env_t, lfo_wave_t */
#include <stdbool.h>
#include <stdint.h>

/* Canonical LFO-target depth scalars - single source of truth. Each is the
 * per-domain exchange rate for the SHARED depth%: anchored so equal depth
 * reads as equal musical intensity across targets. Applies to the native
 * (COEF_MOD) path AND the software-stepper pushes (sequencer/arp/live),
 * which must multiply the same scalar inside their powf(2, ...) term. */
#define VOICE_LFO_DEPTH_FILTER 3.0f
#define VOICE_LFO_DEPTH_AMP    0.5f
/* Pitch is log2: 1.0 = a full octave at 100% depth, which made even 5%
 * (+-60 cents) heavy vibrato. One semitone at 100% puts the whole musical
 * vibrato range on the knob - depth% ~= 1.2 cents/%. Octave-scale sweeps
 * are deliberately out of this LFO's reach (per-target field if ever
 * wanted, like the filter's octave-denominated swing). */
#define VOICE_LFO_DEPTH_PITCH  (1.0f / 12.0f)
#define VOICE_LFO_DEPTH_SCAN   0.5f
#define VOICE_LFO_DEPTH_PAN    0.5f  /* swing around the 0.5 center baseline */

/* Anchor for software-LFO pitch pushes (every software stepper: sequencer
 * layers, arp, live voice). AMY's freq COEF_CONST is an absolute frequency in
 * Hz, converted via logfreq_of_freq(x) = log2(x / 440) (amy.c,
 * ZERO_LOGFREQ_IN_HZ). Multiplying the desired ratio by this base makes the
 * resulting logfreq constant the pure octave offset - 440 Hz itself is the
 * neutral push, identical to the reset default of 0. */
#define SEQ_LFO_PITCH_BASE_HZ 440.0f

/* FILTER-target swing ceiling, in quarter-octave steps (16 = 4.0 octaves). */
#define VOICE_LFO_FLT_OCT_Q_MAX 16u

/* Effective FILTER swing in octaves. AMY's filter_freq COEF_MOD rail is
 * log2-frequency, so this value IS the native coefficient; the software
 * stepper uses it as the 2^x exponent scale. Resolves the flt_oct_q legacy
 * sentinel (0 = depth% x VOICE_LFO_DEPTH_FILTER - the pre-octave law). */
static inline float voice_lfo_filter_octaves(const seq_lfo_t *l)
{
    uint8_t q = l->flt_oct_q;
    if (q == 0) return ((float)l->depth / 100.0f) * VOICE_LFO_DEPTH_FILTER;
    if (q > VOICE_LFO_FLT_OCT_Q_MAX) q = VOICE_LFO_FLT_OCT_Q_MAX;
    return 0.25f * (float)q;
}

/* WOBBLE (second-order LFO): osc2 chained as the osc1 carrier's mod_source.
 * AMP rides the dB combine (contribution is linear in the exponent: amp
 * multiplier = 10^(3 * coef * wob)) and is applied DOWNWARD-ONLY: the
 * carrier's amp CONST is pre-dropped by the swing (10^(-3*coef)) so the
 * breathing peaks exactly at the authored LFO depth and dips below it, never
 * above. Untamed, the +half of the swing multiplies the effective LFO depth
 * through the unclamped exponential MOD rail (2.8x at the former 0.15 coef)
 * and overdrives every target - the "too loud" wobble finding, 2026-08-03.
 * RATE is linear in log2-frequency: 1.0 => +/-1 octave rate swing. */
#define VOICE_WOB_DEPTH_AMP    0.075f
#define VOICE_WOB_DEPTH_RATE   1.0f

/* ── WOBBLE authoring unit ───────────────────────────────────────────────
 * Stored wob_depth is a 0..100 percentage of VOICE_WOB_DEPTH_AMP - meaningless
 * to the user, so authoring and display speak whole dB of TOTAL DIP below the
 * authored depth: the LFO breathes from full authored depth down to -N dB and
 * back (downward-only, see above).
 *
 * The AMP coefficient enters AMY's dB combine linearly (amp = 10^(3*coef*mod),
 * amp_combine_controls) and the carrier CONST is dropped by the same swing, so
 * total dip = 120 * coef dB with coef = wob/100 * VOICE_WOB_DEPTH_AMP: full
 * scale is 120 * VOICE_WOB_DEPTH_AMP = 9 dB in 1 dB steps.
 *
 * 0 dB is OFF, not a distinct setting: at zero swing the modulator is parked
 * (osc2 silenced), so nothing exists between "off" and "1 dB". The stored byte
 * keeps its 0..100 meaning, so old snapshots load unchanged.
 *
 * The same control also swings carrier RATE by up to +/-1 octave
 * (VOICE_WOB_DEPTH_RATE) when the reach includes rate; rate-only reach has no
 * depth breathing to measure, so its readout speaks octaves instead of dB. */
#define VOICE_WOB_DB_MAX  9u   /* = 120 * VOICE_WOB_DEPTH_AMP; keep in step */

/* Stored 0..100 -> whole dB of swing, rounded (0 => OFF). */
uint8_t voice_wob_depth_to_db(uint8_t wob_depth);

/* Whole dB of swing -> stored 0..100. Round-trips with the above at every
 * step, so repeated editing never drifts. */
uint8_t voice_wob_db_to_depth(uint8_t db);

/* ── Note-triggered envelope bounds ──────────────────────────────────────
 * The melodic sequencer, arp and graph editor all drive the same seq_env_t
 * into AMY breakpoint sets and need the same guard rails. Clamp against these
 * names, not respelled numbers, so UI/store/push path cannot drift apart.
 *
 * Scope: NOTE-TRIGGERED voices only. The drone engines keep their own floors -
 * their envelopes are free-running and open over hundreds of ms, so a shared
 * declick policy would encode a constraint they do not have.
 *
 * The graph editor derives its minimum marker spacing from ATTACK_MIN_MS (via
 * graph_popup_set_min_x_gap()) so the plot cannot enforce a stricter floor
 * than the engine.
 */

/* Below roughly one render block (5.33 ms at 48 kHz) an attack is
 * indistinguishable from a step, which clicks on percussive material. 2 ms
 * keeps the ramp inside a single block while still counting as a ramp. */
#define VOICE_ENV_ATTACK_MIN_MS   2u

/* Longer than the attack floor: a note-off lands on whatever the sustain stage
 * held, so the tail needs a few blocks to reach zero without a tick. */
#define VOICE_ENV_RELEASE_MIN_MS  5u

/* Ceiling for any single envelope segment. Far past the graph editor's longest
 * axis (15 s); exists to reject restored garbage, not to constrain authoring. */
#define VOICE_ENV_TIME_MAX_MS     60000u

/* Sustain is stored as a percentage of peak. */
#define VOICE_ENV_SUSTAIN_MAX_PCT 100u

/* Reset a voice_params_t (seq_model.h) to defaults: zeroed/unauthored, with
 * amp_trim at unity. The single place the trim gets its non-zero default -
 * use it instead of re-setting 1.0 after a memset. */
void voice_params_init_defaults(voice_params_t *vp);

/* lfo_wave_t -> AMY wave constant. RANDOM maps to NOISE: compute_mod_noise
 * is a native sample-and-hold (redraws only when the carrier phasor wraps),
 * replacing the 20 Hz software lfo_next_rand() poll. */
uint16_t voice_lfo_wave_to_amy(lfo_wave_t wave);

/* ── Shared WAVE-voice skeleton ──────────────────────────────────────────
 * The canonical "N voices, osc0 = note-following carrier" build used by the
 * arp, drone and melodic sequencer. Sends two events: pool definition and osc0
 * skeleton. Deliberately does NOT touch osc1, envelopes, filters or mod
 * routing - each engine layers its own specialization on top as deltas.
 *
 * AMY does not reset the osc pool when the same num_voices/oscs_per_voice is
 * re-sent, so rebuilding never glitches held voices; that same property is why
 * per-target COEF_MOD state must be cleared explicitly on reconfigure
 * (voice_apply_native_lfo). */
typedef struct {
    uint8_t  synth;
    uint8_t  num_voices;
    uint8_t  oscs_per_voice;       /* 2 when osc1 is reserved as a carrier */
    uint16_t wave;                 /* AMY wave constant for osc0           */
    float    osc0_amp_const;       /* arp/melodic 1.0; drone const_sent    */
    float    osc0_amp_vel;         /* arp/melodic 1.0; drone 0.0           */
    bool     ks_feedback_authored; /* authored feedback drives KS decay    */
    float    ks_feedback;          /* seq_filter_t.feedback (0..1) when authored;
                                      <= 0 or unauthored -> fixed 0.9 default */
    int16_t  wt_preset;            /* >=0 => e->preset (WAVETABLE); else -1 */
} voice_wave_cfg_t;

/* Core-0 / UI-task only; pushes through amy_helpers (never amy_queue_lock). */
void voice_build_wave(const voice_wave_cfg_t *cfg);

/* ── WAVE-voice distortion (DEV-menu controlled, one set per domain) ──────
 * AMY per-osc distortion (DIST_*) on osc0 of WAVE-mode targets. The owning
 * domain is derived from the synth slot, never passed by callers.
 * voice_build_wave() re-applies the owning set on every rebuild; live edits
 * additionally push to already-built targets (ui_screen_dev.c owns that
 * fan-out). PATCH-mode targets and non-WAVE slots never receive these. */
typedef enum {
    VOICE_DIST_ARP = 0,     /* SEQ_ARP_SYNTH                                 */
    VOICE_DIST_DRONE,       /* both drones: DRONE_SYNTH_* + DRONE_STD_SYNTH_* */
    VOICE_DIST_MELODIC,     /* every melodic slot (>= SEQ_MEL_SYNTH_BASE)    */
    VOICE_DIST_DOMAIN_COUNT
} voice_dist_domain_t;

typedef struct {
    uint8_t type;   /* AMY DIST_* value: 0 OFF, 1 CLIP, 2 FOLD, 3 CRUSH */
    uint8_t drive;  /* 1..16 pre-gain (fold depth for FOLD) */
    uint8_t bits;   /* 1..16 CRUSH bit depth */
    uint8_t rate;   /* 1..64 CRUSH sample-hold length in samples */
    uint8_t mix;    /* 0..100 wet % */
} voice_dist_t;

/* Which set governs a slot; VOICE_DIST_DOMAIN_COUNT for slots the stage
 * does not reach (0, drums, live play). */
voice_dist_domain_t voice_dist_domain_of(uint8_t synth);

/* Current set for a domain. Never NULL (out-of-range yields the ARP set). */
const voice_dist_t *voice_dist_get(voice_dist_domain_t domain);

/* Clamp to the documented ranges and store. Does NOT push (see
 * voice_apply_dist). Out-of-range domain: no-op. */
void voice_dist_set(voice_dist_domain_t domain, const voice_dist_t *d);

/* Push the set owning this slot to its voices (osc0); type OFF actively
 * disables. Slots outside every domain: no-op. Core-0 / UI-task only. */
void voice_apply_dist(uint8_t synth);

/* ── Lazy LFO-sibling materialization ────────────────────────────────────
 * voice_build_wave() reserves the osc1 (LFO carrier) and osc2 (wobble) INDEX
 * slots in the pool shape, but AMY allocates an osc's ~532 B struct only when
 * an event first addresses it. voice_config tracks per synth whether the
 * siblings were ever materialized, so the disabled-path park events - which
 * would themselves BE the allocation - are skipped while the oscs are provably
 * fresh (NULL or AMY-reset: silent, no mod coupling).
 *
 * Failure modes are asymmetric by design: a wrongly-SET state costs only the
 * savings, while a wrongly-CLEAR state skips parking a live carrier and
 * resurrects the stale-COEF_MOD DC rail. So the state clears ONLY on a
 * provable pool reset inside voice_build_wave, and every path configuring a
 * wave-built synth with a foreign topology (patch string, bass/FM/additive
 * preset, PCM pool) must call voice_lfo_mark_foreign() first. */

/* True when this synth's LFO siblings (osc1/osc2) may exist or carry
 * state, i.e. park events must be sent. Callers that pre-park osc1
 * outside voice_apply_native_lfo (the melodic dormant slot) gate on it. */
bool voice_lfo_siblings_materialized(uint8_t synth);

/* This synth was configured outside voice_build_wave. Foreign topologies
 * can leave live oscs at the sibling indices and stale-date the shape
 * cache, so force park-always until a future build proves a pool reset. */
void voice_lfo_mark_foreign(uint8_t synth);

/* Register a pool build that reserves a trailing LFO carrier pair but is
 * issued outside voice_build_wave (custom builders - the bass presets). Same
 * shape-proof rules: a known different shape clears the materialized state,
 * anything ambiguous keeps it. Such a builder must NOT also be routed through
 * voice_lfo_mark_foreign. */
void voice_lfo_note_pool_shape(uint8_t synth, uint8_t num_voices,
                               uint8_t oscs_per_voice);

/* Apply the AMY-native mod-source LFO routing for one synth built by
 * voice_build_wave() with oscs_per_voice=2: osc0 gets mod_source=1 plus the
 * selected target's COEF_MOD depth, osc1 becomes the LFO carrier at the
 * BPM-synced rate. Pass lfo=NULL (or a disabled lfo) to deactivate: the mod
 * coupling is cleared and the carrier silenced.
 *
 * Idempotent and state-clean: every target's COEF_MOD is cleared before the
 * selected one is set, because re-sending the same voice count does not reset
 * the osc pool - a prior target's coef would persist and keep modulating (the
 * stale-AMP case rides AMY's convex dB combine and ramps to a DC rail).
 *
 * Core-0 / UI-task only; pushes through amy_helpers (never amy_queue_lock). */
void voice_apply_native_lfo(uint8_t synth, const seq_lfo_t *lfo, uint16_t bpm);

/* Topology-parameterized applier behind voice_apply_native_lfo (the wave-build
 * special case: carrier 1, mask 0x01). carrier_osc = voice-relative index of
 * the reserved LFO carrier (wobble = carrier+1); coupled_mask selects the
 * audible oscs receiving mod_source + COEF_MOD (bit n = osc n). Derive both
 * from sequencer_core_lfo_native_layout(), never hardcode osc indices. */
void voice_apply_native_lfo_topo(uint8_t synth, const seq_lfo_t *lfo,
                                 uint16_t bpm, uint8_t carrier_osc,
                                 uint8_t coupled_mask);
