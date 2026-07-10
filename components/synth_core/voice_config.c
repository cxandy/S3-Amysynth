#include "voice_config.h"
#include "amy.h"   /* SINE, TRIANGLE, SAW_UP, SAW_DOWN, PULSE wave constants */

uint16_t voice_lfo_wave_to_amy(lfo_wave_t wave)
{
    switch (wave) {
        case LFO_WAVE_SINE:     return SINE;
        case LFO_WAVE_TRIANGLE: return TRIANGLE;
        case LFO_WAVE_SAW_UP:   return SAW_UP;
        case LFO_WAVE_SAW_DOWN: return SAW_DOWN;
        case LFO_WAVE_SQUARE:   return PULSE;
        default:                return SINE;   /* LFO_WAVE_RANDOM -> SINE fallback */
    }
}

void voice_env_apply_ks_noise_floor(seq_env_t *env, bool is_ks, bool is_noise)
{
    if (!env) return;
    if (is_ks) {
        env->attack_ms   = 2;   /* onset transient suppressed by attack ramp */
        env->sustain_pct = 0;   /* string body carried by KS feedback decay  */
    } else if (is_noise) {
        env->attack_ms   = 2;   /* onset transient suppressed by attack ramp */
    }
}
