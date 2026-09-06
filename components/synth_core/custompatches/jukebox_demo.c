#include "custompatches/jukebox_demo.h"
#include "sequencer_core.h"
#include "amy_fx.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "jukebox";

/* ── Faithful port of Copych's "Endless Acid Banger" (AcidBox/AciduinoBox
 *    AcidBanger.ino, itself adapted from vitling.xyz/toys/acid-banger).
 *
 *    The banger is a self-playing acid machine: 2 monophonic synths + a 6-hit
 *    drum kit play 16-step patterns that mostly STAY PUT; the groove comes
 *    from quirkily-voiced note sets (chromatic color tones, not pentatonic),
 *    a beat-synced melody density (90/80/50/10 per step-class), slow knob
 *    ramps (triangle sweeps of level/gate) and pre-scheduled breaks that end
 *    on a crash downbeat. The whole thing churns bars without re-rolling the
 *    tune - that stability is the acid.
 *
 *    Box mapping: layer 0 = 808 kit (kick/snare/hat on t0/t1/t2, perc on t3,
 *    6 banger hits folded to 4 tracks), layer 1 = acid bass (BASS_1 saw-ish),
 *    layer 2 = square lead (PULSE). Only 3 of MAX_LAYERS are used. */
#define JK_DRUM   0u
#define JK_SYN1   1u   /* acid bass (banger synth1) */
#define JK_SYN2   2u   /* square lead (banger synth2) */
#define JK_BPM    130u /* AcidBox config.h default */

#define JK_BASS_PATCH  SEQ_PATCH_BASS_1
#define JK_LEAD_PATCH  SEQ_PATCH_PULSE
#define JK_BASS_GATE   32
#define JK_BASS_AMP    0.85f
#define JK_LEAD_GATE   12
#define JK_LEAD_AMP    0.34f

#define JK_PAT_LEN   16
#define JK_MAX_NOTE  16
#define JK_NUM_RAMPS 4

enum {
    JK_VOICE_BASS = 0,
    JK_VOICE_LEAD = 1,
};

enum {
    JK_DRUM_STRAIGHT = 0,
    JK_DRUM_HANG,
    JK_DRUM_BREAK,
};

enum {
    JK_PBASS_AMP = 0,
    JK_PBASS_GATE,
    JK_PLEAD_AMP,
    JK_PLEAD_GATE,
};

/* ── Note sets, straight from AcidBanger: root + quirky offsets. -1 ends. ─ */
static const int8_t s_offset_tables[][JK_MAX_NOTE + 1] = {
    { 0, 0, 12, 24, 27, -1 },
    { 0, 0, 0, 12, 10, 19, 26, 27, -1 },
    { 0, 0, 0, 1, 7, 10, 12, 13, -1 },
    { 0, -1 },
    { 0, 0, 0, 0, 0, 0, 1, 13, 25, -1 },
    { 0, 0, 0, 12, 24, -1 },
    { 0, 0, 12, 12, 18, 24, 24, -1 },
    { 0, 0, 7, 14, 24, 24, -1 },
    { 0, 0, 12, 14, 15, 19, -1 },
    { 0, 0, 0, 12, 12, 13, 16, 19, 22, 24, 25, -1 },
    { 0, 0, 0, 7, 12, 15, 17, 20, 24, -1 },
};
#define JK_OFFSET_COUNT ((uint8_t)(sizeof(s_offset_tables) / sizeof(s_offset_tables[0])))

/* ── Generator state (UI task only; no locking needed) ─────────────────── */
static bool     s_active;
static uint16_t s_seed;

static uint8_t  s_root;              /* current note-set root (28..42) */
static uint8_t  s_note_set[JK_MAX_NOTE];
static uint8_t  s_num_notes;

static uint8_t  s_pat[JK_PAT_LEN];        /* melody note numbers (0 = rest) */
static uint8_t  s_kick[JK_PAT_LEN], s_snare[JK_PAT_LEN],
                s_ch[JK_PAT_LEN], s_oh[JK_PAT_LEN], s_perc[JK_PAT_LEN];
