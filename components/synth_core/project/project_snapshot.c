/* Project snapshot serializer + loader orchestration.
 *
 * Field order is the format: every ser_ writer and its de_ or parse_ reader
 * counterpart below must walk fields in exactly the same sequence, or every
 * saved project silently corrupts on load. The shared env/filter/lfo
 * sub-block helpers exist so the LAYR per-track voice_params_t blocks and the
 * ARP/DRON sections (which embed the same env/filter/lfo shapes) cannot
 * drift from each other.
 *
 * Two-phase load: parse_* validates and stages everything into heap-allocated
 * scratch (never touching live state); only once every section has parsed
 * clean does the apply phase run. Any parse/validation failure frees the
 * scratch and returns false with zero live-state changes.
 */

#include "project/project_snapshot.h"
#include "project_store.h"
#include "project_tlv.h"
#include "project_fs.h"

#include "sequencer_core.h"
#include "seq_core_config.h"   /* SEQ_SWING_MAX - same-component engine limits */
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "amy_fx.h"
#include "quantizer.h"
#include "voice_config.h"
#include "synth_ui/synth_ui_internal.h"   /* synth_ui_reload_mirror_from_core() */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "seq_clamp.h"
#include <string.h>

static const char *TAG = "project_snapshot";

/* Section tags (u32, ASCII little-endian). */
#define TAG_GLOB 0x424F4C47u
#define TAG_LAYR 0x5259414Cu
#define TAG_ARP  0x20505241u
#define TAG_DRON 0x4E4F5244u
#define TAG_PROG 0x474F5250u

#define PROJECT_SER_BUF_CAP (64 * 1024)

_Static_assert(SEQ_TRACKS == 4 && SEQ_MAX_STEPS == 32,
               "LAYR v1 format assumes 4x32; bump section version");

/* ── Shared env/filter/lfo sub-block codecs ──────────────────────────────
 * Used by voice_params_t (LAYR, per track) and directly by ARP/DRON, so the
 * three engines' persisted shapes can never drift from each other. */

static void ser_env(tlv_writer_t *w, const seq_env_t *e)
{
    tlv_put_u32(w, e->attack_ms);
    tlv_put_u32(w, e->decay_ms);
    tlv_put_u8(w,  e->sustain_pct);
    tlv_put_u32(w, e->release_ms);
    tlv_put_u8(w,  e->eg_type);
}

static bool de_env(tlv_reader_t *r, seq_env_t *e)
{
    if (!tlv_get_u32(r, &e->attack_ms))  return false;
    if (!tlv_get_u32(r, &e->decay_ms))   return false;
    if (!tlv_get_u8(r, &e->sustain_pct)) return false;
    if (e->sustain_pct > 100) e->sustain_pct = 100;
    if (!tlv_get_u32(r, &e->release_ms)) return false;
    if (!tlv_get_u8(r, &e->eg_type))     return false;
    return true;
}

static void ser_filter(tlv_writer_t *w, const seq_filter_t *f)
{
    tlv_put_u8(w,  f->filter_type);
    tlv_put_f32(w, f->cutoff_hz);
    tlv_put_f32(w, f->resonance);
    tlv_put_u8(w,  f->enabled ? 1 : 0);
    tlv_put_f32(w, f->filter_env_amount);
}

/* Reads all 5 fields unconditionally (keeps the reader position correct),
 * then resets the whole sub-block to a safe bypass default if filter_type is
 * out of range instead of trusting a bogus enum downstream. */
static bool de_filter(tlv_reader_t *r, seq_filter_t *f)
{
    uint8_t ft, en;
    float cutoff, resonance, env_amount;
    if (!tlv_get_u8(r, &ft))          return false;
    if (!tlv_get_f32(r, &cutoff))     return false;
    if (!tlv_get_f32(r, &resonance))  return false;
    if (!tlv_get_u8(r, &en))          return false;
    if (!tlv_get_f32(r, &env_amount)) return false;

    if (ft > SEQ_FILTER_LPF24) {
        *f = (seq_filter_t){0};
        return true;
    }
    f->filter_type       = ft;
    f->cutoff_hz         = cutoff;
    f->resonance         = resonance;
    f->enabled           = en != 0;
    f->filter_env_amount = env_amount;
    return true;
}

static void ser_lfo(tlv_writer_t *w, const seq_lfo_t *l)
{
    tlv_put_u8(w, l->enabled ? 1 : 0);
    tlv_put_u8(w, (uint8_t)l->mode);
    tlv_put_u8(w, (uint8_t)l->wave);
    tlv_put_u8(w, (uint8_t)l->rate);
    tlv_put_u8(w, l->depth);
    tlv_put_u8(w, l->targets);   /* section v2+: target-set bitmask (v1: index) */
    tlv_put_u8(w, l->wob_rate);  /* LAYR v5+ / ARP v3+: WOBBLE second-order LFO */
    tlv_put_u8(w, l->wob_depth);
}

/* Same "read everything, then validate" shape as de_filter: an out-of-range
 * enum resets the whole LFO sub-block rather than propagating a bad value.
 * The 6th byte changed meaning: section v1 stored a single target index, v2+
 * stores a target-set bitmask. Migrate index -> bit when reading an old file. */
/* `wobble`: whether this file version carries the two WOBBLE bytes — the
 * threshold differs per containing section (LAYR v5+, ARP v3+), so the caller
 * resolves it rather than this shared codec guessing from `ver`. */
