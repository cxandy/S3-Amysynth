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

    /* Rebuild the per-track source notes from the loaded resolved notes.
     * s_track_source_note is not persisted, and leaving the pre-load values
     * in place meant the first re-resolve after load (note nudge, progression
     * advance) snapped every track back to stale pitches — and would strip a
     * loaded chord sentinel entirely. The resolved note re-resolves to itself
     * (idempotent snap), so it is a faithful source reconstruction. Chord
     * sentinels double as their own source; plain notes also seed the
     * chord-delete fallback. */
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        uint8_t n = dst->track_base_note[t];
        s_track_source_note[layer_idx][t] = n;
        if (!SEQ_NOTE_IS_CHORD(n)) {
            s_track_prev_plain[layer_idx][t] = n;
        }
    }

    /* Re-push the full sound config to the layer's synth slots. The public
     * patch setters dedup against the layer's stored patch number - which the
     * struct copy above has already overwritten - so they would silently
     * no-op here. Call the configure path directly instead: it reads patch,
     * num_voices, and synth_flags from the just-copied struct (looping every
     * drum track's track_patch[] in drum-SYNTH mode) and pushes the authored
     * env/env1/filter/LFO for melodic layers itself. */
    sequencer_configure_synth(layer_idx);
    if (dst->type == SEQ_LAYER_DRUM) {
        /* Restore each track's PCM preset selection: live-reloads the osc when
         * the PCM engine is active, otherwise stores the override for the next
         * engine toggle. */
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            sequencer_core_set_drum_pcm_preset(layer_idx, t, dst->track_pcm_preset[t]);
        }
    }

    sequencer_core_trig_reset_all();
    sequencer_resync_layer(layer_idx);
    return true;
}