static bool     s_crash;                  /* crash on this bar's downbeat */

static uint32_t s_bar;                    /* absolute pattern counter */
static bool     s_break_status;
static uint8_t  s_break_len;
static uint32_t s_break_start, s_break_after;

static bool     s_dirty;                  /* patterns rewritten this call */
static uint8_t  s_last_step;              /* last polled drum step (UI task) */

/* ── Knob ramps (the banger's MIDI ramps, mapped to our 4 live params) ─── */
typedef struct {
    uint8_t param;
    float   value, min, max, def, step;
    int16_t left_bars;
} jk_ramp_t;

static jk_ramp_t s_ramps[JK_NUM_RAMPS];

/* ── LFSR with restorable state, exactly the banger's. ─────────────────── */
static inline uint16_t lfsr16_next(uint16_t x)
{
    uint16_t y = (uint16_t)(x >> 1);
    if (x & 1u) y ^= 0xB400u;
    return y;
}

static uint16_t myrandom(void)
{
    s_seed = lfsr16_next(s_seed);
    return s_seed;
}

static uint16_t myrandom_max(uint16_t max)
{
    if (max == 0) return 0;
    return (uint16_t)(myrandom() % max);
}

static bool flip(uint8_t percent)
{
    return myrandom_max(100) < percent;
}

/* ── Core step writers ─────────────────────────────────────────────────── */
static void cells_clear(uint8_t layer)
{
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        for (uint8_t s = 0; s < JK_PAT_LEN; s++) {
            sequencer_core_set_step(layer, t, s, false);
        }
    }
}

/* Folds +-JP_... beyond +-SEQ_STEP_PITCH_OFS_MAX down an octave so the
 * chromatic climbs (offsets up to 27) still fit the 24-semitone clamp. */
static int8_t ofs_fold(int16_t ofs)
{
    while (ofs > SEQ_STEP_PITCH_OFS_MAX) ofs -= 12;
    while (ofs < -SEQ_STEP_PITCH_OFS_MAX) ofs += 12;
    return (int8_t)ofs;
}

static void commit_drums(void)
{
    for (uint8_t s = 0; s < JK_PAT_LEN; s++) {
        sequencer_core_set_step(JK_DRUM, 0, s, s_kick[s] != 0);
        sequencer_core_set_step(JK_DRUM, 1, s, s_snare[s] != 0);
        sequencer_core_set_step(JK_DRUM, 2, s, (s_ch[s] | s_oh[s]) != 0);
        sequencer_core_set_step(JK_DRUM, 3, s, s_perc[s] != 0);
    }
    if (s_crash) {
        sequencer_core_set_step(JK_DRUM, 3, 0, true);
        s_crash = false;
    }
}

static void commit_melody(uint8_t voice)
{
    uint8_t layer = (voice == JK_VOICE_BASS) ? JK_SYN1 : JK_SYN2;
    int16_t base  = (voice == JK_VOICE_BASS) ? s_root : (int16_t)(s_root + 12);
    for (uint8_t s = 0; s < JK_PAT_LEN; s++) {
        uint8_t note = s_pat[s];
        if (note == 0) {
            sequencer_core_set_step(layer, 0, s, false);
        } else {
            sequencer_core_set_step(layer, 0, s, true);
            sequencer_core_set_step_pitch_ofs(layer, 0, s,
                                              ofs_fold((int16_t)note - base));
        }
    }
}

/* ── Note sets & melodies (AcidBanger port) ────────────────────────────── */
static uint8_t gen_note_set(void)
{
    uint8_t root = (uint8_t)(myrandom_max(15) + 28);
    uint8_t set  = (uint8_t)myrandom_max(JK_OFFSET_COUNT);
    uint8_t n    = 0;
    while (n < JK_MAX_NOTE && s_offset_tables[set][n] >= 0) {
        s_note_set[n] = (uint8_t)(root + s_offset_tables[set][n]);
        n++;
    }
    s_root = root;
    s_num_notes = n;
    return n;
}

