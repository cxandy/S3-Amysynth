/* bass_presets.c — three two-oscillator bass presets for melodic layers.
 *
 * Patch IDs 264-266 (SEQ_PATCH_BASS_1/2/3).  Dispatched from
 * sequencer_configure_synth() in sequencer_core.c when is_bass_patch is true.
 *
 * All AMY interaction goes through the queued event API (amy_helpers_event_*)
 * — never direct synth[] access.  These configure functions run from
 * synth_ui_task (Core 0), outside the render body; this is consistent with
 * the amy_render lock rules documented in AMY-EDITS.md.
 *
 * filter_freq_coefs[] note: COEF_CONST is in Hz (converted to logfreq
 * internally by EVENT_TO_DELTA_FREQ_COEFS).  Non-CONST coefs (COEF_EG0 etc.)
 * are dimensionless logfreq-delta weights applied linearly by combine_controls.
 * COEF_EG0 = 4.06 → filter sweeps log2(2000/8.18) - log2(120/8.18) ≈ 4.06
 * octaves when EG0 peaks (120 Hz → ~2000 Hz). */

#include "custompatches/bass_presets.h"
#include "sequencer_core.h"    /* SEQ_PATCH_BASS_* */
#include "amy.h"               /* PULSE, SAW_DOWN, SINE, FILTER_LPF24, COEF_*, ENVELOPE_* */
#include "amy_helpers.h"       /* amy_helpers_event_begin/send */