static bool de_lfo(tlv_reader_t *r, seq_lfo_t *l, uint8_t ver, bool wobble)
{
    uint8_t en, mode, wave, rate, depth, tgt;
    uint8_t wrate = 0, wdepth = 0;
    if (!tlv_get_u8(r, &en))     return false;
    if (!tlv_get_u8(r, &mode))   return false;
    if (!tlv_get_u8(r, &wave))   return false;
    if (!tlv_get_u8(r, &rate))   return false;
    if (!tlv_get_u8(r, &depth))  return false;
    if (!tlv_get_u8(r, &tgt))    return false;
    if (wobble) {
        if (!tlv_get_u8(r, &wrate))  return false;
        if (!tlv_get_u8(r, &wdepth)) return false;
    }

    if (mode > LFO_MODE_RETRIG || wave >= LFO_WAVE_COUNT ||
        rate >= LFO_RATE_COUNT) {
        *l = (seq_lfo_t){0};
        return true;
    }
    l->enabled = en != 0;
    l->mode    = (lfo_mode_t)mode;
    l->wave    = (lfo_wave_t)wave;
    l->rate    = (lfo_rate_t)rate;
    l->depth   = (depth > 100) ? 100 : depth;
    l->targets = (ver < 2)
        ? ((tgt < LFO_TARGET_COUNT) ? LFO_TGT_BIT(tgt) : 0u)  /* v1 index */
        : (uint8_t)(tgt & LFO_TGT_ALL);                       /* v2 bitmask */
    l->wob_rate  = (wrate < LFO_RATE_COUNT) ? wrate : 0;
    l->wob_depth = (wdepth > 100) ? 100 : wdepth;
    return true;
}

/* ── Shared voice_params_t codec (per track, LAYR) ───────────────────────── */

static void ser_vp(tlv_writer_t *w, const voice_params_t *vp)
{
    ser_env(w, &vp->env);
    ser_env(w, &vp->env1);
    ser_filter(w, &vp->filter);
    ser_lfo(w, &vp->lfo);
    tlv_put_u8(w, vp->env_authored ? 1 : 0);
    tlv_put_u8(w, vp->env1_authored ? 1 : 0);
    tlv_put_u8(w, vp->filter_authored ? 1 : 0);
    tlv_put_u8(w, vp->lfo_authored ? 1 : 0);
    tlv_put_f32(w, vp->amp_trim);
}

static bool de_vp(tlv_reader_t *r, voice_params_t *vp, uint8_t ver)
{
    voice_params_init_defaults(vp);   /* zeroed baseline + unity amp_trim */
    if (!de_env(r, &vp->env))       return false;
    if (!de_env(r, &vp->env1))      return false;
    if (!de_filter(r, &vp->filter)) return false;
    if (!de_lfo(r, &vp->lfo, ver, ver >= 5))  return false;   /* LAYR: wobble v5+ */
    uint8_t ea, e1a, fa, la;
    if (!tlv_get_u8(r, &ea))  return false;
    if (!tlv_get_u8(r, &e1a)) return false;
    if (!tlv_get_u8(r, &fa))  return false;
    if (!tlv_get_u8(r, &la))  return false;
    vp->env_authored    = ea  != 0;
    vp->env1_authored   = e1a != 0;
    vp->filter_authored = fa  != 0;
    vp->lfo_authored    = la  != 0;
    if (!tlv_get_f32(r, &vp->amp_trim)) return false;
    vp->amp_trim = SEQ_CLAMP_F32(vp->amp_trim, 0.0f, 1.0f);
    return true;
}

/* ── GLOB section ─────────────────────────────────────────────────────────
 * Backward-compat: an older/shorter GLOB body simply runs out of bytes
 * partway through; every field beyond that point keeps the live-state value
 * it was seeded with (tlv_reader_t.err sticks on the first short read, so a
 * failed tlv_get_* here is "no more data", not corruption). */

typedef struct {
    uint16_t bpm;
    float    master_volume;
    bool     quant_enabled;
    uint8_t  quant_root;
    uint8_t  quant_scale;
    seq_drum_engine_t drum_engine;
    fx_state_t fx;
} staged_glob_t;

static void ser_glob(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_GLOB, 1);
    tlv_put_u16(w, sequencer_core_get_bpm());
    tlv_put_f32(w, amy_fx_get_master_volume());
    tlv_put_u8(w, sequencer_core_get_quantizer_enabled() ? 1 : 0);
    tlv_put_u8(w, sequencer_core_get_quantizer_root_note());
    tlv_put_u8(w, sequencer_core_get_quantizer_scale());
    tlv_put_u8(w, (uint8_t)sequencer_core_get_drum_engine());
    tlv_put_i8(w,  s_fx.eq_low_db);
    tlv_put_i8(w,  s_fx.eq_mid_db);
    tlv_put_i8(w,  s_fx.eq_high_db);
    tlv_put_u8(w,  s_fx.echo_level);
    tlv_put_u8(w,  s_fx.chorus_level);
    tlv_put_u8(w,  s_fx.reverb_level);
    tlv_put_i16(w, s_fx.echo_delay_ms);
    tlv_put_i16(w, s_fx.echo_feedback);
    tlv_put_i16(w, s_fx.echo_tone);
    tlv_put_i16(w, s_fx.reverb_liveness);
    tlv_put_i16(w, s_fx.reverb_damping);
    tlv_put_i16(w, s_fx.reverb_xover_hz);
    tlv_put_i16(w, s_fx.chorus_rate);
    tlv_put_i16(w, s_fx.chorus_depth);
    tlv_put_u8(w,  s_fx.presets_alter_global ? 1 : 0);
    tlv_end_section(w, h);
}

