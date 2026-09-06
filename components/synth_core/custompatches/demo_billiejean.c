#include "custompatches/demo_billiejean.h"
#include "sequencer_core.h"
#include "amy_fx.h"
#include "esp_log.h"
#include <stdint.h>

static const char *TAG = "demo_billiejean";

/* Billie Jean, as retraced from the upstream examples
 * (components/amy/examples/BillieJeanDrumsBass, BillieJeanScheduled):
 * eighth-note bass in G minor pentatonic over a Michael-style 808
 * kick / snare / 16th-hat pattern, with (scheduled) 2-bar Juno pad chords
 * and global reverb. */
#define DEMO_BPM         120u
#define DEMO_SCHED_BPM   116u
#define DEMO_KICK         0u    /* 808 kit track roles from the ROM bank */
#define DEMO_SNARE        1u
#define DEMO_HAT          2u
#define DEMO_BASS_BASE    43u   /* G2: octave-relative centre for the riff */
#define DEMO_BASS_AMP     0.6f

/* Bass is played from ONE track via per-step pitch offsets, so the single
 * track keeps the groove under the drums instead of stacking eight rows. */
static const int8_t s_bass_offsets[8] = {
     0,   /* step 0: G2 */
    -5,   /* step 2: D2 */
    -2,   /* step 4: F2 */
     0,   /* step 6: G2 */
    -2,   /* step 8: F2 */
    -5,   /* step 10: D2 */
    -7,   /* step 12: C2 */
    -5,   /* step 14: D2 */
};

/* Juno A16 "Brass & Strings" (patch 5): a 3-voice pad stroking
 *   Fmin {70,74,79}, Amin {72,76,81}, Bb {74,77,82}, Amin — one stroke per
 * three tracks, again as offsets off fixed base notes 79/81/84 (one track per
 * chord voice). Strokes sit at steps 0/6/16/22 of a 32-step (2-bar) layer. */
#define DEMO_CHORD_PATCH    5
#define DEMO_CHORD_GATE_PCT 15
#define DEMO_CHORD_AMP      0.8f
static const uint8_t s_chord_steps[4] = {  0,  6, 16, 22 };
static const uint8_t s_chord_base[3] = { 79, 81, 84 };
static const int8_t  s_chord_ofs[3][4] = {
    { -9, -7, -5, -7 },
    { -7, -5, -4, -5 },
    { -5, -3, -2, -3 },
};

/* Stop the transport, delete every melodic layer, and clear any stray
 * solo/mute gating from the previous session that would silence the fresh
 * kit. The drum layer (index 0) is never deletable. */
static void demo_clear_song(void)
{
    if (sequencer_core_get_playing()) {
        sequencer_core_set_playing(false);
    }
    while (sequencer_core_get_num_layers() > 1) {
        uint8_t n = sequencer_core_get_num_layers();
        if (!sequencer_core_delete_layer((uint8_t)(n - 1))) break;
    }
    sequencer_core_clear_all_solos();
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        sequencer_core_set_track_mute(0, t, false);
    }
}

/* 808 kit on the drum layer (may already be PCM; re-assert so the seeded
 * roles/notes are the boot bank's tuned ones), unset pattern, kick/snare/hat. */
static void demo_program_kit(void)
{
    sequencer_core_set_drum_engine(SEQ_DRUM_PCM);

    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
            sequencer_core_set_step(0, t, s, false);
        }
    }

    static const uint8_t kick_steps[]  = {  0,  3,  6, 11 };
    static const uint8_t snare_steps[] = {  4, 12 };
    for (uint8_t i = 0; i < sizeof(kick_steps); i++) {
        sequencer_core_set_step(0, DEMO_KICK,  kick_steps[i], true);
    }
    for (uint8_t i = 0; i < sizeof(snare_steps); i++) {
        sequencer_core_set_step(0, DEMO_SNARE, snare_steps[i], true);
    }
    for (uint8_t s = 0; s < SEQ_STEPS; s++) {
        sequencer_core_set_step(0, DEMO_HAT, s, true);   /* steady 16ths */
    }
}

/* Add a melodic layer holding the single-track bass riff. Returns its index,
 * or 0xFF when the layer table is full. */
static uint8_t demo_add_bass(void)
{
    uint8_t bass = sequencer_core_add_layer(SEQ_LAYER_MELODIC, SEQ_STEPS);
    if (bass >= MAX_LAYERS) {
        ESP_LOGE(TAG, "add_layer failed");
        return 0xFF;
    }
    sequencer_core_set_layer_patch(bass, SEQ_PATCH_BASS_1);
    sequencer_core_set_track_midi_note(bass, 0, DEMO_BASS_BASE);
    sequencer_core_set_melodic_amp_scale(bass, 0, DEMO_BASS_AMP);
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t s = (uint8_t)(i * 2u);           /* steps 0,2,...,14 */
        sequencer_core_set_step(bass, 0, s, true);
        sequencer_core_set_step_pitch_ofs(bass, 0, s, s_bass_offsets[i]);
    }
    return bass;
}

bool demo_billiejean_load(void)
{
    demo_clear_song();
    demo_program_kit();
    if (demo_add_bass() >= MAX_LAYERS) {
        return false;
    }

    sequencer_core_set_bpm(DEMO_BPM);
    sequencer_core_set_playing(true);

    ESP_LOGI(TAG, "loaded: 808 kick/snare/hat + G-minor bass @ %u BPM",
             (unsigned)DEMO_BPM);
    return true;
}

bool demo_billiejean_scheduled_load(void)
{
    demo_clear_song();
    demo_program_kit();
    if (demo_add_bass() >= MAX_LAYERS) {
        return false;
    }

    /* 2-bar Juno pad layer: three chord voices, one per track, on a short
     * gate so each stroke is a stab rather than a pad drone. */
    uint8_t chords = sequencer_core_add_layer(SEQ_LAYER_MELODIC, 32);
    if (chords >= MAX_LAYERS) {
        ESP_LOGE(TAG, "add_layer failed");
        return false;
    }
    sequencer_core_set_layer_patch(chords, DEMO_CHORD_PATCH);
    sequencer_core_set_melodic_gate_pct(chords, DEMO_CHORD_GATE_PCT);
    for (uint8_t v = 0; v < 3; v++) {
        sequencer_core_set_track_midi_note(chords, v, s_chord_base[v]);
        sequencer_core_set_melodic_amp_scale(chords, v, DEMO_CHORD_AMP);
        for (uint8_t i = 0; i < 4; i++) {
            uint8_t s = s_chord_steps[i];
            sequencer_core_set_step(chords, v, s, true);
            sequencer_core_set_step_pitch_ofs(chords, v, s, s_chord_ofs[v][i]);
        }
    }

    /* Global reverb on the mix, matching BillieJeanScheduled.ino
     * (0.5 / 0.85 / 0.5 / 3 kHz), going through the shared FX cache so the
     * FX menu shows the loaded values. */
    s_fx.reverb_level   = 50;
    s_fx.reverb_liveness = 85;
    s_fx.reverb_damping  = 50;
    s_fx.reverb_xover_hz = 3000;
    fx_push_reverb();

    sequencer_core_set_bpm(DEMO_SCHED_BPM);
    sequencer_core_set_playing(true);

    ESP_LOGI(TAG, "loaded: kit + bass + 2-bar Juno pad chords, reverb @ %u BPM",
             (unsigned)DEMO_SCHED_BPM);
    return true;
}