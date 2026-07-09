#include "sequencer_core/seq_core_internal.h"

/* ════════════════════════════════════════════════════════════════════════
 *  Per-step probability / ratchet / conditional-trig engine
 * ════════════════════════════════════════════════════════════════════════
 *
 * A step is "plain" when step_prob==100 && step_ratchet==1 &&
 * step_cond_type==NONE: it keeps using sequencer_emit_step()'s original
 * always-on repeating AMY sequence tag (seq_core_engine.c) — zero added cost,
 * zero behaviour change from before this file existed.
 *
 * Any other combination makes the step "decorated". sequencer_emit_step()
 * (seq_core_engine.c) leaves such a step's plain ON/OFF tag pair cleared, and
 * sequencer_core_service_tick() — called once per AMY sequencer tick from the
 * existing 500us sequencer poll hook (main.c) — takes over: it detects the
 * moment a layer's playhead crosses into a new step (O(1) integer compare per
 * layer per tick; the per-track decoration check only runs on that edge, so
 * the steady-state per-tick cost is a handful of layer-count comparisons, not
 * a scan of every step), evaluates the step's conditional trig and
 * probability roll, and — if it should sound — one-shot schedules
 * step_ratchet sub-hits on a dedicated tag range reserved solely for this
 * (SEQ_RATCHET_TAG_BASE.., see seq_core_config.h).
 */

/* ── State (per MAX_LAYERS[/xSEQ_TRACKS]) ────────────────────────────────
 * Runtime bookkeeping only — never persisted, never carries pattern data
 * (that lives in seq_layer_t and rides along with its own memmove/memset).
 * Deliberately reset wholesale (sequencer_core_trig_reset_all) on any layer
 * add/delete rather than shifted in lockstep with compaction — see the
 * call-site comments in seq_core_state.c for the rationale. */
uint32_t s_layer_loop_count[MAX_LAYERS];
uint8_t  s_layer_last_step[MAX_LAYERS];
bool     s_track_last_played[MAX_LAYERS][SEQ_TRACKS];

/* Dedicated xorshift32 PRNG, independent of seq_core_editors.c's s_lfo_rng_state:
 * that one is mutated from synth_ui_task (Core 0, 20 Hz); this one is mutated
 * exclusively from sequencer_core_service_tick(), which itself only ever runs
 * on the esp_timer "sequencer" task callback (also Core 0, but a different
 * task) — sharing one PRNG across two independently-scheduled tasks would be
 * a real (if narrow) data race. */
static uint32_t s_trig_rng_state = 0xA53C9E17u;

static inline uint8_t trig_rand_pct(void)
{
    uint32_t x = s_trig_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_trig_rng_state = x;
    return (uint8_t)(x % 100u);
}

/* Semitone span shared by every non-NONE transform mode: RANDOM offsets by
 * +/-SEQ_STEP_TRANSFORM_SPAN, RAMP walks 0..SEQ_STEP_TRANSFORM_SPAN across
 * successive layer loops then wraps. Fixed (no per-step param array) to keep
 * the OP-Z step-component subset minimal; one octave reads musically once the
 * result is re-snapped to the scale. */
#define SEQ_STEP_TRANSFORM_SPAN 12

/* Per-step pitch transform (spec 20 §3.1). Offsets the step's authored note per
 * fire, then re-snaps the result to the active chord/scale via the same engine
 * resolve the plain per-track path uses — unless step_quant_bypass is set, in
 * which case the transformed note is only bounds-clamped and emitted
 * chromatically (spec 20 §3.2). SEQ_STEP_TRANSFORM_NONE returns `base`
 * unchanged (this helper is only reached for decorated steps anyway). All
 * integer math: no float, no lock, safe on the Core-0 service-tick task. */