static bool parse_glob(tlv_reader_t *b, staged_glob_t *g)
{
    /* Seed from live state so a short/older section leaves the tail as-is. */
    g->bpm           = sequencer_core_get_bpm();
    g->master_volume = amy_fx_get_master_volume();
    g->quant_enabled = sequencer_core_get_quantizer_enabled();
    g->quant_root    = sequencer_core_get_quantizer_root_note();
    g->quant_scale   = sequencer_core_get_quantizer_scale();
    g->drum_engine   = sequencer_core_get_drum_engine();
    g->fx            = s_fx;

    uint8_t v;
    if (!tlv_get_u16(b, &g->bpm))           return true;
    if (!tlv_get_f32(b, &g->master_volume)) return true;
    if (!tlv_get_u8(b, &v))                 return true;
    g->quant_enabled = v != 0;
    if (!tlv_get_u8(b, &g->quant_root))     return true;
    if (!tlv_get_u8(b, &g->quant_scale))    return true;
    if (!tlv_get_u8(b, &v))                 return true;
    g->drum_engine = (v == SEQ_DRUM_PCM) ? SEQ_DRUM_PCM : SEQ_DRUM_SYNTH;
    if (!tlv_get_i8(b, &g->fx.eq_low_db))       return true;
    if (!tlv_get_i8(b, &g->fx.eq_mid_db))       return true;
    if (!tlv_get_i8(b, &g->fx.eq_high_db))      return true;
    if (!tlv_get_u8(b, &g->fx.echo_level))      return true;
    if (!tlv_get_u8(b, &g->fx.chorus_level))    return true;
    if (!tlv_get_u8(b, &g->fx.reverb_level))    return true;
    if (!tlv_get_i16(b, &g->fx.echo_delay_ms))  return true;
    if (!tlv_get_i16(b, &g->fx.echo_feedback))  return true;
    if (!tlv_get_i16(b, &g->fx.echo_tone))      return true;
    if (!tlv_get_i16(b, &g->fx.reverb_liveness))return true;
    if (!tlv_get_i16(b, &g->fx.reverb_damping)) return true;
    if (!tlv_get_i16(b, &g->fx.reverb_xover_hz))return true;
    if (!tlv_get_i16(b, &g->fx.chorus_rate))    return true;
    if (!tlv_get_i16(b, &g->fx.chorus_depth))   return true;
    if (!tlv_get_u8(b, &v))                     return true;
    g->fx.presets_alter_global = v != 0;
    return true;
}

static void apply_glob(const staged_glob_t *g)
{
    sequencer_core_set_bpm(g->bpm);
    amy_fx_set_master_volume(g->master_volume);
    sequencer_core_set_quantizer_enabled(g->quant_enabled);
    sequencer_core_set_quantizer_root_note(g->quant_root);
    sequencer_core_set_quantizer_scale(g->quant_scale);
    sequencer_core_set_drum_engine(g->drum_engine);
    s_fx = g->fx;
    fx_push_eq();
    fx_push_echo();
    fx_push_chorus();
    fx_push_reverb();
}

/* ── LAYR section ─────────────────────────────────────────────────────────
 * Strict: any truncated read rejects the whole file (unlike GLOB, a partial
 * LAYR body cannot be safely defaulted field-by-field once step arrays are
 * involved). Per-value clamps normalize out-of-range data instead of
 * rejecting, except for the layer-count/type/first-layer-is-drum invariants
 * enforced by the caller after every LAYR section has parsed. */

static void ser_layer(tlv_writer_t *w, const seq_layer_t *L)
{
    size_t h = tlv_begin_section(w, TAG_LAYR, 5);  /* v2: LFO target bitmask;
                                                    * v3: +gate_pct, +portamento_ms;
                                                    * v4: +groove_pct;
                                                    * v5: LFO +wob_rate/+wob_depth */
    tlv_put_u8(w, (uint8_t)L->type);
    tlv_put_u8(w, L->num_steps);
    tlv_put_u16(w, L->patch);
    tlv_put_u32(w, L->synth_flags);
    tlv_put_u8(w, L->num_voices);
    tlv_put_u8(w, L->chord_mode ? 1 : 0);
    tlv_put_u8(w, L->chord_root);
    tlv_put_u8(w, (uint8_t)L->chord_type);
    tlv_put_u8(w, L->swing_pct);
    for (int t = 0; t < SEQ_TRACKS; t++) {
        tlv_put_u8(w, L->track_base_note[t]);
        tlv_put_u16(w, L->track_patch[t]);
        tlv_put_u16(w, L->track_pcm_preset[t]);
        tlv_put_u8(w, L->repeat_rate[t]);
        tlv_put_u8(w, L->mute[t] ? 1 : 0);
        tlv_put_u8(w, L->solo[t] ? 1 : 0);
        ser_vp(w, &L->vp[t]);
    }
    tlv_put_bytes(w, L->grid,               sizeof L->grid);
    tlv_put_bytes(w, L->step_note,          sizeof L->step_note);
    tlv_put_bytes(w, L->step_prob,          sizeof L->step_prob);
    tlv_put_bytes(w, L->step_ratchet,       sizeof L->step_ratchet);
    tlv_put_bytes(w, L->step_cond_type,     sizeof L->step_cond_type);
    tlv_put_bytes(w, L->step_cond_param,    sizeof L->step_cond_param);
    tlv_put_bytes(w, L->step_transform,     sizeof L->step_transform);
    tlv_put_bytes(w, L->step_quant_bypass,  sizeof L->step_quant_bypass);
    tlv_put_bytes(w, L->step_nudge,         sizeof L->step_nudge);
    tlv_put_bytes(w, L->step_velocity_adj,  sizeof L->step_velocity_adj);
    tlv_put_bytes(w, L->step_ratchet_taper, sizeof L->step_ratchet_taper);
    /* v3: per-layer melodic NoteFX (gate length + glide). Appended at the tail so
     * v2 readers stop cleanly before them and v2 files parse under the ver<3 path.*/
    tlv_put_u8(w, L->gate_pct);
    tlv_put_u16(w, L->portamento_ms);
    /* v4: NoteFX GROOVE (accent-curve amount), same tail-append pattern. */
    tlv_put_u8(w, L->groove_pct);
    tlv_end_section(w, h);
}

