#include "custompatches/fm_voice.h"
#include "amy.h"           /* ALGO, SINE, COEF_*, ENVELOPE_*, MAX_ALGO_OPS */
#include "amy_helpers.h"   /* amy_helpers_event_begin/send */

/* FM_NUM_OPS must track amy.h's MAX_ALGO_OPS (algo_source[] array size) —
 * catch a future AMY update that changes it before it silently truncates the
 * operator wiring below. */
_Static_assert(FM_NUM_OPS == MAX_ALGO_OPS, "FM_NUM_OPS must equal AMY's MAX_ALGO_OPS");

/* The live-editable "custom" FM voice — see fm_voice.h for the ownership
 * convention (mirrors amy_fx.h's s_fx). */
fm_voice_t s_fm_voice;

/* Shared short default envelope baked onto every operator osc at configure
 * time. The row's own per-track ADSR editor (seq_core_editors.c) instead
 * targets osc 0 only (the ALGO control osc's own envelope, which scales any
 * operator that is a final carrier) — see fm_voice.h's header comment on the
 * per-operator-envelope limitation. */
#define FM_OP_ATTACK_MS  4u
#define FM_OP_DECAY_MS   300u
#define FM_OP_SUSTAIN    0.6f
#define FM_OP_RELEASE_MS 200u

void fm_voice_default(fm_voice_t *v)
{
    if (!v) return;
    v->algorithm = 0;
    v->feedback  = 0.0f;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        v->op_ratio[i] = 1.0f;
        v->op_level[i] = 0.0f;
    }
    /* Algorithm 0's second chain is op_source index 4 (modulator) driving
     * index 5 (carrier) — see algorithms.c and the worked trace in
     * docs/handoff/a2-fm-dx7.md. This is the only pair enabled by default so
     * a freshly-selected custom voice is immediately audible as a plain
     * 2-operator FM tone. */
    v->op_ratio[4] = 1.0f;  v->op_level[4] = 0.5f;
    v->op_ratio[5] = 1.0f;  v->op_level[5] = 1.0f;
}

void fm_voice_configure_track(uint8_t synth_id, uint16_t num_voices,
                              const fm_voice_t *voice)
{
    if (!voice) return;

    amy_event *e = amy_helpers_event_begin();
    e->synth          = synth_id;
    e->num_voices     = num_voices;
    e->oscs_per_voice = FM_NUM_OPS + 1;
    amy_helpers_event_send(e);

    /* osc 0: ALGO control osc. Carries the shared voice envelope/velocity and
     * the algorithm's operator wiring. algo_source[i] always points at
     * relative osc (i+1); AMY offsets it by the voice's base_osc internally. */
    e = amy_helpers_event_begin();
    e->synth                 = synth_id;
    e->osc                   = 0;
    e->wave                  = ALGO;
    e->algorithm              = voice->algorithm;
    e->feedback              = voice->feedback;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        e->algo_source[i] = (int16_t)(i + 1);
    }
    e->freq_coefs[COEF_NOTE] = 1.0f;
    e->amp_coefs[COEF_CONST] = 1.0f;
    e->amp_coefs[COEF_VEL]   = 1.0f;
    e->amp_coefs[COEF_EG0]   = 1.0f;
    e->eg_type[0]            = ENVELOPE_NORMAL;
    e->eg0_times[0]  = 5;    e->eg0_values[0] = 1.0f;
    e->eg0_times[1]  = 250;  e->eg0_values[1] = 0.6f;
    e->eg0_times[2]  = 200;  e->eg0_values[2] = 0.0f;
    amy_helpers_event_send(e);

    /* oscs 1..6: SINE operators. Each keeps its own short default envelope so
     * it decays even before the row author an explicit ADSR (see fm_voice.h). */
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        e = amy_helpers_event_begin();
        e->synth                 = synth_id;
        e->osc                   = (uint16_t)(i + 1);
        e->wave                  = SINE;
        e->ratio                 = voice->op_ratio[i];
        e->freq_coefs[COEF_NOTE] = 1.0f;
        e->amp_coefs[COEF_CONST] = voice->op_level[i];
        e->amp_coefs[COEF_VEL]   = 1.0f;
        e->amp_coefs[COEF_EG0]   = 1.0f;
        e->eg_type[0]            = ENVELOPE_NORMAL;
        e->eg0_times[0]  = FM_OP_ATTACK_MS;  e->eg0_values[0] = 1.0f;
        e->eg0_times[1]  = FM_OP_DECAY_MS;   e->eg0_values[1] = FM_OP_SUSTAIN;
        e->eg0_times[2]  = FM_OP_RELEASE_MS; e->eg0_values[2] = 0.0f;
        amy_helpers_event_send(e);
    }
}

void fm_voice_push_live(uint8_t synth_id, const fm_voice_t *voice)
{
    if (!voice) return;

    amy_event *e = amy_helpers_event_begin();
    e->synth     = synth_id;
    e->osc       = 0;
    e->algorithm = voice->algorithm;
    e->feedback  = voice->feedback;
    amy_helpers_event_send(e);

    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        e = amy_helpers_event_begin();
        e->synth                 = synth_id;
        e->osc                   = (uint16_t)(i + 1);
        e->ratio                 = voice->op_ratio[i];
        e->amp_coefs[COEF_CONST] = voice->op_level[i];
        amy_helpers_event_send(e);
    }
}
