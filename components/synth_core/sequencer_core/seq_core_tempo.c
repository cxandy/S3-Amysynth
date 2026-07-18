#include "sequencer_core/seq_core_internal.h"
#include "custompatches/drone_std_core.h"   /* drone_std_core_refresh_lfo_freq */

/* ── State definitions — owns BPM and quantizer ─────────────────────── */
uint16_t s_bpm = SEQ_DEFAULT_BPM;
quantizer_state_t s_quantizer = {
    .root_note  = CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE,
    .scale_index = CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE,
    .enabled    = CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED,
};

/* ── BPM helpers ─────────────────────────────────────────────────────── */

uint16_t sequencer_clamp_bpm(uint16_t b)
{
    return SEQ_CLAMP_U16(b, SEQ_MIN_BPM, SEQ_MAX_BPM);
}

void sequencer_push_tempo(uint16_t b)
{
    amy_event *e = amy_helpers_event_begin();
    e->tempo = b;
    amy_helpers_event_send(e);
}

/* ── LFO helpers ─────────────────────────────────────────────────────── */

float lfo_rate_to_hz(lfo_rate_t rate, uint16_t bpm)
{
    float b = (float)bpm;
    switch (rate) {
        case LFO_RATE_1_8:  return b / 30.0f;    /* 1/8 note */
        case LFO_RATE_1_4:  return b / 60.0f;    /* 1/4 note */
        case LFO_RATE_1_2:  return b / 120.0f;   /* 1/2 note */
        case LFO_RATE_1BAR: return b / 240.0f;   /* 1 bar (4/4) */
        case LFO_RATE_2BAR: return b / 480.0f;
        case LFO_RATE_4BAR: return b / 960.0f;
        default:            return b / 240.0f;
    }
}

float lfo_next_rand(void)
{
    s_lfo_rng_state ^= s_lfo_rng_state << 13;
    s_lfo_rng_state ^= s_lfo_rng_state >> 17;
    s_lfo_rng_state ^= s_lfo_rng_state << 5;
    return (float)(s_lfo_rng_state >> 17) / 32767.0f * 2.0f - 1.0f;
}

void lfo_push_target_neutral(uint8_t synth_id, lfo_target_t target)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth = synth_id;
    switch (target) {
        case LFO_TARGET_FILTER: e->filter_freq_coefs[COEF_CONST] = 8000.0f; break;
        case LFO_TARGET_AMP:    e->amp_coefs[COEF_CONST]  = 1.0f;           break;
        case LFO_TARGET_PITCH:  e->freq_coefs[COEF_CONST] = 1.0f;           break;
        case LFO_TARGET_PAN:    e->pan_coefs[COEF_CONST]  = 0.5f;           break;
        default: break;
    }
    amy_helpers_event_send(e);
}

/* ── Public API — BPM ────────────────────────────────────────────────── */

void sequencer_core_set_bpm(uint16_t new_bpm)
{
    s_bpm = sequencer_clamp_bpm(new_bpm);
    sequencer_push_tempo(s_bpm);
    for (int li = 0; li < s_num_layers; li++) {
        for (int tr = 0; tr < SEQ_TRACKS; tr++) {
            if (s_layers[li].vp[tr].lfo_authored && s_layers[li].vp[tr].lfo.enabled)
                s_lfo_hz[li][tr] = lfo_rate_to_hz(s_layers[li].vp[tr].lfo.rate, s_bpm);
        }
    }
    /* Sync the arp WAVE-mode LFO carrier to the new BPM (no-op when not active). */
    arp_core_refresh_lfo_freq();
    /* Same for the normal drone's native LFO carrier. */
    drone_std_core_refresh_lfo_freq();
    /* Sync native LFO carriers on all melodic wave-patch tracks. */
    melodic_lfo_refresh_native_freq();
}

uint16_t sequencer_core_get_bpm(void) { return s_bpm; }

/* ── Public API — quantizer ──────────────────────────────────────────── */

void sequencer_core_set_quantizer_enabled(bool enabled)
{
    if (s_quantizer.enabled == enabled) return;
    s_quantizer.enabled = enabled;
    sequencer_refresh_melodic_layers(false);
    ESP_LOGI(TAG, "quantizer %s", enabled ? "enabled" : "disabled");
}

void sequencer_core_set_quantizer_root_note(uint8_t root_note)
{
    s_quantizer.root_note = root_note;
    sequencer_refresh_melodic_layers(false);
    ESP_LOGI(TAG, "quantizer root -> %u", root_note);
}

void sequencer_core_set_quantizer_scale(uint8_t scale_index)
{
    s_quantizer.scale_index = (scale_index >= quantizer_scale_count()) ? 0 : scale_index;
    sequencer_refresh_melodic_layers(false);
    ESP_LOGI(TAG, "quantizer scale -> %u", s_quantizer.scale_index);
}

bool sequencer_core_get_quantizer_enabled(void)
{
    return s_quantizer.enabled;
}

uint8_t sequencer_core_get_quantizer_root_note(void)
{
    return s_quantizer.root_note;
}

uint8_t sequencer_core_get_quantizer_scale(void)
{
    return s_quantizer.scale_index;
}