static uint16_t clamp_patch(uint16_t patch)
{
    if (patch > SEQ_PATCH_FULL_MAX) return 0;
    if (sequencer_core_patch_compiled_out(patch)) return 0;
    return patch;
}

static bool parse_layer(tlv_reader_t *b, seq_layer_t *L, uint8_t ver)
{
    memset(L, 0, sizeof *L);

    uint8_t type_raw;
    if (!tlv_get_u8(b, &type_raw)) return false;
    L->type = (type_raw > SEQ_LAYER_MELODIC) ? SEQ_LAYER_MELODIC
                                              : (seq_layer_type_t)type_raw;

    if (!tlv_get_u8(b, &L->num_steps)) return false;
    if (L->num_steps != 16 && L->num_steps != 32) L->num_steps = 16;

    if (!tlv_get_u16(b, &L->patch)) return false;
    L->patch = clamp_patch(L->patch);

    if (!tlv_get_u32(b, &L->synth_flags)) return false;
    if (!tlv_get_u8(b, &L->num_voices))   return false;
    /* AMY's instrument_init() aborts outside 1..MAX_VOICES_PER_INSTRUMENT
     * (32); a CRC-valid but hand-edited file must not crash the load. */
    L->num_voices = SEQ_CLAMP_U8(L->num_voices, 1, 32);
    { uint8_t v; if (!tlv_get_u8(b, &v)) return false; L->chord_mode = v != 0; }
    if (!tlv_get_u8(b, &L->chord_root)) return false;
    { uint8_t v; if (!tlv_get_u8(b, &v)) return false;
      L->chord_type = (v >= CHORD_TYPE_COUNT) ? CHORD_MAJ : (chord_type_t)v; }
    if (!tlv_get_u8(b, &L->swing_pct)) return false;
    /* Bulk import bypasses sequencer_core_set_layer_swing(), so the engine
     * ceiling must be enforced here: the swing-offset math requires the
     * delay to stay short of one full step. */
    if (L->swing_pct > SEQ_SWING_MAX) L->swing_pct = SEQ_SWING_MAX;

    for (int t = 0; t < SEQ_TRACKS; t++) {
        if (!tlv_get_u8(b, &L->track_base_note[t]))  return false;
        if (!tlv_get_u16(b, &L->track_patch[t]))      return false;
        L->track_patch[t] = clamp_patch(L->track_patch[t]);
        if (!tlv_get_u16(b, &L->track_pcm_preset[t])) return false;
        { uint8_t v; if (!tlv_get_u8(b, &v)) return false; L->repeat_rate[t] = v; }
        { uint8_t v; if (!tlv_get_u8(b, &v)) return false; L->mute[t] = v != 0; }
        { uint8_t v; if (!tlv_get_u8(b, &v)) return false; L->solo[t] = v != 0; }
        if (!de_vp(b, &L->vp[t], ver)) return false;
    }

    if (!tlv_get_bytes(b, L->grid,               sizeof L->grid))               return false;
    if (!tlv_get_bytes(b, L->step_note,          sizeof L->step_note))          return false;
    if (!tlv_get_bytes(b, L->step_prob,          sizeof L->step_prob))          return false;
    if (!tlv_get_bytes(b, L->step_ratchet,       sizeof L->step_ratchet))       return false;
    if (!tlv_get_bytes(b, L->step_cond_type,     sizeof L->step_cond_type))     return false;
    if (!tlv_get_bytes(b, L->step_cond_param,    sizeof L->step_cond_param))    return false;
    if (!tlv_get_bytes(b, L->step_transform,     sizeof L->step_transform))     return false;
    if (!tlv_get_bytes(b, L->step_quant_bypass,  sizeof L->step_quant_bypass))  return false;
    if (!tlv_get_bytes(b, L->step_nudge,         sizeof L->step_nudge))         return false;
    if (!tlv_get_bytes(b, L->step_velocity_adj,  sizeof L->step_velocity_adj))  return false;
    if (!tlv_get_bytes(b, L->step_ratchet_taper, sizeof L->step_ratchet_taper)) return false;

    for (int t = 0; t < SEQ_TRACKS; t++) {
        for (int s = 0; s < SEQ_MAX_STEPS; s++) {
            L->grid[t][s]              = L->grid[t][s] ? 1 : 0;
            L->step_quant_bypass[t][s] = L->step_quant_bypass[t][s] ? 1 : 0;
            if (L->step_prob[t][s] > 100) L->step_prob[t][s] = 100;
            L->step_ratchet[t][s] = SEQ_CLAMP_U8(L->step_ratchet[t][s], 1, SEQ_MAX_RATCHET);
            if (L->step_cond_type[t][s] >= SEQ_STEP_COND_COUNT) L->step_cond_type[t][s] = SEQ_STEP_COND_NONE;
            if (L->step_cond_param[t][s] > 8) L->step_cond_param[t][s] = 8;  /* FILL divisor caps at 8 */
            if (L->step_transform[t][s] >= SEQ_STEP_TRANSFORM_COUNT) L->step_transform[t][s] = SEQ_STEP_TRANSFORM_NONE;
        }
    }

    /* v3: per-layer melodic NoteFX. Pre-v3 files predate the fields — default to
     * the legacy gate (glide off). Clamp to the live control ranges either way. */
    if (ver >= 3) {
        if (!tlv_get_u8(b, &L->gate_pct)) return false;
        if (!tlv_get_u16(b, &L->portamento_ms)) return false;
    } else {
        L->gate_pct      = SEQ_MELODIC_GATE_DEFAULT_PCT;
        L->portamento_ms = 0;
    }
    L->gate_pct = SEQ_CLAMP_U8(L->gate_pct, 10, 100);
    if (L->portamento_ms > SEQ_MELODIC_PORTAMENTO_MAX_MS)
        L->portamento_ms = SEQ_MELODIC_PORTAMENTO_MAX_MS;

    /* v4: NoteFX GROOVE. Pre-v4 files predate the control — default to the full
     * legacy accent curve so old projects keep their feel. */
    if (ver >= 4) {
        if (!tlv_get_u8(b, &L->groove_pct)) return false;
    } else {
        L->groove_pct = 100;
    }
    if (L->groove_pct > 100) L->groove_pct = 100;

    return true;
}

