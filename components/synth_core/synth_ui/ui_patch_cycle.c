#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#include "patch_names.h"
#include "patch_cycle.h"
#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS
#include "live_play.h"
#endif
#include "esp_log.h"

static const char *TAG = "synth_ui";

#if !CONFIG_SEQ_PATCH_BROWSE_FULL_RANGE
/* THE shared patch catalog: one curated, musical shortlist for every cycling
 * consumer (melodic, arp, drone). Used only when the browse mode is
 * "preselected"; the full-range mode walks 0..SEQ_PATCH_FULL_MAX instead.
 * Consumers that cannot play an entry EXCLUDE it via their domain's
 * `excluded` predicate (see patch_cycle.h) — new patches added here (or new
 * ranges under SEQ_PATCH_FULL_MAX) reach every consumer automatically. */
static const uint16_t s_patch_catalog[] = {
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
    /* Kconfig-gated ranges are listed unconditionally: when a feature is
     * compiled out, its numbers are skipped at cycle time by the domains'
     * sequencer_core_patch_compiled_out() exclusion — one mechanism for
     * curated and full-range browse alike, no #if bookkeeping here. */
    267, /* Wavetable: 111.WAV      */
    268, /* Wavetable: BRAIDS01.WAV */
    269, /* Wavetable: PPG_WA00.WAV */
    270, /* Wavetable: SINE2SAW.WAV */
    271, /* Wavetable: VIRAL.WAV    */
    272, /* FM Bass (6-op ALGO) */
    273, /* FM E.Piano (6-op ALGO) */
    274, /* FM Bell (6-op ALGO) */
    275, /* FM Lead (6-op ALGO) */
    276, /* FM Custom — opens the FM edit screen (Menu > Screen: FM) */
    277, /* Add Organ (BYO_PARTIALS, 8 harmonics) */
    278, /* Add Bell (inharmonic partial ratios)  */
    279, /* Add Custom — live-editable additive voice (editor screen TBD) */
};
#define SEQ_RUNTIME_PATCH_COUNT ((int)(sizeof(s_patch_catalog) / sizeof(s_patch_catalog[0])))
#endif

/* Full-range browse walks 0..SEQ_PATCH_FULL_MAX (sequencer_core.h — always
 * the top of the fixed numbering space): Juno 0..127, DX7 128..255,
 * piano 256, waves 257..263, bass 264..266, wavetable banks 267..271,
 * FM/ALGO 272..276, additive 277..279. Ranges whose Kconfig gate is off are
 * interior holes skipped by the domains' compiled-out exclusion. */
#if CONFIG_SEQ_PATCH_BROWSE_FULL_RANGE
#define PATCH_DOMAIN_CATALOG .list = NULL, .count = 0, .full_max = SEQ_PATCH_FULL_MAX
#else
#define PATCH_DOMAIN_CATALOG .list = s_patch_catalog, \
    .count = SEQ_RUNTIME_PATCH_COUNT, .full_max = SEQ_PATCH_FULL_MAX
#endif

/* Melodic and arp play everything in the catalog that is compiled into this
 * build — ranges gated off by Kconfig (wavetable/FM/additive) are interior
 * holes in the fixed numbering and get skipped dynamically. */
static const patch_domain_t s_melodic_domain = {
    PATCH_DOMAIN_CATALOG, .excluded = sequencer_core_patch_compiled_out
};

/* Drone: same catalog, minus what its excitation model can't play
 * (NOISE/KS/bass/FM/additive — see drone_patch_excluded() in drone_core.c,
 * which also folds in the compiled-out check above). The
 * predicate is skipped during stepping, so cycling stays monotonic in both
 * browse modes and never trips drone_set_patch()'s snap-back. */