static uint8_t trig_transform_note(uint8_t layer_idx, const seq_layer_t *layer,
                                   uint8_t track, uint8_t step, uint8_t base)
{
    int offset;
    switch ((seq_step_transform_t)layer->step_transform[track][step]) {
        case SEQ_STEP_TRANSFORM_RANDOM:
            offset = (int)(trig_rand_pct() % (2u * SEQ_STEP_TRANSFORM_SPAN + 1u))
                     - SEQ_STEP_TRANSFORM_SPAN;
            break;
        case SEQ_STEP_TRANSFORM_RAMP_UP:
            offset = (int)(s_layer_loop_count[layer_idx]
                           % (SEQ_STEP_TRANSFORM_SPAN + 1u));
            break;
        case SEQ_STEP_TRANSFORM_RAMP_DOWN:
            offset = -(int)(s_layer_loop_count[layer_idx]
                            % (SEQ_STEP_TRANSFORM_SPAN + 1u));
            break;
        case SEQ_STEP_TRANSFORM_NONE:
        default:
            return base;
    }

    int transformed = (int)base + offset;
    if (transformed < 0)   transformed = 0;
    if (transformed > 127) transformed = 127;

    if (layer->step_quant_bypass[track][step]) {
        return sequencer_clamp_layer_note(layer, (uint8_t)transformed);
    }
    return sequencer_resolve_track_note(layer, (uint8_t)transformed);
}

/* ── Ratchet tag formula ──────────────────────────────────────────────────
 * Statically assigned per (layer, track, ratchet-slot) — never pooled/shared
 * across tracks — so a slow ratchet group can never overwrite another
 * track's in-flight schedule. Only one step per track is ever "current" at
 * a time, so SEQ_MAX_RATCHET dedicated slots per track fully cover the worst
 * case (see seq_core_config.h for the tag-space layout comment). */
static inline uint32_t ratchet_slot_index(uint8_t layer, uint8_t track, uint8_t slot)
{
    return ((uint32_t)layer * SEQ_TRACKS + track) * SEQ_MAX_RATCHET + slot;
}

static inline uint32_t ratchet_on_tag(uint8_t layer, uint8_t track, uint8_t slot)
{
    return SEQ_RATCHET_TAG_BASE + ratchet_slot_index(layer, track, slot) * 2u;
}

static inline uint32_t ratchet_off_tag(uint8_t layer, uint8_t track, uint8_t slot)
{
    return ratchet_on_tag(layer, track, slot) + 1u;
}

bool sequencer_core_step_is_decorated(const seq_layer_t *layer, uint8_t track, uint8_t step)
{
    if (track >= SEQ_TRACKS || step >= SEQ_MAX_STEPS) return false;
    return layer->step_prob[track][step]      != 100
        || layer->step_ratchet[track][step]   != 1
        || layer->step_cond_type[track][step] != (uint8_t)SEQ_STEP_COND_NONE
        || layer->step_transform[track][step] != (uint8_t)SEQ_STEP_TRANSFORM_NONE;
}

void sequencer_core_trig_reset(uint8_t layer_idx)
{
    if (layer_idx >= MAX_LAYERS) return;
    s_layer_loop_count[layer_idx] = 0;
    s_layer_last_step[layer_idx]  = 0xFF;   /* sentinel: no step seen yet this run */
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        s_track_last_played[layer_idx][t] = false;
    }
}

void sequencer_core_trig_reset_all(void)
{
    for (uint8_t i = 0; i < MAX_LAYERS; i++) {
        sequencer_core_trig_reset(i);
    }
}

void sequencer_core_trig_clear_all(uint8_t layer_idx)
{
    if (layer_idx >= MAX_LAYERS) return;
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        for (uint8_t k = 0; k < SEQ_MAX_RATCHET; k++) {
            sequencer_emit_clear_tag(ratchet_on_tag(layer_idx, t, k));
            sequencer_emit_clear_tag(ratchet_off_tag(layer_idx, t, k));
        }
    }
}

/* ── Conditional trig evaluation ──────────────────────────────────────── */
static bool trig_eval_condition(uint8_t layer_idx, const seq_layer_t *layer,
                                uint8_t track, uint8_t step)
{
    switch ((seq_step_cond_type_t)layer->step_cond_type[track][step]) {
        case SEQ_STEP_COND_FILL: {
            uint8_t n = layer->step_cond_param[track][step];
            if (n < 2) n = 2;
            return (s_layer_loop_count[layer_idx] % n) == 0;
        }
        case SEQ_STEP_COND_PREV:
            return s_track_last_played[layer_idx][track];
        case SEQ_STEP_COND_NONE:
        default:
            return true;
    }
}

static bool trig_roll_probability(uint8_t prob_pct)
{
    if (prob_pct >= 100) return true;
    if (prob_pct == 0)   return false;
    return trig_rand_pct() < prob_pct;
}