void bass_preset_configure_track(uint8_t synth_id, uint16_t patch,
                                 uint16_t num_voices)
{
    /* Allocate: 2 oscillators per voice. */
    amy_event *e = amy_helpers_event_begin();
    e->synth          = synth_id;
    e->num_voices     = num_voices;
    e->oscs_per_voice = 2;
    amy_helpers_event_send(e);

    if (patch == SEQ_PATCH_BASS_1) {
        /* ─── Preset 264: Classic Sub-Heavy Detune Bass ─────────────────
         * PULSE carrier + detuned SAW_DOWN sub, LPF24 on carrier.
         * Osc 1 detuned +1.005 ratio (~8.6 cents) for chorus thickness.
         * Filter env-swept: 120 Hz → ~2000 Hz at peak attack. */

        /* osc 0: PULSE carrier with envelope-swept low-pass filter */
        e = amy_helpers_event_begin();
        e->synth                          = synth_id;
        e->osc                            = 0;
        e->wave                           = PULSE;
        e->freq_coefs[COEF_NOTE]          = 1.0f;
        e->amp_coefs[COEF_CONST]          = 1.0f;
        e->amp_coefs[COEF_VEL]            = 1.0f;
        e->amp_coefs[COEF_EG0]            = 1.0f;
        e->filter_type                    = FILTER_LPF24;
        e->resonance                      = 0.5f;
        e->filter_freq_coefs[COEF_CONST]  = 120.0f;   /* start low per spec */
        e->filter_freq_coefs[COEF_EG0]    = 4.06f;    /* EG0 sweeps 120→~2000 Hz at peak */
        e->chained_osc                    = 1;         /* voice-local osc 1 */
        e->eg_type[0]                     = ENVELOPE_NORMAL;
        e->eg0_times[0]  = 5;    e->eg0_values[0] = 1.0f;   /* atk  5ms */
        e->eg0_times[1]  = 250;  e->eg0_values[1] = 0.4f;   /* dec 250ms, sus 40% */
        e->eg0_times[2]  = 150;  e->eg0_values[2] = 0.0f;   /* rel 150ms */
        amy_helpers_event_send(e);

        /* osc 1: SAW_DOWN, slightly detuned (+1.005 ratio ≈ 8.6 cents) */
        e = amy_helpers_event_begin();
        e->synth                 = synth_id;
        e->osc                   = 1;
        e->wave                  = SAW_DOWN;
        e->freq_coefs[COEF_NOTE] = 1.0f;
        e->ratio                 = 1.005f;   /* log2(1.005) => logratio => +0.7% pitch */
        e->amp_coefs[COEF_CONST] = 0.8f;    /* slightly quieter than carrier */
        e->amp_coefs[COEF_VEL]   = 1.0f;
        e->amp_coefs[COEF_EG0]   = 1.0f;
        e->eg_type[0]            = ENVELOPE_NORMAL;
        e->eg0_times[0]  = 5;    e->eg0_values[0] = 1.0f;
        e->eg0_times[1]  = 250;  e->eg0_values[1] = 0.4f;
        e->eg0_times[2]  = 150;  e->eg0_values[2] = 0.0f;
        amy_helpers_event_send(e);

    } else if (patch == SEQ_PATCH_BASS_2) {
        /* ─── Preset 265: Solid Sine-Reinforced Acid/Pluck Bass ─────────
         * SINE sub (clean, unfiltered) + SAW_DOWN bite with LPF24/high Q.
         * The SINE guarantees a clean sub even when the filter closes.
         * 80 Hz cutoff puts the squelch resonance peak in the bass register. */

        /* osc 0: SINE sub-bass (no filter — bypasses completely) */
        e = amy_helpers_event_begin();
        e->synth                 = synth_id;
        e->osc                   = 0;
        e->wave                  = SINE;
        e->freq_coefs[COEF_NOTE] = 1.0f;
        e->amp_coefs[COEF_CONST] = 1.0f;
        e->amp_coefs[COEF_VEL]   = 1.0f;
        e->amp_coefs[COEF_EG0]   = 1.0f;
        e->chained_osc           = 1;         /* voice-local osc 1 */
        e->eg_type[0]            = ENVELOPE_NORMAL;
        e->eg0_times[0]  = 10;   e->eg0_values[0] = 1.0f;   /* atk 10ms */
        e->eg0_times[1]  = 180;  e->eg0_values[1] = 0.5f;   /* dec 180ms, sus 50% */
        e->eg0_times[2]  = 100;  e->eg0_values[2] = 0.0f;   /* rel 100ms */
        amy_helpers_event_send(e);

        /* osc 1: SAW_DOWN bite with aggressive filter (high resonance squelch) */
        e = amy_helpers_event_begin();
        e->synth                          = synth_id;
        e->osc                            = 1;
        e->wave                           = SAW_DOWN;
        e->freq_coefs[COEF_NOTE]          = 1.0f;
        e->amp_coefs[COEF_CONST]          = 0.7f;
        e->amp_coefs[COEF_VEL]            = 1.0f;
        e->amp_coefs[COEF_EG0]            = 1.0f;
        e->filter_type                    = FILTER_LPF24;
        e->resonance                      = 1.5f;             /* squelch resonance */
        e->filter_freq_coefs[COEF_CONST]  = 80.0f;            /* squelch in bass register per spec */
        e->eg_type[0]                     = ENVELOPE_NORMAL;
        e->eg0_times[0]  = 5;    e->eg0_values[0] = 1.0f;   /* atk  5ms (snappy) */
        e->eg0_times[1]  = 120;  e->eg0_values[1] = 0.2f;   /* dec 120ms, sus 20% */
        e->eg0_times[2]  = 100;  e->eg0_values[2] = 0.0f;   /* rel 100ms */
        amy_helpers_event_send(e);

    } else if (patch == SEQ_PATCH_BASS_3) {
        /* ─── Preset 266: FM DX7-Style Bass ─────────────────────────────
         * SINE carrier + sub-octave SINE (ratio=0.5) with DX7 envelopes.
         * Osc 1 uses a sharp DX7 decay that mimics an FM modulator's
         * "brightness over time" effect (metallic click at note start).
         * Note: AMY mod_source is AMP-domain; for pitch-tracking audio-rate
         * FM, this preset uses a sub-octave approach with ENVELOPE_DX7
         * on osc 1 to approximate DX7 bass tonal character. */

        /* osc 0: SINE carrier, DX7 envelope (will be overwritten by melodic env) */
        e = amy_helpers_event_begin();
        e->synth                 = synth_id;
        e->osc                   = 0;
        e->wave                  = SINE;
        e->freq_coefs[COEF_NOTE] = 1.0f;
        e->amp_coefs[COEF_CONST] = 1.0f;
        e->amp_coefs[COEF_VEL]   = 1.0f;
        e->amp_coefs[COEF_EG0]   = 1.0f;
        e->chained_osc           = 1;         /* voice-local osc 1 */
        e->eg_type[0]            = ENVELOPE_DX7;
        e->eg0_times[0]  = 5;    e->eg0_values[0] = 1.0f;   /* atk  5ms */
        e->eg0_times[1]  = 200;  e->eg0_values[1] = 0.8f;   /* dec 200ms, sus 80% */
        e->eg0_times[2]  = 150;  e->eg0_values[2] = 0.0f;   /* rel 150ms */
        amy_helpers_event_send(e);

        /* osc 1: SINE at ratio=0.5 (one octave sub), sharp DX7 decay for
         * transient metallic click characteristic of DX7 FM bass. */
        e = amy_helpers_event_begin();
        e->synth                 = synth_id;
        e->osc                   = 1;
        e->wave                  = SINE;
        e->freq_coefs[COEF_NOTE] = 1.0f;
        e->ratio                 = 0.5f;     /* one octave below carrier */
        e->amp_coefs[COEF_CONST] = 0.7f;
        e->amp_coefs[COEF_VEL]   = 1.0f;
        e->amp_coefs[COEF_EG0]   = 1.0f;
        e->eg_type[0]            = ENVELOPE_DX7;
        e->eg0_times[0]  = 2;    e->eg0_values[0] = 1.0f;   /* atk  2ms (attack floor) */
        e->eg0_times[1]  = 75;   e->eg0_values[1] = 0.1f;   /* dec  75ms, sus 10% */
        e->eg0_times[2]  = 100;  e->eg0_values[2] = 0.0f;   /* rel 100ms */
        amy_helpers_event_send(e);
    }
}
