#include "custompatches/demo_aurora.h"
#include "sequencer_core.h"
#include "amy_fx.h"
#include "esp_log.h"
#include <stdint.h>

static const char *TAG = "demo_aurora";

/* "Aurora" - an original chill-synth arrangement (see the header). Key A
 * minor, 100 BPM, two-chord loop Am|F (32-step = 2-bar versions of every
 * melodic layer, drums looping a single 16-step bar underneath). */
#define AURORA_BPM       100u
#define AURORA_KICK       0u    /* ROM-bank track roles */
#define AURORA_SNARE      1u
#define AURORA_HAT        2u
#define AURORA_PERC       3u

#define AURORA_BASS_BASE  45u   /* A2 */
#define AURORA_BASS_AMP   0.55f

#define AURORA_PAD_PATCH  5     /* Juno "Brass & Strings": warm pad stabs     */
#define AURORA_PAD_GATE   30
#define AURORA_PAD_AMP    0.45f

#define AURORA_LEAD_BASE  76u   /* E5: melodic hook centre                    */
#define AURORA_LEAD_PATCH SEQ_PATCH_FM_EPIANO
#define AURORA_LEAD_GATE  18    /* short pluck per note                       */
#define AURORA_LEAD_AMP   0.50f

/* ── Drums: 4-on-floor house loop at 100 BPM. Off-beat 8th hats (2,6,10,14)
 * keep it light, plus a subtle perc accent hitting just before the snares
 * (3,11). The velocity accent/humanize engine supplies the feel. */
static void aurora_program_drums(void)
{
    sequencer_core_set_drum_engine(SEQ_DRUM_PCM);
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
            sequencer_core_set_step(0, t, s, false);
        }
    }
    static const uint8_t kick_steps[]  = {  0,  4,  8, 12 };
    static const uint8_t snare_steps[] = {  4, 12 };
    static const uint8_t hat_steps[]   = {  2,  6, 10, 14 };
    static const uint8_t perc_steps[]  = {  3, 11 };
    for (uint8_t i = 0; i < sizeof(kick_steps); i++)
        sequencer_core_set_step(0, AURORA_KICK,  kick_steps[i], true);
    for (uint8_t i = 0; i < sizeof(snare_steps); i++)
        sequencer_core_set_step(0, AURORA_SNARE, snare_steps[i], true);
    for (uint8_t i = 0; i < sizeof(hat_steps); i++)
        sequencer_core_set_step(0, AURORA_HAT,   hat_steps[i], true);
    for (uint8_t i = 0; i < sizeof(perc_steps); i++)
        sequencer_core_set_step(0, AURORA_PERC,  perc_steps[i], true);
}

/* ── Bass: one track, 32-step two-bar walk, offsets off A2.
 *   bar 1 (Am): A A  C3 E3 C3 A      - syncopated tonic figure
 *   bar 2 (F ): F  F  G2 C3 F2 G2(-) - walks up, G leads back to A (turnaround) */
typedef struct { uint8_t s; int8_t ofs; } aurora_hit_t;

static const aurora_hit_t s_bass_hits[] = {
    {  0,  0 }, {  3,  0 }, {  6,  3 }, {  8,  7 },
    { 11,  3 }, { 14,  0 },
    { 16, -4 }, { 19, -4 }, { 22, -2 }, { 24,  3 },
    { 27, -4 }, { 30, -2 },
};

/* ── Pads: three chord voices via fixed track bases + per-step offsets.
 * Voicing A (Am)  = {A3, C4, E4}; voicing F = {F3, A3, C4}. Both derive from
 * the same bases {57,60,64}, so bar2 needs only the offset switch below.
 * One stab per chord, plus an off-beat eighth lift at 12/28 (the classic
 * "and of 4" push before the harmony change). */
static const uint8_t s_pad_base[3] = { 57, 60, 64 };
static const int8_t  s_pad_ofs[2][3] = {
    {  0,  0,  0 },   /* Am  */
    { -4,  0, -7 },   /* F   */
};
static const uint8_t s_pad_steps[4]    = {  0, 12, 16, 28 };
static const uint8_t s_pad_chord[4]    = {  0,  0,  1,  1 };

/* ── Lead: FM E.Piano hook, eighth-note grid with a rest at each bar top.
 * Offsets off E5; phrase: (E5) G5 G5 B5 | B5 A5 G5 | (D5) E5 G5 A5 B5 A5. */