static const patch_domain_t s_drone_domain = {
    PATCH_DOMAIN_CATALOG, .excluded = drone_patch_excluded
};

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
        ESP_LOGI(TAG, "L%u melodic patch -> %u (%s)", (unsigned)li + 1u, (unsigned)applied, name);
    } else {
        ESP_LOGI(TAG, "L%u melodic patch -> %u", (unsigned)li + 1u, (unsigned)applied);
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

    /* PCM engine: the control browses the ROM drum samples instead of the
     * curated SYNTH patch list (which stays stored for the next engine
     * switch). Mirror + name lookup use the PCM preset table. */
    if (sequencer_core_get_drum_engine() == SEQ_DRUM_PCM) {
        uint16_t preset = sequencer_core_cycle_drum_pcm_preset(li, track, dir);
        seq_state.layers[li].track_pcm_preset[track] = preset;

        const char *pcm_name = pcm_preset_name_for(preset);
        if (pcm_name) {
            ESP_LOGI(TAG, "drum PCM preset cycle L%u T%u -> %u (%s)",
                     li + 1u, track + 1u, (unsigned)preset, pcm_name);
        } else {
            ESP_LOGI(TAG, "drum PCM preset cycle L%u T%u -> %u",
                     li + 1u, track + 1u, (unsigned)preset);
        }
        return;
    }

    uint16_t applied = sequencer_core_cycle_drum_patch(li, track, dir);

    /* Keep the UI mirror in sync (track_patch[] drives the label). */
    seq_state.layers[li].track_patch[track] = applied;
    if (track == 0) seq_state.layers[li].patch = applied;

    const char *name = patch_name_for(applied);
    if (name) {
        ESP_LOGI(TAG, "drum patch cycle L%u T%u -> %u (%s)",
                 li + 1u, track + 1u, (unsigned)applied, name);
    } else {
        ESP_LOGI(TAG, "drum patch cycle L%u T%u -> %u",
                 li + 1u, track + 1u, (unsigned)applied);
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

#if CONFIG_SYNTH_WIRELESS
/* The live slot's Source toggle partitions the shared catalog into two browse
 * groups: WAVE = the contiguous raw-wave / bass / wavetable block (257..271,
 * everything that plays like an oscillator you shape yourself), PATCH = the
 * string/FM/additive presets. Both route through the same kind dispatch as
 * ever; only the cycling domain differs. */
static bool live_wave_group_excluded(uint16_t p)
{
    if (p < SEQ_PATCH_WAVE_BASE || p > SEQ_PATCH_WAVETABLE_MAX) return true;
    return sequencer_core_patch_compiled_out(p);
}

static bool live_patch_group_excluded(uint16_t p)
{
    if (p >= SEQ_PATCH_WAVE_BASE && p <= SEQ_PATCH_WAVETABLE_MAX) return true;
    return sequencer_core_patch_compiled_out(p);
}

static const patch_domain_t s_live_wave_domain = {
    PATCH_DOMAIN_CATALOG, .excluded = live_wave_group_excluded
};
static const patch_domain_t s_live_patch_domain = {
    PATCH_DOMAIN_CATALOG, .excluded = live_patch_group_excluded
};

/* Cycle the live-play slot's patch (Wireless menu page's Patch row) within
 * the group selected by the Source toggle. */
void synth_ui_cycle_live_patch(int delta)
{
    if (delta == 0) return;
    int dir = (delta > 0) ? 1 : -1;
    const patch_domain_t *dom = live_play_get_wave_mode() ? &s_live_wave_domain
                                                          : &s_live_patch_domain;
    uint16_t next = patch_domain_step(dom, live_play_get_patch(), dir);
    live_play_set_patch(next);

    const char *name = patch_name_for(next);
    if (name) {
        ESP_LOGI(TAG, "live patch cycle -> %u (%s)", (unsigned)next, name);
    } else {
        ESP_LOGI(TAG, "live patch cycle -> %u", (unsigned)next);
    }
}
#endif

/* Cycle the drone's PATCH-mode preset (hold+turn gesture on the drone screen).
 * Reuses the same browse-mode stepping + name lookup as the others. */
void synth_ui_drone_cycle_patch(int delta)
{
    if (delta == 0) return;
    int dir = (delta > 0) ? 1 : -1;
    uint16_t next = patch_domain_step(&s_drone_domain, drone_get_patch(), dir);
    drone_set_patch(next);

    const char *name = patch_name_for(next);
    if (name) {
        ESP_LOGI(TAG, "drone patch cycle -> %u (%s)", (unsigned)next, name);
    } else {
        ESP_LOGI(TAG, "drone patch cycle -> %u", (unsigned)next);
    }
    s_force_redraw = true;
}

/* Same gesture on the normal drone screen; the two drones share the patch
 * domain (identical exclusion predicate), only the store differs. */
void synth_ui_drone_std_cycle_patch(int delta)
{
    if (delta == 0) return;
    int dir = (delta > 0) ? 1 : -1;
    uint16_t next = patch_domain_step(&s_drone_domain, drone_std_get_patch(), dir);
    drone_std_set_patch(next);

    const char *name = patch_name_for(next);
    if (name) {
        ESP_LOGI(TAG, "drone_std patch cycle -> %u (%s)", (unsigned)next, name);
    } else {
        ESP_LOGI(TAG, "drone_std patch cycle -> %u", (unsigned)next);
    }
    s_force_redraw = true;
}