static void gen_melody(uint8_t voice)
{
    uint8_t chance;
    for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
        chance = (uint8_t)(255u * (i % 4u == 0 ? 90u :
                            (i % 3u == 0 ? 80u :
                            (i % 2u == 0 ? 50u : 10u))) >> 8);
        if (flip(chance)) {
            s_pat[i] = s_note_set[myrandom_max(s_num_notes)];
        } else {
            s_pat[i] = 0;
        }
    }
    commit_melody(voice);
}

/* ── Drums (all six virtual banger hits, folded to four tracks) ────────── */
static void gen_drums(uint8_t kind)
{
    uint8_t kick_mode = 0, hat_mode = 0, snare_mode = 0, perc_mode = 0;
    uint8_t rnd;
    memset(s_kick, 0, JK_PAT_LEN);
    memset(s_snare, 0, JK_PAT_LEN);
    memset(s_oh, 0, JK_PAT_LEN);
    memset(s_ch, 0, JK_PAT_LEN);
    memset(s_perc, 0, JK_PAT_LEN);

    switch (kind) {
    case JK_DRUM_BREAK:
        rnd = (uint8_t)myrandom_max(100);
        kick_mode  = (rnd < 10) ? 2 : (rnd < 60 ? 1 : 3);   /* bigbeat/4floor/none */
        rnd = (uint8_t)myrandom_max(100);
        snare_mode = (rnd < 40) ? 2 : (rnd < 80 ? 3 : 0);   /* fill/break/backbeat */
        hat_mode  = (uint8_t)myrandom_max(4);
        perc_mode = (uint8_t)myrandom_max(5);
        break;
    case JK_DRUM_HANG:
        kick_mode  = (myrandom_max(100) < 50) ? 2 : 3;
        snare_mode = (myrandom_max(100) < 60) ? 4 : 0;
        hat_mode  = (uint8_t)myrandom_max(4);
        perc_mode = (uint8_t)myrandom_max(5);
        break;
    case JK_DRUM_STRAIGHT:
    default:
        kick_mode  = (myrandom_max(100) < 20) ? 2 : 1;
        snare_mode = (myrandom_max(100) < 60) ? 4 : 0;
        hat_mode  = flip(70) ? 1 : (uint8_t)myrandom_max(4);   /* pop/any */
        perc_mode = (uint8_t)myrandom_max(5);
        break;
    }

    if (kick_mode == 1) {                       /* four-floor */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if (i % 4u == 0) s_kick[i] = 120;
        }
    } else if (kick_mode == 2) {                /* bigbeat */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if (i == 0) s_kick[i] = 127;
            else if (i == 14 && flip(20)) s_kick[i] = (uint8_t)myrandom_max(80);
        }
    }

    if (snare_mode == 0) {                      /* backbeat: 1 & 3 (t0,t8) */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if (i % 8u == 0) s_snare[i] = 110;
        }
    } else if (snare_mode == 2) {               /* fill: all 16 */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) s_snare[i] = 120;
    } else if (snare_mode == 3) {               /* break: patterned roll */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if (i == 2 || i == 8) continue;
            if (i == 4 || i == 5 || i == 6 || i == 10)
                s_snare[i] = (uint8_t)myrandom_max(100);
            else
                s_snare[i] = 120;
        }
    } else if (snare_mode == 4) {               /* straight: 2 & 4 (t4,t12) */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if (i % 8u == 4) s_snare[i] = 80;
        }
    }

    if (hat_mode == 0) {                        /* offbeats */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if (i % 4u == 2) s_oh[i] = 50;
            else if (flip(30)) {
                if (flip(50)) s_ch[i] = (uint8_t)myrandom_max(25);
                else s_oh[i] = (uint8_t)myrandom_max(25);
            }
        }
    } else if (hat_mode == 1) {                 /* pop: OH offbeats + CH all */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if (i % 4u == 2) s_oh[i] = 60;
            else s_ch[i] = (uint8_t)(40u + myrandom_max(40));
        }
    } else if (hat_mode == 2) {                 /* closed 8ths */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if (i % 2u == 0) s_ch[i] = 50;
            else if (flip(50)) s_ch[i] = (uint8_t)myrandom_max(40);
        }
    } else {                                    /* pat1: sparse */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if (i % 8u != 1 && i % 8u != 4 && i % 8u != 7) s_ch[i] = 80;
        }
    }

    if (perc_mode == 0) {                       /* filler: fill empty steps */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if (!s_oh[i] && !s_ch[i] && !s_kick[i] && !s_snare[i])
                s_perc[i] = (uint8_t)(50u + myrandom_max(37));
        }
    } else if (perc_mode == 1) {                /* xor(kick, snare) */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if ((s_kick[i] == 0) ^ (s_snare[i] == 0))
                s_perc[i] = (uint8_t)(50u + myrandom_max(37));
        }
    } else if (perc_mode == 2) {                /* xor(oh, ch) */
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            if ((s_oh[i] == 0) ^ (s_ch[i] == 0))
                s_perc[i] = (uint8_t)(50u + myrandom_max(37));
        }
    } else if (perc_mode == 3) {                /* echo of hat hits */
        uint8_t dist = (uint8_t)(1u + myrandom_max(7));
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            uint8_t prev = (uint8_t)((i + JK_PAT_LEN - dist) % JK_PAT_LEN);
            if (s_ch[prev] || s_oh[prev])
                s_perc[i] = (uint8_t)(50u + myrandom_max(37));
        }
    } else if (perc_mode == 4) {                /* rolls: accel. line */
        uint8_t roll = 0, roll_vol = 0;
        for (uint8_t i = 0; i < JK_PAT_LEN; i++) {
            bool do_roll = false;
            if (i % 8u == 3 && flip(40)) do_roll = true;
            else if (i % 2u == 0 && flip(20)) do_roll = true;
            else if (flip(10)) do_roll = true;
            if (do_roll) {
                roll_vol = (uint8_t)(50u + myrandom_max(37));
                roll = 4;
            }
            if (roll > 0) {
                s_perc[i] = roll_vol;
                roll_vol /= 2;
                roll--;
            }
        }
    }

    commit_drums();
}