/* One-shot schedule step_ratchet[track][step] sub-hits, evenly spaced across
 * the step's slot. n==1 reuses the exact plain-path gate math (including the
 * melodic off-beat shortening) from sequencer_emit_step() (seq_core_engine.c)
 * so a merely-probabilistic or conditional (ratchet==1) step still feels
 * identical to a plain one when it does fire; n>1 subdivides the slot. */
static void trig_schedule_ratchets(uint8_t layer_idx, const seq_layer_t *layer,
                                   uint8_t track, uint8_t step, uint32_t now_ticks)
{
    uint8_t n = layer->step_ratchet[track][step];
    if (n < 1) n = 1;
    if (n > SEQ_MAX_RATCHET) n = SEQ_MAX_RATCHET;

    uint16_t sub_ticks;
    uint16_t gate;
    if (n == 1) {
        sub_ticks = SEQ_TICKS_PER_STEP;
        gate = (layer->type == SEQ_LAYER_DRUM) ? SEQ_GATE_DRUM : SEQ_GATE_MELODIC;
        if (layer->type == SEQ_LAYER_MELODIC && (step % 2) == 1 && gate > 2) {
            gate -= 2;
        }
    } else {
        sub_ticks = SEQ_TICKS_PER_STEP / n;
        if (sub_ticks < 2) sub_ticks = 2;
        gate = (uint16_t)(sub_ticks - 1);
    }

    float velocity = sequencer_step_velocity(layer, track, step) * layer->amp_scale[track];
    if (velocity > 1.0f) velocity = 1.0f;
    /* Single per-fire pitch source for both ratchet sub-hits and the plain
     * decorated (n==1) path: apply any per-step note transform here so every
     * sub-hit of one fire shares the same transformed pitch. */
    uint8_t note  = trig_transform_note(layer_idx, layer, track, step,
                                        layer->step_note[track][step]);
    uint8_t synth = layer->synth_id[track];

    for (uint8_t k = 0; k < n; k++) {
        uint32_t tick_on  = now_ticks + 1u + (uint32_t)k * sub_ticks;
        uint32_t tick_off = tick_on + gate;
        amy_send_note_sched(synth, (float)note, velocity,
                            ratchet_on_tag(layer_idx, track, k), tick_on, 0);
        amy_send_note_sched(synth, (float)note, 0.0f,
                            ratchet_off_tag(layer_idx, track, k), tick_off, 0);
    }
}

/* ── The one new per-tick entry point (called from main.c's AMY sequencer
 *    hook, ~2 kHz worst case, esp_timer task, Core 0) ───────────────────── */
void sequencer_core_service_tick(void)
{
    if (!s_playing) return;
    uint32_t now_ticks = sequencer_ticks();

    for (uint8_t li = 0; li < s_num_layers; li++) {
        seq_layer_t *layer = &s_layers[li];
        uint32_t bar_ticks = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
        if (bar_ticks == 0) continue;
        uint8_t cur_step = (uint8_t)((now_ticks % bar_ticks) / SEQ_TICKS_PER_STEP);
        if (cur_step == s_layer_last_step[li]) continue;  /* still mid-step: O(1) exit */

        if (cur_step == 0 && s_layer_last_step[li] != 0xFF) {
            s_layer_loop_count[li]++;
        }
        s_layer_last_step[li] = cur_step;

        for (uint8_t tr = 0; tr < layer->num_tracks; tr++) {
            bool on        = layer->grid[tr][cur_step];
            bool decorated = sequencer_core_step_is_decorated(layer, tr, cur_step);
            bool fire;

            if (!on) {
                fire = false;
            } else if (!decorated) {
                fire = true;  /* AMY's own periodic tag already fires this one */
            } else {
                fire = trig_eval_condition(li, layer, tr, cur_step)
                    && trig_roll_probability(layer->step_prob[tr][cur_step]);
                /* Mute/solo silences output only — condition/probability state
                 * (fire, s_track_last_played below) keeps evaluating normally
                 * so a track resumes its rhythmic position seamlessly when
                 * unmuted rather than freezing while muted. */
                if (fire && sequencer_track_audible(layer, tr)) {
                    trig_schedule_ratchets(li, layer, tr, cur_step, now_ticks);
                }
            }
            s_track_last_played[li][tr] = fire;
        }
    }
}

