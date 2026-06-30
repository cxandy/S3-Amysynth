#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "patch_names.h"
#include "patch_cycle.h"
#include "sdkconfig.h"
#include "esp_log.h"

static const char *TAG = "synth_ui";

#if !CONFIG_SEQ_PATCH_BROWSE_FULL_RANGE
/* Runtime patch cycling shortlist: intentionally small and musical.
 * Values map to AMY built-ins (Juno/DX7/piano). Used only when the browse mode
 * is "preselected"; the full-range mode walks 0..SEQ_PATCH_FULL_MAX instead. */
static const uint16_t s_melodic_patch_cycle[] = {
    138, /* DX7 E.PIANO 1 */
    135, /* DX7 PIANO 1 */
    141, /* DX7 SYN-LEAD 1 */
    151, /* DX7 FLUTE 1 */
    7,   /* Juno A18 Piano I */
    104, /* Juno B61 E. Piano with Tremolo */
    256, /* Built-in piano */
    257, /* Raw SINE */
    258, /* Raw SAW DOWN */
    259, /* Raw SAW UP */
    260, /* Raw PULSE */
    261, /* Raw TRIANGLE */
    262, /* Raw NOISE */
    263, /* Raw KS */
    264, /* Bass 1: Sub-Heavy Detune (PULSE + detuned SAW, LPF24) */
    265, /* Bass 2: Sine-Reinforced Acid/Pluck (SINE + SAW, LPF24) */
    266, /* Bass 3: FM DX7-Style (SINE + sub-octave SINE, DX7 env) */
};
#define SEQ_RUNTIME_PATCH_COUNT ((int)(sizeof(s_melodic_patch_cycle) / sizeof(s_melodic_patch_cycle[0])))
#endif

/* Full-range browse: Juno 0..127, DX7 128..255, piano 256, waves 257..263, bass 264..266. */
#define SEQ_PATCH_FULL_MAX 266

/* Domain descriptor for melodic, arp, and drone patch cycling.
 * Full-range mode walks 0..SEQ_PATCH_FULL_MAX; curated mode steps the
 * shortlist above.  Shared by all three non-drum wrappers. */
#if CONFIG_SEQ_PATCH_BROWSE_FULL_RANGE
static const patch_domain_t s_melodic_domain = {
    .list = NULL, .count = 0, .full_max = SEQ_PATCH_FULL_MAX
};
#else
static const patch_domain_t s_melodic_domain = {
    .list = s_melodic_patch_cycle, .count = SEQ_RUNTIME_PATCH_COUNT,
    .full_max = SEQ_PATCH_FULL_MAX
};
#endif

void synth_ui_cycle_melodic_patch(int delta)
{
    if (delta == 0) return;

    uint8_t li = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return;
    if (seq_state.layers[li].type != SEQ_LAYER_MELODIC) return;

    int dir = (delta > 0) ? 1 : -1;
    uint16_t next = patch_domain_step(&s_melodic_domain, sequencer_core_get_layer_patch(li), dir);

    sequencer_core_set_layer_patch(li, next);

    uint16_t applied = sequencer_core_get_layer_patch(li);
    seq_state.layers[li].patch = applied;

    const char *name = patch_name_for(applied);
    if (name) {
        ESP_LOGI(TAG, "L%u melodic patch -> %u (%s)", (unsigned)li, (unsigned)applied, name);
    } else {
        ESP_LOGI(TAG, "L%u melodic patch -> %u", (unsigned)li, (unsigned)applied);
    }
}

/* Cycle the SELECTED drum track's patch through the curated drum list. The drum
 * layer is per-track, so this targets seq_state.selected_track on the active
 * layer (must be a drum layer). Mirrors the applied patch into the UI copy so
 * the on-screen patch number updates. */
void synth_ui_cycle_drum_patch(int delta)
{
    if (delta == 0) return;

    uint8_t li = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return;
    if (seq_state.layers[li].type != SEQ_LAYER_DRUM) return;

    uint8_t track = seq_state.selected_track;
    if (track >= SEQ_TRACKS) return;

    int dir = (delta > 0) ? 1 : -1;
    uint16_t applied = sequencer_core_cycle_drum_patch(li, track, dir);

    /* Keep the UI mirror in sync (track_patch[] drives the label). */
    seq_state.layers[li].track_patch[track] = applied;
    if (track == 0) seq_state.layers[li].patch = applied;

    const char *name = patch_name_for(applied);
    if (name) {
        ESP_LOGI(TAG, "drum patch cycle L%u t%u -> %u (%s)",
                 li, track, (unsigned)applied, name);
    } else {
        ESP_LOGI(TAG, "drum patch cycle L%u t%u -> %u", li, track, (unsigned)applied);
    }
}

/* Cycle the arp's OWN patch (independent of the sequencer's melodic patch).
 * Reuses the same browse-mode stepping + name lookup as the sequencer. */
void synth_ui_arp_cycle_patch(int delta)
{
    if (delta == 0) return;
    int dir = (delta > 0) ? 1 : -1;
    uint16_t next = patch_domain_step(&s_melodic_domain, arp_get_patch(), dir);
    arp_set_patch(next);

    const char *name = patch_name_for(next);
    if (name) {
        ESP_LOGI(TAG, "arp patch cycle -> %u (%s)", (unsigned)next, name);
    } else {
        ESP_LOGI(TAG, "arp patch cycle -> %u", (unsigned)next);
    }
}

/* Cycle the drone's PATCH-mode preset (hold+turn gesture on the drone screen).
 * Reuses the same browse-mode stepping + name lookup as the others. */
void synth_ui_drone_cycle_patch(int delta)
{
    if (delta == 0) return;
    int dir = (delta > 0) ? 1 : -1;
    uint16_t next = patch_domain_step(&s_melodic_domain, drone_get_patch(), dir);
    drone_set_patch(next);

    const char *name = patch_name_for(next);
    if (name) {
        ESP_LOGI(TAG, "drone patch cycle -> %u (%s)", (unsigned)next, name);
    } else {
        ESP_LOGI(TAG, "drone patch cycle -> %u", (unsigned)next);
    }
    s_force_redraw = true;
}