/* ── Knob ramps ────────────────────────────────────────────────────────── */
static void ramp_apply(const jk_ramp_t *r)
{
    switch (r->param) {
    case JK_PBASS_AMP:  sequencer_core_set_melodic_amp_scale(JK_SYN1, 0, r->value); break;
    case JK_PBASS_GATE: sequencer_core_set_melodic_gate_pct(JK_SYN1, (uint8_t)r->value); break;
    case JK_PLEAD_AMP:  sequencer_core_set_melodic_amp_scale(JK_SYN2, 0, r->value); break;
    case JK_PLEAD_GATE: sequencer_core_set_melodic_gate_pct(JK_SYN2, (uint8_t)r->value); break;
    }
}

static void ramp_pick(jk_ramp_t *r)
{
    float spread = (r->max - r->min) * 0.08f;
    r->step = (float)((int16_t)(myrandom_max(200) - 100)) * 0.01f * spread;
    if (r->step < 0) r->step = -r->step;
    if (r->step < spread * 0.05f) r->step = spread * 0.05f;
    if (flip(50)) r->step = -r->step;
    r->left_bars = (int16_t)(2u + 2u * myrandom_max(2));  /* 2 or 4 bars */
    ramp_apply(r);
}

static void ramp_sweep(void)
{
    for (uint8_t i = 0; i < JK_NUM_RAMPS; i++) {
        jk_ramp_t *r = &s_ramps[i];
        r->value = r->min;
        r->step  = (r->max - r->min) / (float)(s_break_len * JK_PAT_LEN);
        r->left_bars = s_break_len;
        ramp_apply(r);
    }
}