static const aurora_hit_t s_lead_hits[] = {
    {  2,  0 }, {  4,  3 }, {  6,  3 }, {  8,  7 },
    { 10,  7 }, { 12,  5 }, { 14,  3 },
    { 18, -2 }, { 20,  0 }, { 22,  0 }, { 24,  3 },
    { 26,  5 }, { 28,  7 }, { 30,  5 },
};

/* Stop the transport, delete every melodic layer, and clear any stray
 * solo/mute gating. The drum layer (index 0) is never deletable. */
static void aurora_clear_song(void)
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

static void aurora_set_hits(uint8_t layer, uint8_t track,
                            const aurora_hit_t *hits, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sequencer_core_set_step(layer, track, hits[i].s, true);
        sequencer_core_set_step_pitch_ofs(layer, track, hits[i].s,
                                          hits[i].ofs);
    }
}

bool demo_aurora_load(void)
{
    aurora_clear_song();
    aurora_program_drums();

    /* Bass: 32-step (2-bar) single-track layer. */
    uint8_t bass = sequencer_core_add_layer(SEQ_LAYER_MELODIC, 32);
    if (bass >= MAX_LAYERS) {
        ESP_LOGE(TAG, "add_layer failed (bass)");
        return false;
    }
    sequencer_core_set_layer_patch(bass, SEQ_PATCH_BASS_1);
    sequencer_core_set_track_midi_note(bass, 0, AURORA_BASS_BASE);
    sequencer_core_set_melodic_amp_scale(bass, 0, AURORA_BASS_AMP);
    aurora_set_hits(bass, 0, s_bass_hits,
                    sizeof(s_bass_hits) / sizeof(s_bass_hits[0]));

    /* Pads: 32-step, three chord voices. */
    uint8_t pads = sequencer_core_add_layer(SEQ_LAYER_MELODIC, 32);
    if (pads >= MAX_LAYERS) {
        ESP_LOGE(TAG, "add_layer failed (pads)");
        return false;
    }
    sequencer_core_set_layer_patch(pads, AURORA_PAD_PATCH);
    sequencer_core_set_melodic_gate_pct(pads, AURORA_PAD_GATE);
    for (uint8_t v = 0; v < 3; v++) {
        sequencer_core_set_track_midi_note(pads, v, s_pad_base[v]);
        sequencer_core_set_melodic_amp_scale(pads, v, AURORA_PAD_AMP);
        for (uint8_t i = 0; i < 4; i++) {
            uint8_t s = s_pad_steps[i];
            sequencer_core_set_step(pads, v, s, true);
            sequencer_core_set_step_pitch_ofs(pads, v, s,
                                              s_pad_ofs[s_pad_chord[i]][v]);
        }
    }

    /* Lead hook: 32-step, single track. */
    uint8_t lead = sequencer_core_add_layer(SEQ_LAYER_MELODIC, 32);
    if (lead >= MAX_LAYERS) {
        ESP_LOGE(TAG, "add_layer failed (lead)");
        return false;
    }
    sequencer_core_set_layer_patch(lead, AURORA_LEAD_PATCH);
    sequencer_core_set_melodic_gate_pct(lead, AURORA_LEAD_GATE);
    sequencer_core_set_track_midi_note(lead, 0, AURORA_LEAD_BASE);
    sequencer_core_set_melodic_amp_scale(lead, 0, AURORA_LEAD_AMP);
    aurora_set_hits(lead, 0, s_lead_hits,
                    sizeof(s_lead_hits) / sizeof(s_lead_hits[0]));

    /* Space: light reverb + echo on the mix (shared FX cache, so the FX menu
     * shows the loaded values). The Juno pad patch adds its own chorus. */
    s_fx.echo_level   = 22;
    s_fx.echo_delay_ms = 280;   /* dotted-8th feel at 100 BPM */
    s_fx.echo_feedback = 45;
    s_fx.echo_tone     = 0;
    fx_push_echo();
    s_fx.reverb_level   = 45;
    s_fx.reverb_liveness = 70;
    s_fx.reverb_damping  = 40;
    s_fx.reverb_xover_hz = 3000;
    fx_push_reverb();

    sequencer_core_set_bpm(AURORA_BPM);
    sequencer_core_set_playing(true);

    ESP_LOGI(TAG, "loaded: drums + bass + pad stabs + EP hook (Am|F) @ %u BPM",
             (unsigned)AURORA_BPM);
    return true;
}