#include "custompatches/fm_voice.h"
#include "amy.h"           /* ALGO, SINE, COEF_*, ENVELOPE_*, MAX_ALGO_OPS */
#include "amy_helpers.h"   /* amy_helpers_event_begin/send */

/* FM_NUM_OPS must track amy.h's MAX_ALGO_OPS (algo_source[] size): an AMY
 * update that changes it would silently truncate the wiring below. */
_Static_assert(FM_NUM_OPS == MAX_ALGO_OPS, "FM_NUM_OPS must equal AMY's MAX_ALGO_OPS");

/* The live-editable "custom" FM voice; ownership convention in fm_voice.h
 * (mirrors amy_fx.h's s_fx). */
fm_voice_t s_fm_voice;

/* Short default envelope baked onto every operator osc at configure time. The
 * per-track ADSR editor instead targets osc 0 only - the ALGO control osc,
 * whose envelope scales any operator that is a final carrier (fm_voice.h on
 * the per-operator-envelope limitation). */
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
    /* Algorithm 0's second chain is op_source 4 (modulator) driving 5
     * (carrier) - see algorithms.c. Enabling only this pair makes a freshly
     * selected custom voice immediately audible as a plain 2-op FM tone. */
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

    /* oscs 1..6: SINE operators, each with the short default envelope so they
     * decay before the row authors an explicit ADSR (fm_voice.h). */
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        e = amy_helpers_event_begin();
        e->synth                 = synth_id;
        e->osc                   = (uint16_t)(i + 1);
        e->wave                  = SINE;
        e->ratio                 = voice->op_ratio[i];
        e->freq_coefs[COEF_NOTE] = 1.0f;
        e->amp_coefs[COEF_CONST] = voice->op_level[i];
        /* VEL must be 0 on operators: AMY never delivers velocity to
         * SYNTH_IS_ALGO_SOURCE oscs, so their velocity input stays 0. Under
         * the dB amp-combine a nonzero VEL coef with 0 input contributes
         * -60 dB and amp_combine_controls floors the result to exactly 0 -
         * every operator renders silence. Velocity comes from the ALGO control
         * osc (VEL=1 above), whose amp scales the carriers in render_algo.
         * Same convention as the built-in DX7 patch strings. */
        e->amp_coefs[COEF_VEL]   = 0.0f;
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