/* ── ARP section ──────────────────────────────────────────────────────────── */

typedef struct {
    bool         enabled;
    arp_source_t source;
    uint16_t     wave;
    uint16_t     patch;
    arp_dir_t    dir;
    uint8_t      octaves;
    arp_rate_t   rate;
    uint8_t      gate_pct;
    uint8_t      scale;
    uint8_t      root;
    uint16_t     portamento_ms;
    float        amp_scale;
    int16_t      slots[ARP_MAX_SLOTS];
    seq_env_t    env, env2;
    seq_filter_t filter;
    seq_lfo_t    lfo;
} staged_arp_t;

static void ser_arp(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_ARP, 3);  /* v2: LFO target is a bitmask;
                                                   * v3: LFO +wob_rate/+wob_depth */
    tlv_put_u8(w, arp_get_enabled() ? 1 : 0);
    tlv_put_u8(w, (uint8_t)arp_get_source());
    tlv_put_u16(w, arp_get_wave());
    tlv_put_u16(w, arp_get_patch());
    tlv_put_u8(w, (uint8_t)arp_get_direction());
    tlv_put_u8(w, arp_get_octaves());
    tlv_put_u8(w, (uint8_t)arp_get_rate());
    tlv_put_u8(w, arp_get_gate_pct());
    tlv_put_u8(w, arp_get_scale());
    tlv_put_u8(w, arp_get_root_note());
    tlv_put_u16(w, arp_get_portamento_ms());
    tlv_put_f32(w, arp_get_amp_scale());
    for (int i = 0; i < ARP_MAX_SLOTS; i++) tlv_put_i16(w, arp_get_slot((uint8_t)i));
    seq_env_t e; arp_get_envelope(&e);   ser_env(w, &e);
    seq_env_t e2; arp_get_envelope2(&e2); ser_env(w, &e2);
    seq_filter_t f; arp_get_filter(&f);  ser_filter(w, &f);
    seq_lfo_t l; arp_get_lfo(&l);        ser_lfo(w, &l);
    tlv_end_section(w, h);
}

static bool parse_arp(tlv_reader_t *b, staged_arp_t *a, uint8_t ver)
{
    uint8_t v;
    if (!tlv_get_u8(b, &v)) return false;
    a->enabled = v != 0;
    if (!tlv_get_u8(b, &v)) return false;
    a->source = (v > ARP_SRC_PATCH) ? ARP_SRC_PATCH : (arp_source_t)v;
    if (!tlv_get_u16(b, &a->wave))  return false;
    if (!tlv_get_u16(b, &a->patch)) return false;
    a->patch = clamp_patch(a->patch);
    if (!tlv_get_u8(b, &v)) return false;
    a->dir = (v >= ARP_DIR_COUNT) ? ARP_UP : (arp_dir_t)v;
    if (!tlv_get_u8(b, &a->octaves)) return false;
    a->octaves = SEQ_CLAMP_U8(a->octaves, 1, ARP_OCT_MAX);
    if (!tlv_get_u8(b, &v)) return false;
    a->rate = (v >= ARP_RATE_COUNT) ? ARP_RATE_1_4 : (arp_rate_t)v;
    if (!tlv_get_u8(b, &a->gate_pct)) return false;
    a->gate_pct = SEQ_CLAMP_U8(a->gate_pct, 10, 100);
    if (!tlv_get_u8(b, &a->scale)) return false;
    if (a->scale >= quantizer_scale_count()) a->scale = 0;
    if (!tlv_get_u8(b, &a->root)) return false;
    if (!tlv_get_u16(b, &a->portamento_ms)) return false;
    if (a->portamento_ms > ARP_PORTAMENTO_MAX_MS) a->portamento_ms = ARP_PORTAMENTO_MAX_MS;
    if (!tlv_get_f32(b, &a->amp_scale)) return false;
    a->amp_scale = SEQ_CLAMP_F32(a->amp_scale, 0.0f, 1.0f);
    for (int i = 0; i < ARP_MAX_SLOTS; i++) {
        if (!tlv_get_i16(b, &a->slots[i])) return false;
        if (a->slots[i] < ARP_REST || a->slots[i] > 127) a->slots[i] = -1;
    }
    if (!de_env(b, &a->env))       return false;
    if (!de_env(b, &a->env2))      return false;
    if (!de_filter(b, &a->filter)) return false;
    if (!de_lfo(b, &a->lfo, ver, ver >= 3))  return false;   /* ARP: wobble v3+ */
    return true;
}

