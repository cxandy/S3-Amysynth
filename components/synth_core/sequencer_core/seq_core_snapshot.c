/* Bulk layer snapshot support for project save/load.
 *
 * The per-field setters in sequencer_core.h each re-emit their own scheduled
 * steps on every call, and three per-step arrays (step_nudge/step_velocity_adj/
 * step_ratchet_taper) have no public setters at all. A project loader needs to
 * overwrite an entire layer's persisted state and settle the AMY side effects
 * exactly once, so this file copies the layer struct wholesale and then re-runs
 * the same "configure synth + resync" sequence the layer-management calls use.
 */

#include "sequencer_core.h"
#include "seq_core_internal.h"
#include "seq_core_config.h"
#include <string.h>

bool sequencer_core_export_layer(uint8_t layer_idx, seq_layer_t *out)
{
    if (layer_idx >= s_num_layers || !out) return false;
    *out = s_layers[layer_idx];   /* RAM-to-RAM copy is fine; only flash
                                     serialization must be field-by-field */
    return true;
}

bool sequencer_core_import_layer(uint8_t layer_idx, const seq_layer_t *src)
{
    if (layer_idx >= s_num_layers || !src) return false;
    seq_layer_t *dst = &s_layers[layer_idx];
    if (dst->type != src->type) return false;   /* topology fixed by caller */

    /* Preserve runtime-owned fields. */
    uint8_t synth_id[SEQ_TRACKS];
    memcpy(synth_id, dst->synth_id, sizeof synth_id);
    uint8_t num_tracks = dst->num_tracks;

    *dst = *src;
    memcpy(dst->synth_id, synth_id, sizeof synth_id);
    dst->num_tracks = num_tracks;
    dst->step_page  = 0;

    /* Re-push per-track sound config to the layer's synth slots. */
    if (dst->type == SEQ_LAYER_MELODIC)
        sequencer_core_set_layer_patch(layer_idx, dst->patch);   /* once - fans out */
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (dst->type == SEQ_LAYER_MELODIC) {
            if (dst->vp[t].env_authored)
                sequencer_configure_melodic_envelope_track(layer_idx, t);
            if (dst->vp[t].env1_authored)
                sequencer_configure_melodic_envelope1_track(layer_idx, t);
            if (dst->vp[t].filter_authored)
                sequencer_configure_melodic_filter_track(layer_idx, t);
        } else {
            sequencer_core_set_drum_patch(layer_idx, t, dst->track_patch[t]);
        }
    }
    if (dst->type == SEQ_LAYER_MELODIC) {
        sequencer_configure_melodic_lfo(layer_idx);
    } else if (sequencer_core_get_drum_engine() == SEQ_DRUM_PCM) {
        /* PCM mode plays from track_pcm_preset[] instead of track_patch[];
         * re-apply per-track so a saved sample_rec override survives reload. */
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            sequencer_core_set_drum_pcm_preset(layer_idx, t, dst->track_pcm_preset[t]);
        }
    }

    sequencer_core_trig_reset_all();
    sequencer_resync_layer(layer_idx);
    return true;
}