static void ramps_check(bool force)
{
    for (uint8_t i = 0; i < JK_NUM_RAMPS; i++) {
        jk_ramp_t *r = &s_ramps[i];
        if (force || r->left_bars <= 0) {
            r->value = r->def;
            ramp_apply(r);
            ramp_pick(r);
        } else if (--r->left_bars == 0) {
            r->value = r->def;
            ramp_apply(r);
            ramp_pick(r);
        }
    }
}

static void ramps_tick_16th(void)
{
    for (uint8_t i = 0; i < JK_NUM_RAMPS; i++) {
        jk_ramp_t *r = &s_ramps[i];
        r->value += r->step;
        if (r->value >= r->max) { r->value = r->max; r->step = -r->step; }
        else if (r->value <= r->min) { r->value = r->min; r->step = -r->step; }
        ramp_apply(r);
    }
}

/* ── Break scheduler (AcidBanger decide_on_break port) ─────────────────── */
static void decide_on_break(void)
{
    uint32_t bars_played = s_bar - s_break_after;

    if (!s_break_status) {
        if (bars_played == 28) {
            s_break_status = true; s_break_len = 4;
        } else if (bars_played == 15) {
            if (flip(20)) { s_break_status = true; s_break_len = 1; }
        } else if (bars_played == 14) {
            if (flip(15)) { s_break_status = true; s_break_len = 2; }
        } else if (bars_played == 13) {
            if (flip(15)) { s_break_status = true; s_break_len = 3; }
        } else if (bars_played == 12) {
            if (flip(15)) { s_break_status = true; s_break_len = 4; }
        } else if (bars_played == 7) {
            if (flip(15)) { s_break_status = true; s_break_len = 1; }
        } else if (bars_played == 6) {
            if (flip(15)) { s_break_status = true; s_break_len = 2; }
        } else if (bars_played == 3) {
            if (flip(10)) { s_break_status = true; s_break_len = 1; }
        }
        if (s_break_status) {
            s_break_start = s_bar;
            s_break_after = s_bar + s_break_len;
            gen_drums(JK_DRUM_BREAK);
            s_dirty = true;
        }
    } else if (s_break_after == s_bar) {
        /* break over: back to the groove + quarter-parallel re-rolls.
         * Order mirrors AcidBanger: note set, then drums, then the two
         * synths on their own probabilities. */
        s_break_status = false;
        if (flip(15)) {
            gen_note_set();
            sequencer_core_set_track_midi_note(JK_SYN1, 0, s_root);
            sequencer_core_set_track_midi_note(JK_SYN2, 0, (uint8_t)(s_root + 12));
            gen_melody(JK_VOICE_BASS);
            gen_melody(JK_VOICE_LEAD);
        }
        gen_drums(flip(10) ? JK_DRUM_HANG : JK_DRUM_STRAIGHT);
        if (flip(80)) gen_melody(JK_VOICE_BASS);
        if (flip(60)) gen_melody(JK_VOICE_LEAD);
        s_dirty = true;
    }
}

/* ── One 16-step bar boundary ──────────────────────────────────────────── */
static void on_bar(void)
{
    s_bar++;
    decide_on_break();

    if (s_break_status) {
        if (s_break_start == s_bar) ramp_sweep();   /* riser across the fill */
    } else if (s_break_after == s_bar) {
        s_crash = true;                             /* crash lands the break */
        commit_drums();
        ramps_check(true);
        s_dirty = true;
    } else {
        ramps_check(false);
    }
}