/* source before patch/wave so the reconfigure lands once; enabled LAST. */
static void apply_arp(const staged_arp_t *a)
{
    arp_set_source(a->source);
    arp_set_wave(a->wave);
    arp_set_patch(a->patch);
    arp_set_direction(a->dir);
    arp_set_octaves(a->octaves);
    arp_set_rate(a->rate);
    arp_set_gate_pct(a->gate_pct);
    arp_set_scale(a->scale);
    arp_set_root_note(a->root);
    arp_set_portamento_ms(a->portamento_ms);
    arp_set_amp_scale(a->amp_scale);
    for (int i = 0; i < ARP_MAX_SLOTS; i++) arp_set_slot((uint8_t)i, a->slots[i]);
    arp_set_envelope(&a->env);
    arp_set_envelope2(&a->env2);
    arp_set_filter(&a->filter);
    arp_set_lfo(&a->lfo);
    arp_set_enabled(a->enabled);
}

/* ── DRON section ─────────────────────────────────────────────────────────── */

typedef struct {
    bool            enabled;
    drone_source_t  source;
    uint16_t        wave;
    chord_type_t    chord;
    uint8_t         root;
    uint16_t        patch;
    float           resonance;
    float           amp_peak;
    float           amp_duck;
    float           amp_trim;
    drone_rate_t    rate;
    bool            sub_enabled;
    int8_t          sub_interval;
    float           sweep_lo, sweep_hi;
    uint8_t         sweep_bars;
    float           gate_len;
    uint8_t         swing;
    float           blip;
    drone_pattern_t pattern;
    seq_env_t       env, env2;
} staged_drone_t;

static void ser_drone(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_DRON, 1);
    tlv_put_u8(w, drone_get_enabled() ? 1 : 0);
    tlv_put_u8(w, (uint8_t)drone_get_source());
    tlv_put_u16(w, drone_get_wave());
    tlv_put_u8(w, (uint8_t)drone_get_chord());
    tlv_put_u8(w, drone_get_root_note());
    tlv_put_u16(w, drone_get_patch());
    tlv_put_f32(w, drone_get_resonance());
    tlv_put_f32(w, drone_get_amp_peak());
    tlv_put_f32(w, drone_get_amp_duck());
    tlv_put_f32(w, drone_get_amp_trim());
    tlv_put_u8(w, (uint8_t)drone_get_rate());
    tlv_put_u8(w, drone_get_sub_enabled() ? 1 : 0);
    tlv_put_i8(w, drone_get_sub_interval());
    tlv_put_f32(w, drone_get_sweep_lo());
    tlv_put_f32(w, drone_get_sweep_hi());
    tlv_put_u8(w, drone_get_sweep_bars());
    tlv_put_f32(w, drone_get_gate_len());
    tlv_put_u8(w, drone_get_swing());
    tlv_put_f32(w, drone_get_blip());
    tlv_put_u8(w, (uint8_t)drone_get_pattern());
    seq_env_t e; drone_get_envelope(&e);   ser_env(w, &e);
    seq_env_t e2; drone_get_envelope2(&e2); ser_env(w, &e2);
    tlv_end_section(w, h);
}

static bool parse_drone(tlv_reader_t *b, staged_drone_t *d)
{
    uint8_t v;
    if (!tlv_get_u8(b, &v)) return false;
    d->enabled = v != 0;
    if (!tlv_get_u8(b, &v)) return false;
    d->source = (v > DRONE_SRC_PATCH) ? DRONE_SRC_PATCH : (drone_source_t)v;
    if (!tlv_get_u16(b, &d->wave)) return false;
    if (!tlv_get_u8(b, &v)) return false;
    d->chord = (v >= CHORD_TYPE_COUNT) ? CHORD_MAJ : (chord_type_t)v;
    if (!tlv_get_u8(b, &d->root))  return false;
    if (!tlv_get_u16(b, &d->patch)) return false;
    d->patch = clamp_patch(d->patch);
    if (!tlv_get_f32(b, &d->resonance)) return false;
    if (!tlv_get_f32(b, &d->amp_peak))  return false;
    d->amp_peak = SEQ_CLAMP_F32(d->amp_peak, 0.0f, 1.0f);
    if (!tlv_get_f32(b, &d->amp_duck))  return false;
    d->amp_duck = SEQ_CLAMP_F32(d->amp_duck, 0.0f, 1.0f);
    if (!tlv_get_f32(b, &d->amp_trim))  return false;
    d->amp_trim = SEQ_CLAMP_F32(d->amp_trim, 0.0f, 1.0f);
    if (!tlv_get_u8(b, &v)) return false;
    d->rate = (v >= DRONE_RATE_COUNT) ? DRONE_RATE_1_4 : (drone_rate_t)v;
    { uint8_t se; if (!tlv_get_u8(b, &se)) return false; d->sub_enabled = se != 0; }
    if (!tlv_get_i8(b, &d->sub_interval)) return false;
    if (!tlv_get_f32(b, &d->sweep_lo)) return false;
    if (!tlv_get_f32(b, &d->sweep_hi)) return false;
    if (!tlv_get_u8(b, &d->sweep_bars)) return false;
    if (!tlv_get_f32(b, &d->gate_len)) return false;
    d->gate_len = SEQ_CLAMP_F32(d->gate_len, 0.05f, 0.95f);
    if (!tlv_get_u8(b, &d->swing)) return false;
    if (d->swing > 66) d->swing = 66;   /* mirrors the private SEQ/DRONE_SWING_MAX ceiling */
    if (!tlv_get_f32(b, &d->blip)) return false;
    d->blip = SEQ_CLAMP_F32(d->blip, 0.0f, 1.0f);
    if (!tlv_get_u8(b, &v)) return false;
    d->pattern = (v >= DRONE_PAT_COUNT) ? DRONE_PAT_FULL : (drone_pattern_t)v;
    if (!de_env(b, &d->env))  return false;
    if (!de_env(b, &d->env2)) return false;
    return true;
}