/* ── Public setters/getters ──────────────────────────────────────────────
 * Every setter re-emits the step so sequencer_emit_step() (seq_core_engine.c)
 * immediately re-resolves plain-vs-decorated scheduling; the trig service
 * itself always reads step_prob/step_ratchet/step_cond_* live, so no other
 * propagation is needed. */
void sequencer_core_set_step_prob(uint8_t layer_idx, uint8_t track, uint8_t step,
                                  uint8_t prob_pct)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    layer->step_prob[track][step] = SEQ_CLAMP_U8(prob_pct, 0, 100);
    sequencer_emit_step(layer_idx, track, step);
}

uint8_t sequencer_core_get_step_prob(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    if (layer_idx >= s_num_layers) return 100;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return 100;
    return layer->step_prob[track][step];
}

void sequencer_core_set_step_ratchet(uint8_t layer_idx, uint8_t track, uint8_t step,
                                     uint8_t count)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    layer->step_ratchet[track][step] = SEQ_CLAMP_U8(count, 1, SEQ_MAX_RATCHET);
    sequencer_emit_step(layer_idx, track, step);
}

uint8_t sequencer_core_get_step_ratchet(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    if (layer_idx >= s_num_layers) return 1;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return 1;
    return layer->step_ratchet[track][step];
}

void sequencer_core_set_step_cond(uint8_t layer_idx, uint8_t track, uint8_t step,
                                  seq_step_cond_type_t type, uint8_t param)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    if ((uint8_t)type >= SEQ_STEP_COND_COUNT) type = SEQ_STEP_COND_NONE;
    if (type == SEQ_STEP_COND_FILL) param = SEQ_CLAMP_U8(param, 2, 8);
    layer->step_cond_type[track][step]  = (uint8_t)type;
    layer->step_cond_param[track][step] = param;
    sequencer_emit_step(layer_idx, track, step);
}

void sequencer_core_get_step_cond(uint8_t layer_idx, uint8_t track, uint8_t step,
                                  seq_step_cond_type_t *type, uint8_t *param)
{
    seq_step_cond_type_t t = SEQ_STEP_COND_NONE;
    uint8_t p = 0;
    if (layer_idx < s_num_layers) {
        const seq_layer_t *layer = &s_layers[layer_idx];
        if (track < layer->num_tracks && step < layer->num_steps) {
            t = (seq_step_cond_type_t)layer->step_cond_type[track][step];
            p = layer->step_cond_param[track][step];
        }
    }
    if (type)  *type  = t;
    if (param) *param = p;
}

/* ── Per-step note transform + quantize bypass (spec 20 §3.1/§3.2) ─────────
 * Both re-emit the step so sequencer_emit_step() re-resolves plain-vs-decorated
 * (a non-NONE transform forces the decorated one-shot path). The quantize
 * bypass is a rider on the transform: it only alters output while a transform
 * is active (with NONE, the base note is already snapped and uniform per
 * track, so there is nothing to un-snap). */
void sequencer_core_set_step_transform(uint8_t layer_idx, uint8_t track, uint8_t step,
                                       seq_step_transform_t mode, bool quant_bypass)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    if ((uint8_t)mode >= SEQ_STEP_TRANSFORM_COUNT) mode = SEQ_STEP_TRANSFORM_NONE;
    layer->step_transform[track][step]    = (uint8_t)mode;
    layer->step_quant_bypass[track][step] = quant_bypass ? 1u : 0u;
    sequencer_emit_step(layer_idx, track, step);
}

void sequencer_core_get_step_transform(uint8_t layer_idx, uint8_t track, uint8_t step,
                                       seq_step_transform_t *mode, bool *quant_bypass)
{
    seq_step_transform_t m = SEQ_STEP_TRANSFORM_NONE;
    bool q = false;
    if (layer_idx < s_num_layers) {
        const seq_layer_t *layer = &s_layers[layer_idx];
        if (track < layer->num_tracks && step < layer->num_steps) {
            m = (seq_step_transform_t)layer->step_transform[track][step];
            q = layer->step_quant_bypass[track][step] != 0;
        }
    }
    if (mode)         *mode         = m;
    if (quant_bypass) *quant_bypass = q;
}