/* ── Public demo entry ─────────────────────────────────────────────────── */
bool jukebox_demo_load(void)
{
    jukebox_demo_deactivate();

    if (sequencer_core_get_playing()) sequencer_core_set_playing(false);
    while (sequencer_core_get_num_layers() > 1) {
        uint8_t n = sequencer_core_get_num_layers();
        if (!sequencer_core_delete_layer((uint8_t)(n - 1))) break;
    }
    sequencer_core_clear_all_solos();
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        sequencer_core_set_track_mute(0, t, false);
    }

    sequencer_core_set_drum_engine(SEQ_DRUM_PCM);
    cells_clear(JK_DRUM);

    if (sequencer_core_add_layer(SEQ_LAYER_MELODIC, JK_PAT_LEN) >= MAX_LAYERS) {
        ESP_LOGE(TAG, "add bass failed");
        return false;
    }
    sequencer_core_set_layer_patch(JK_SYN1, JK_BASS_PATCH);
    sequencer_core_set_melodic_gate_pct(JK_SYN1, JK_BASS_GATE);
    sequencer_core_set_melodic_amp_scale(JK_SYN1, 0, JK_BASS_AMP);

    if (sequencer_core_add_layer(SEQ_LAYER_MELODIC, JK_PAT_LEN) >= MAX_LAYERS) {
        ESP_LOGE(TAG, "add lead failed");
        return false;
    }
    sequencer_core_set_layer_patch(JK_SYN2, JK_LEAD_PATCH);
    sequencer_core_set_melodic_gate_pct(JK_SYN2, JK_LEAD_GATE);
    sequencer_core_set_melodic_amp_scale(JK_SYN2, 0, JK_LEAD_AMP);

    /* tiny room ambience, way lighter than the previous mix */
    s_fx.echo_level    = 10;
    s_fx.echo_delay_ms = 240;
    s_fx.echo_feedback = 25;
    s_fx.echo_tone     = 80;
    fx_push_echo();
    s_fx.reverb_level   = 15;
    s_fx.reverb_liveness = 60;
    s_fx.reverb_damping  = 45;
    s_fx.reverb_xover_hz = 3000;
    fx_push_reverb();

    s_seed = (uint16_t)esp_random();
    if (s_seed == 0u) s_seed = 0xAC1Du;

    s_bar = 0;
    s_break_status = false;
    s_break_after = 0;
    s_crash = false;
    s_dirty = true;
    s_last_step = 0;

    gen_note_set();
    sequencer_core_set_track_midi_note(JK_SYN1, 0, s_root);
    sequencer_core_set_track_midi_note(JK_SYN2, 0, (uint8_t)(s_root + 12));
    gen_melody(JK_VOICE_BASS);
    gen_melody(JK_VOICE_LEAD);
    gen_drums(JK_DRUM_STRAIGHT);

    static const jk_ramp_t init_ramps[JK_NUM_RAMPS] = {
        { .param = JK_PBASS_AMP,  .min = 0.6f,  .max = 1.0f, .def = JK_BASS_AMP },
        { .param = JK_PBASS_GATE, .min = 8.0f,  .max = 55.0f, .def = JK_BASS_GATE },
        { .param = JK_PLEAD_AMP,  .min = 0.12f, .max = 0.6f, .def = JK_LEAD_AMP },
        { .param = JK_PLEAD_GATE, .min = 4.0f,  .max = 30.0f, .def = JK_LEAD_GATE },
    };
    memcpy(s_ramps, init_ramps, sizeof(s_ramps));
    ramps_check(true);

    s_active = true;
    sequencer_core_set_bpm(JK_BPM);
    sequencer_core_set_playing(true);

    ESP_LOGI(TAG, "Endless Acid Banger on @ %u BPM (seed %u)",
             (unsigned)JK_BPM, (unsigned)s_seed);
    return true;
}

bool jukebox_demo_is_active(void)
{
    return s_active;
}

void jukebox_demo_deactivate(void)
{
    s_active = false;
}

bool jukebox_demo_service(void)
{
    if (!s_active) return false;
    if (!sequencer_core_get_playing()) {
        jukebox_demo_deactivate();
        return false;
    }

    uint8_t st = sequencer_core_get_current_step(JK_DRUM);
    if (st == 0u && s_last_step != 0u) {
        on_bar();
        s_last_step = 0u;
        if (s_dirty) {
            s_dirty = false;
            return true;
        }
        ramps_tick_16th();
        return false;
    }
    if (st != s_last_step) {
        s_last_step = st;
        ramps_tick_16th();
    }
    return false;
}