/* enabled LAST; patch is skipped (leaving the live patch) when the value
 * falls in a range the drone's excitation model can't play. */
static void apply_drone(const staged_drone_t *d)
{
    drone_set_source(d->source);
    drone_set_wave(d->wave);
    drone_set_chord(d->chord);
    drone_set_root_note(d->root);
    if (!drone_patch_excluded(d->patch)) drone_set_patch(d->patch);
    drone_set_resonance(d->resonance);
    drone_set_amp_peak(d->amp_peak);
    drone_set_amp_duck(d->amp_duck);
    drone_set_amp_trim(d->amp_trim);
    drone_set_rate(d->rate);
    drone_set_sub_interval(d->sub_interval);
    drone_set_sub_enabled(d->sub_enabled);
    drone_set_sweep_lo(d->sweep_lo);
    drone_set_sweep_hi(d->sweep_hi);
    drone_set_sweep_bars(d->sweep_bars);
    drone_set_gate_len(d->gate_len);
    drone_set_swing(d->swing);
    drone_set_blip(d->blip);
    drone_set_pattern(d->pattern);
    drone_set_envelope(&d->env);
    drone_set_envelope2(&d->env2);
    drone_set_enabled(d->enabled);
}

/* ── PROG section ─────────────────────────────────────────────────────────── */

/* Generous static cap for the staged array; the real ceiling is whatever
 * sequencer_core_progression_get_max() reports at runtime. Entries beyond
 * either bound are still consumed from the stream (to keep the reader
 * position correct) but not stored. */
#define PROJECT_PROG_MAX_ENTRIES 16

typedef struct {
    bool         enabled;
    uint8_t      count;
    uint8_t      root[PROJECT_PROG_MAX_ENTRIES];
    chord_type_t chord_type[PROJECT_PROG_MAX_ENTRIES];
    uint8_t      duration_bars[PROJECT_PROG_MAX_ENTRIES];
} staged_prog_t;

static void ser_prog(tlv_writer_t *w)
{
    size_t h = tlv_begin_section(w, TAG_PROG, 1);
    tlv_put_u8(w, sequencer_core_progression_get_enabled() ? 1 : 0);
    uint8_t count = sequencer_core_progression_get_count();
    tlv_put_u8(w, count);
    for (uint8_t i = 0; i < count; i++) {
        uint8_t root, bars; chord_type_t ct;
        sequencer_core_progression_get_entry(i, &root, &ct, &bars);
        tlv_put_u8(w, root);
        tlv_put_u8(w, (uint8_t)ct);
        tlv_put_u8(w, bars);
    }
    tlv_end_section(w, h);
}

static bool parse_prog(tlv_reader_t *b, staged_prog_t *p)
{
    uint8_t v, raw_count;
    if (!tlv_get_u8(b, &v)) return false;
    p->enabled = v != 0;
    if (!tlv_get_u8(b, &raw_count)) return false;

    uint8_t max_entries = sequencer_core_progression_get_max();
    if (max_entries > PROJECT_PROG_MAX_ENTRIES) max_entries = PROJECT_PROG_MAX_ENTRIES;
    p->count = (raw_count > max_entries) ? max_entries : raw_count;

    for (uint8_t i = 0; i < raw_count; i++) {
        uint8_t root, ctraw, bars;
        if (!tlv_get_u8(b, &root))  return false;
        if (!tlv_get_u8(b, &ctraw)) return false;
        if (!tlv_get_u8(b, &bars))  return false;
        if (i < p->count) {
            p->root[i]          = root;
            p->chord_type[i]    = (ctraw >= CHORD_TYPE_COUNT) ? CHORD_MAJ : (chord_type_t)ctraw;
            p->duration_bars[i] = bars;
        }
    }
    return true;
}

