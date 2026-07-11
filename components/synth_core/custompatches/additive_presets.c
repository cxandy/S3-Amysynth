#include "custompatches/additive_presets.h"
#include "custompatches/additive_voice.h"
#include "sequencer_core.h"   /* SEQ_PATCH_ADDITIVE_* */

/* Drawbar organ: exactly the shared default — first 8 harmonics at 1/n, no
 * per-partial decay, so the whole spectrum follows the row's ADSR. Kept as
 * its own function so the preset can drift from the default later without
 * touching the custom voice's starting point. */
static void additive_preset_organ(additive_voice_t *v)
{
    additive_voice_default(v);
}

/* Inharmonic bell: transverse free-bar partial ratios (1 : 2.76 : 5.40 :
 * 8.93 : 13.34 : 18.64). The bell character comes from the staggered
 * per-partial decays — upper partials ring down quickly, the hum and prime
 * linger — layered under whatever parent envelope the row authors. */
static void additive_preset_bell(additive_voice_t *v)
{
    additive_voice_default(v);
    v->num_partials = 6;

    static const float bell_ratio[6] = { 1.0f, 2.76f, 5.40f, 8.93f, 13.34f, 18.64f };
    static const float bell_level[6] = { 1.0f, 0.70f, 0.45f, 0.30f,  0.18f,  0.10f };
    static const float bell_decay[6] = { 2500.0f, 1800.0f, 1200.0f, 800.0f, 500.0f, 300.0f };

    for (uint8_t i = 0; i < 6; i++) {
        v->ratio[i]    = bell_ratio[i];
        v->level[i]    = bell_level[i];
        v->decay_ms[i] = bell_decay[i];
    }
    for (uint8_t i = 6; i < ADD_MAX_PARTIALS; i++) {
        v->ratio[i] = 1.0f; v->level[i] = 0.0f; v->decay_ms[i] = 0.0f;
    }
}

void additive_preset_configure_track(uint8_t synth_id, uint16_t patch,
                                     uint16_t num_voices)
{
    additive_voice_t v;
    switch (patch) {
        case SEQ_PATCH_ADDITIVE_ORGAN: additive_preset_organ(&v); break;
        case SEQ_PATCH_ADDITIVE_BELL:  additive_preset_bell(&v);  break;
        default:                       additive_voice_default(&v); break;
    }
    additive_voice_configure_track(synth_id, num_voices, &v);
}