static void apply_prog(const staged_prog_t *p)
{
    sequencer_core_progression_set_count(p->count);
    for (uint8_t i = 0; i < p->count; i++) {
        sequencer_core_progression_set_entry(i, p->root[i], p->chord_type[i], p->duration_bars[i]);
    }
    sequencer_core_progression_set_enabled(p->enabled);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool project_snapshot_save(uint8_t slot, const char *name)
{
    if (!project_fs_ok()) return false;

    uint8_t *buf = heap_caps_malloc(PROJECT_SER_BUF_CAP, MALLOC_CAP_SPIRAM);
    seq_layer_t *scratch = heap_caps_malloc(sizeof(seq_layer_t), MALLOC_CAP_SPIRAM);
    if (!buf || !scratch) {
        free(buf);
        free(scratch);
        ESP_LOGE(TAG, "save slot %u: SPIRAM allocation failed", slot);
        return false;
    }

    tlv_writer_t w;
    tlv_writer_init(&w, buf, PROJECT_SER_BUF_CAP);
    ser_glob(&w);
    uint8_t n = sequencer_core_get_num_layers();
    for (uint8_t i = 0; i < n; i++) {
        if (sequencer_core_export_layer(i, scratch)) ser_layer(&w, scratch);
    }
    ser_arp(&w);
    ser_drone(&w);
    ser_prog(&w);

    bool ok = !w.err && project_store_write(slot, name, buf, w.len);

    free(scratch);
    free(buf);
    ESP_LOGI(TAG, "save slot %u: %s (%u layer(s), %u bytes)",
             slot, ok ? "OK" : "FAILED", n, (unsigned)w.len);
    return ok;
}

bool project_snapshot_load(uint8_t slot)
{
    uint8_t *payload = NULL;
    size_t   len = 0;
    char     name[PROJECT_NAME_LEN];
    if (!project_store_read(slot, &payload, &len, name)) return false;

    seq_layer_t *staged_layers =
        heap_caps_malloc(sizeof(seq_layer_t) * MAX_LAYERS, MALLOC_CAP_SPIRAM);
    if (!staged_layers) {
        free(payload);
        ESP_LOGE(TAG, "load slot %u: SPIRAM allocation failed", slot);
        return false;
    }

    staged_glob_t  staged_glob;  memset(&staged_glob, 0, sizeof staged_glob);
    staged_arp_t   staged_arp;   memset(&staged_arp, 0, sizeof staged_arp);
    staged_drone_t staged_drone; memset(&staged_drone, 0, sizeof staged_drone);
    staged_prog_t  staged_prog;  memset(&staged_prog, 0, sizeof staged_prog);
    uint8_t staged_layer_count = 0;
    bool got_glob = false, got_arp = false, got_drone = false, got_prog = false;

    tlv_reader_t r;
    tlv_reader_init(&r, payload, len);
    bool ok = true;
    uint32_t tag; uint8_t ver; tlv_reader_t body;
    while (ok && tlv_next_section(&r, &tag, &ver, &body)) {
        switch (tag) {
        case TAG_GLOB:
            if (got_glob || ver != 1) { ok = false; break; }
            ok = parse_glob(&body, &staged_glob);
            got_glob = ok;
            break;
        case TAG_LAYR:
            if (ver < 1 || ver > 3 || staged_layer_count >= MAX_LAYERS) { ok = false; break; }
            ok = parse_layer(&body, &staged_layers[staged_layer_count], ver);
            if (ok) staged_layer_count++;
            break;
        case TAG_ARP:
            if (got_arp || ver < 1 || ver > 2) { ok = false; break; }
            ok = parse_arp(&body, &staged_arp, ver);
            got_arp = ok;
            break;
        case TAG_DRON:
            if (got_drone || ver != 1) { ok = false; break; }
            ok = parse_drone(&body, &staged_drone);
            got_drone = ok;
            break;
        case TAG_PROG:
            if (got_prog || ver != 1) { ok = false; break; }
            ok = parse_prog(&body, &staged_prog);
            got_prog = ok;
            break;
        default:
            break;   /* unknown section: ignore (forward-compat) */
        }
    }
    if (r.err) ok = false;   /* truncated/corrupt outer stream */

    if (ok && (!got_glob || staged_layer_count < 1 ||
               staged_layers[0].type != SEQ_LAYER_DRUM)) {
        ok = false;
    }
    /* Exactly one drum layer, and only at index 0: add_layer(DRUM) always
     * binds the fixed SEQ_DRUM_SYNTH_BASE slots, so a second drum layer
     * would alias the first one's AMY synths. */
    for (uint8_t i = 1; ok && i < staged_layer_count; i++) {
        if (staged_layers[i].type == SEQ_LAYER_DRUM) ok = false;
    }

    if (!ok) {
        free(staged_layers);
        free(payload);
        ESP_LOGW(TAG, "load slot %u: validation failed, no changes made", slot);
        return false;
    }

    /* ── Phase 2: apply. Nothing above this point touched live state. ── */
    sequencer_core_set_playing(false);
    arp_core_clear_all();

    while (sequencer_core_get_num_layers() > 1) {
        sequencer_core_delete_layer((uint8_t)(sequencer_core_get_num_layers() - 1));
    }
    for (uint8_t i = 1; i < staged_layer_count; i++) {
        sequencer_core_add_layer(staged_layers[i].type, staged_layers[i].num_steps);
    }
    for (uint8_t i = 0; i < staged_layer_count; i++) {
        sequencer_core_import_layer(i, &staged_layers[i]);
    }

    apply_glob(&staged_glob);
    if (got_arp)   apply_arp(&staged_arp);
    if (got_drone) apply_drone(&staged_drone);
    if (got_prog)  apply_prog(&staged_prog);

    synth_ui_reload_mirror_from_core();

    free(staged_layers);
    free(payload);
    ESP_LOGI(TAG, "load slot %u ('%s') OK: %u layer(s)", slot, name, staged_layer_count);
    return true;
}

#if CONFIG_SYNTH_PROJECT_SELFTEST
void project_snapshot_selftest(void)
{
    /* Never over a real project: the test deletes the slot afterwards. */
    uint8_t slot = CONFIG_SYNTH_PROJECT_MAX_SLOTS - 1;
    project_slot_info_t info;
    if (project_store_slot_info(slot, &info) && info.used) {
        ESP_LOGW(TAG, "SNAPSHOT SELFTEST SKIPPED (slot P%02u in use)", (unsigned)slot);
        return;
    }
    bool pass = project_snapshot_save(slot, "st2");
    pass = pass && project_snapshot_load(slot);
    project_store_delete(slot);
    ESP_LOGI(TAG, "SNAPSHOT SELFTEST %s", pass ? "PASS" : "FAIL");
}
#endif
