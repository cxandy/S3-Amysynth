#include "arp_core.h"
#include "sequencer_core.h"
#include "custompatches/fm_voice.h"  /* s_fm_voice + fm_voice_push_live (FM_CUSTOM) */
#include "amy_helpers.h"   /* amy_helpers_event_begin/send — for WAVE mode osc config */
#include "voice_config.h"  /* canonical LFO depth scalars + shared wave map */
#include "quantizer.h"
#include "seq_clamp.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

/* DEBUG: bisect heap corruption inside arp init. Gated by
 * CONFIG_AMYSYNTH_HEAP_CHECK; compiles to nothing when off (default). */
#if CONFIG_AMYSYNTH_HEAP_CHECK
#define ARP_HEAP_CHECK(where) do { \
    if (!heap_caps_check_integrity_all(true)) { \
        ESP_LOGE(TAG, "HEAP CORRUPT detected at: %s", where); \
    } else { \
        ESP_LOGI(TAG, "HEAP OK at: %s", where); \
    } \
} while (0)
#else
#define ARP_HEAP_CHECK(where) do { (void)(where); } while (0)
#endif

#ifndef CONFIG_SEQ_ARP_DEFAULT_ENABLED
#define CONFIG_SEQ_ARP_DEFAULT_ENABLED 0
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_SCALE
#define CONFIG_SEQ_ARP_DEFAULT_SCALE 1   /* Major */
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_ROOT_NOTE
#define CONFIG_SEQ_ARP_DEFAULT_ROOT_NOTE 60  /* C4 */
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_GATE_PCT
#define CONFIG_SEQ_ARP_DEFAULT_GATE_PCT 75
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_OCTAVES
#define CONFIG_SEQ_ARP_DEFAULT_OCTAVES 1
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_PATCH
#define CONFIG_SEQ_ARP_DEFAULT_PATCH 138  /* DX7 E.Piano, matches melodic default */
#endif

/* Default AMY waveform for WAVE mode.  SAW_DOWN matches the drone's default and
 * gives a bright, harmonically rich sound that sits well in an arp context.
 * (SAW_DOWN is defined in amy.h as 2.) */
#define ARP_DEFAULT_WAVE SAW_DOWN

/* Octaves of filter-cutoff sweep EG1 contributes in WAVE mode once the arp's
 * filter is authored+enabled (see arp_configure_wave_synth()). Fixed, not
 * user-editable — the arp exposes the EG1 *timing* (attack/decay/sustain/
 * release), not its modulation depth. */
#define ARP_FILTER_EG1_DEPTH_OCT 3.0f

static const char *TAG = "arp_core";

/* AMY_SEQUENCER_PPQ = 48 → 1/16 = 12 ticks. */
static const uint32_t s_rate_ticks[ARP_RATE_COUNT] = {
    [ARP_RATE_1_1]  = 192,
    [ARP_RATE_1_4]  = 48,
    [ARP_RATE_1_8]  = 24,
    [ARP_RATE_1_16] = 12,
    [ARP_RATE_1_32] = 6,
    [ARP_RATE_1_4T]   = 32,
    [ARP_RATE_1_8T]   = 16,
    [ARP_RATE_1_16T]  = 8,
    [ARP_RATE_1_32T]  = 4,
};

static const char *s_rate_names[ARP_RATE_COUNT] = {
    [ARP_RATE_1_1]  = "1/1",
    [ARP_RATE_1_4]  = "1/4",
    [ARP_RATE_1_8]  = "1/8",
    [ARP_RATE_1_16] = "1/16",
    [ARP_RATE_1_32] = "1/32",
    [ARP_RATE_1_4T]   = "1/4T",
    [ARP_RATE_1_8T]   = "1/8T",
    [ARP_RATE_1_16T]  = "1/16T",
    [ARP_RATE_1_32T]  = "1/32T",
};

typedef struct {
    bool       enabled;
    arp_dir_t  dir;
    uint8_t    octaves;       /* 1..ARP_OCT_MAX */
    arp_rate_t rate;
    uint8_t    gate_pct;      /* 10..100 */
    int16_t    slots[ARP_MAX_SLOTS];  /* raw chromatic MIDI, -1 = empty */
    uint8_t    scale_index;
    uint8_t    root_note;
    uint16_t     patch;
    arp_source_t source;         /* WAVE or PATCH (default PATCH)               */
    uint16_t     wave;           /* AMY waveform used when source==ARP_SRC_WAVE */
    seq_env_t    env;            /* runtime-editable ADSR (shared graph editor) */
    bool         env_authored;   /* true once the user commits a custom env      */
    seq_env_t    env1;           /* second envelope (EG1) — see arp_core.h        */
    bool         env1_authored;  /* true once the user commits a custom EG1       */
    seq_filter_t filter;         /* runtime-editable filter (shared filter editor) */
    bool         filter_authored;/* true once the user commits a custom filter    */
    seq_lfo_t    lfo;            /* AMY native LFO — WAVE mode only               */
    bool         lfo_authored;   /* true once the user commits an LFO             */
    float        amp_scale;      /* per-target amplitude trim (default 1.0, 0..1);
                                    scaled into note velocity at emit time. Set via
                                    the graph editor amp mode (MY_BUTTON_2). MUST be
                                    initialised to 1.0f in arp_core_init — memset
                                    zeroes it. */
    uint16_t     portamento_ms;  /* glide time between note pitches, 0=off. Default
                                    0 from memset matches AMY's own reset value, so
                                    no explicit init needed in arp_core_init. */
} arp_state_t;

static arp_state_t s_arp;

/* Refresh coalescing: setters mark the arp dirty instead of re-emitting the
 * whole sequence inline. arp_core_service() (called once per UI frame) performs
 * a single re-emit when dirty, so a fast encoder spin through octaves/rate/gate
 * collapses many setter calls into at most one full re-emit per frame instead of
 * one per detent. Each re-emit clears + re-schedules up to ARP_MAX_STEPS events,
 * every one through the shared AMY event mutex, so this removes a real burst of
 * mutex traffic from Core 0 during edits. */
static volatile bool s_arp_dirty = false;

static inline void arp_mark_dirty(void) { s_arp_dirty = true; }

/* Max distinct arp steps we can schedule = slots * max octaves. Each step uses
 * two tags (on + on+1); we space step tags by 2. */
#define ARP_MAX_STEPS (ARP_MAX_SLOTS * ARP_OCT_MAX)

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Shared ascending bubble sort for collect helpers. */
static void arp_sort_asc(uint8_t *out, uint8_t n)
{
    for (uint8_t i = 0; i + 1 < n; i++)
        for (uint8_t j = 0; j + 1 < n - i; j++)
            if (out[j] > out[j + 1]) { uint8_t t = out[j]; out[j] = out[j+1]; out[j+1] = t; }
}

/* UP mode: REST is a transparent skip — all non-empty notes are collected.
 * Returns sorted ascending; caller uses sorted[note_idx] directly. */
static uint8_t arp_collect_up(uint8_t out[ARP_MAX_SLOTS])
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++) {
        if (s_arp.slots[i] == -1) break;          /* empty sentinel: stop */
        if (s_arp.slots[i] == ARP_REST) continue; /* REST in UP: skip */
        out[n++] = (uint8_t)s_arp.slots[i];
    }
    arp_sort_asc(out, n);
    return n;
}

/* DOWN mode: identical skip-REST logic to UP; caller reverses with
 * sorted[count-1-note_idx]. */
static uint8_t arp_collect_down(uint8_t out[ARP_MAX_SLOTS])
{
    return arp_collect_up(out);
}

/* Snap a chromatic note to the arp's own scale/root (independent quantizer). */
static uint8_t arp_snap(uint8_t chromatic)
{
    const musical_scale_t *scale = quantizer_get_scale(s_arp.scale_index);
    uint8_t snapped = quantizer_snap_midi_note(chromatic, s_arp.root_note, scale);
    return sequencer_core_clamp_melodic_note((int32_t)snapped);
}

/* Clear every possible arp tag slot (removes all scheduled repeating events). */
void arp_core_clear_all(void)
{
    uint32_t base = sequencer_core_arp_tag_base();
    for (uint8_t i = 0; i < ARP_MAX_STEPS; i++) {
        sequencer_core_arp_clear_note(base + (uint32_t)i * 2u);
    }
}

/* ── Source configuration helpers ────────────────────────────────────── */

/* Map lfo_wave_t → AMY wave constant (shared map in voice_config.c). */
#define arp_lfo_wave_to_amy voice_lfo_wave_to_amy

/* Configure the arp's AMY synth slot as a bare oscillator (WAVE mode).
 *
 * One osc per voice — no patch, no LFO carrier.  Pitch follows the MIDI note
 * delivered by sequencer_core_arp_emit_note() (COEF_NOTE=1).  Amplitude is
 * velocity-scaled + EG0-gated so the shared ADSR editor and custom envelopes
 * work exactly as they do in PATCH mode.
 *
 * The caller is responsible for pushing an EG0 envelope afterwards (arp_rebuild
 * always does this in WAVE mode so even the default env is applied). */
static void arp_configure_wave_synth(void)
{
    uint8_t synth  = sequencer_core_arp_synth();
    uint8_t voices = sequencer_core_arp_voices();

    /* Shared skeleton: pool (2 oscs/voice — osc 1 is the AMY-native LFO
     * carrier, dormant when no LFO is authored; always allocating it avoids a
     * pool reset when the LFO is toggled later) + osc0 note-following carrier
     * with velocity + EG0 amplitude. */
    voice_wave_cfg_t cfg = {
        .synth                = synth,
        .num_voices           = voices,
        .oscs_per_voice       = 2,
        .wave                 = s_arp.wave,
        .osc0_amp_const       = 1.0f,
        .osc0_amp_vel         = 1.0f,
        .ks_feedback_authored = s_arp.filter_authored,
        .ks_feedback_q        = s_arp.filter.resonance,
        .wt_preset            = -1,
    };
    voice_build_wave(&cfg);

    /* osc 0 arp specializations, layered on the skeleton:
     * — native LFO coupling: mod_source=1 (voice-local — AMY adds base_osc
     *   offset, so it resolves to osc 1 within each voice) plus the COEF_MOD
     *   depth for the chosen target;
     * — plucky amp (EG0) / slower independent filter sweep (EG1): only wired
     *   once the user has authored+enabled a filter, so a stock arp voice with
     *   no filter is unaffected. arp_rebuild() pushes valid EG1 breakpoints
     *   alongside this so the coef never reads a stuck 1.0 (AMY treats a
     *   never-configured breakpoint set as a permanent unity gate). */
    bool lfo_on          = s_arp.lfo_authored && s_arp.lfo.enabled;
    bool wave_filter_eg1 = s_arp.filter_authored && s_arp.filter.enabled;
    amy_event *e;
    if (lfo_on || wave_filter_eg1) {
        e = amy_helpers_event_begin();
        e->synth = synth;
        e->osc   = 0;
        if (lfo_on) {
            e->mod_source = 1;
            /* Clear every target's mod coef before selecting one: re-sending
             * the same voice count does NOT reset the osc pool, so a prior
             * target's COEF_MOD would otherwise persist and keep modulating.
             * The stale-AMP case is the worst: amp COEF_MOD rides AMY's convex
             * dB combine path, so a leftover coef ramps the output to a rail. */
            e->filter_freq_coefs[COEF_MOD] = 0.0f;
            e->amp_coefs[COEF_MOD]         = 0.0f;
            e->freq_coefs[COEF_MOD]        = 0.0f;
            e->duty_coefs[COEF_MOD]        = 0.0f;
            float d = s_arp.lfo.depth / 100.0f;
            switch (s_arp.lfo.target) {
                case LFO_TARGET_FILTER: e->filter_freq_coefs[COEF_MOD] = d * VOICE_LFO_DEPTH_FILTER; break;
                case LFO_TARGET_AMP:    e->amp_coefs[COEF_MOD]         = d * VOICE_LFO_DEPTH_AMP;    break;
                case LFO_TARGET_PITCH:  e->freq_coefs[COEF_MOD]        = d * VOICE_LFO_DEPTH_PITCH;  break;
                case LFO_TARGET_SCAN:   e->duty_coefs[COEF_MOD]        = d * VOICE_LFO_DEPTH_SCAN;   break;
                default: break;
            }
        }
        if (wave_filter_eg1) {
            e->filter_freq_coefs[COEF_EG1] = ARP_FILTER_EG1_DEPTH_OCT;
        }
        amy_helpers_event_send(e);
    }

    /* osc 1: LFO carrier — no pitch tracking, no velocity, no envelope.
     * amp_coefs[COEF_CONST]=1 when active so AMY computes mod_value each block;
     * amp_coefs[COEF_CONST]=0 when disabled (dormant, renders nothing). */
    e = amy_helpers_event_begin();
    e->synth = synth;
    e->osc   = 1;
    if (lfo_on) {
        e->wave                   = (uint16_t)arp_lfo_wave_to_amy(s_arp.lfo.wave);
        e->freq_coefs[COEF_CONST] = lfo_rate_to_hz(s_arp.lfo.rate,
                                                         sequencer_core_get_bpm());
        e->freq_coefs[COEF_NOTE]  = 0.0f;   /* no pitch tracking */
        e->freq_coefs[COEF_BEND]  = 0.0f;   /* no pitch bend     */
        e->amp_coefs[COEF_CONST]  = 1.0f;   /* active            */
        e->amp_coefs[COEF_VEL]    = 0.0f;   /* no velocity scale */
        e->amp_coefs[COEF_EG0]    = 0.0f;   /* no envelope decay */
    } else {
        e->amp_coefs[COEF_CONST]  = 0.0f;   /* dormant           */
    }
    amy_helpers_event_send(e);
}

/* Recompute and push the LFO carrier frequency at the current BPM.
 * Called by sequencer_core_set_bpm() after s_bpm is updated so the carrier
 * stays in sync when the user changes tempo.  Must NOT be called from the
 * render body (amy_queue_lock is held there); sequencer_core_set_bpm() runs
 * from the UI task, which is safe. */
void arp_core_refresh_lfo_freq(void)
{
    if (!s_arp.lfo_authored || !s_arp.lfo.enabled || s_arp.source != ARP_SRC_WAVE)
        return;

    amy_event *e = amy_helpers_event_begin();
    e->synth                  = sequencer_core_arp_synth();
    e->osc                    = 1;
    e->freq_coefs[COEF_CONST] = lfo_rate_to_hz(s_arp.lfo.rate,
                                                     sequencer_core_get_bpm());
    amy_helpers_event_send(e);
}

/* Push the current glide time straight to the arp synth. e->osc is left unset
 * and e->velocity is unset (not a note on/off), so patches_event_has_voices()
 * fans this out to every voice's base osc — see amy.c/patches.c dispatch. */
static void arp_push_portamento(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth         = sequencer_core_arp_synth();
    e->portamento_ms = s_arp.portamento_ms;
    amy_helpers_event_send(e);
}

/* (Re)build the arp synth slot for the current source and params, then
 * re-impose any authored ADSR / filter.  Mirrors drone_rebuild(). */
static void arp_rebuild(void)
{
    if (s_arp.source == ARP_SRC_WAVE) {
        arp_configure_wave_synth();
        /* WAVE mode has no patch envelope; always push the arp's env (authored
         * or default) so EG0 breakpoints are valid and notes decay correctly. */
        seq_env_t env_to_push = s_arp.env;
        voice_env_apply_ks_noise_floor(&env_to_push,
                                       s_arp.wave == KS, s_arp.wave == NOISE);
        sequencer_core_push_envelope(sequencer_core_arp_synth(), &env_to_push);
    } else {
        sequencer_core_arp_configure(s_arp.patch, sequencer_core_arp_voices(),
                                     s_arp.filter_authored, s_arp.filter.resonance);
        /* Raw wave/wavetable patches have no built-in EG0; always push the
         * envelope so notes decay. Juno/DX7 patch strings AND the bass/FM
         * presets carry their own envelopes (they're part of the preset's
         * character): only push over those when the user has authored one. */
        if (sequencer_core_is_wave_patch(s_arp.patch) || s_arp.env_authored) {
            sequencer_core_push_envelope(sequencer_core_arp_synth(), &s_arp.env);
        }
    }
    /* Filter re-apply is source-agnostic: both WAVE and PATCH respect it. */
    if (s_arp.filter_authored) {
        sequencer_core_push_filter(sequencer_core_arp_synth(), &s_arp.filter,
                                   s_arp.wave == KS);
    }
    /* EG1 (independent second envelope): push whenever the user has authored
     * one directly, OR whenever WAVE mode just wired filter_freq_coefs[COEF_EG1]
     * above (that coef must never read a stuck permanent 1.0). PATCH mode with
     * no authored EG1 is left alone — if the loaded patch already routes its
     * own bp1, its own patch-string values keep driving it. */
    bool wave_filter_eg1 = (s_arp.source == ARP_SRC_WAVE) &&
                           s_arp.filter_authored && s_arp.filter.enabled;
    if (s_arp.env1_authored || wave_filter_eg1) {
        sequencer_core_push_envelope_eg1(sequencer_core_arp_synth(), 0, &s_arp.env1);
    }
    /* Any reconfigure above (patch load or WAVE pool re-init) resets AMY's
     * per-osc portamento_alpha to 0 — reassert regardless of source. */
    arp_push_portamento();
}

/* ── Public API ──────────────────────────────────────────────────────── */

void arp_core_init(void)
{
    memset(&s_arp, 0, sizeof(s_arp));
    s_arp.enabled     = CONFIG_SEQ_ARP_DEFAULT_ENABLED;
    s_arp.dir         = ARP_UP;
    s_arp.octaves     = CONFIG_SEQ_ARP_DEFAULT_OCTAVES;
    s_arp.rate        = ARP_RATE_1_16;
    s_arp.gate_pct    = CONFIG_SEQ_ARP_DEFAULT_GATE_PCT;
    s_arp.scale_index = CONFIG_SEQ_ARP_DEFAULT_SCALE;
    s_arp.root_note   = CONFIG_SEQ_ARP_DEFAULT_ROOT_NOTE;
    s_arp.patch       = CONFIG_SEQ_ARP_DEFAULT_PATCH;
    s_arp.source      = ARP_SRC_PATCH;      /* preserve existing PATCH behaviour */
    s_arp.wave        = ARP_DEFAULT_WAVE;   /* SAW_DOWN — sensible WAVE default  */
    /* Default ADSR mirrors the melodic compile-time defaults; not authored until
     * the user commits in the graph editor (patch's own env wins until then). */
s_arp.env.attack_ms   = 4;    // Tiny curve to prevent an aggressive digital click
s_arp.env.decay_ms    = 250;  // Medium-short decay allows the note body to breathe
s_arp.env.sustain_pct = 30;   // Low sustain keeps the sequence energetic but audible if held
s_arp.env.release_ms  = 200;  // Controlled tail that fills space without causing a muddy low-end
    s_arp.env.eg_type     = 0;   /* ENVELOPE_NORMAL */
    s_arp.env_authored      = false;
    /* Second envelope (EG1): slower than the amp env above — the classic
     * "plucky amp decay, slower filter tail" shape once the filter is
     * authored+enabled in WAVE mode (see arp_configure_wave_synth()). */
    s_arp.env1.attack_ms   = 15;
    s_arp.env1.decay_ms    = 400;
    s_arp.env1.sustain_pct = 20;
    s_arp.env1.release_ms  = 300;
    s_arp.env1.eg_type     = 0;   /* ENVELOPE_NORMAL */
    s_arp.env1_authored    = false;
    /* Default filter: bypass (not authored; patch's filter wins until user commits). */
    s_arp.filter.filter_type = 0;   /* SEQ_FILTER_NONE */
    s_arp.filter.cutoff_hz   = 800.0f;
    s_arp.filter.resonance   = 1.0f;
    s_arp.filter.enabled     = false;
    s_arp.filter_authored    = false;
    /* Default LFO: disabled, not authored (bypass until user commits). */
    s_arp.lfo.enabled = false;
    s_arp.lfo.mode    = LFO_MODE_FREE;
    s_arp.lfo.wave    = LFO_WAVE_SINE;
    s_arp.lfo.rate    = LFO_RATE_1BAR;
    s_arp.lfo.depth   = 50;
    s_arp.lfo.target  = LFO_TARGET_FILTER;
    s_arp.lfo_authored = false;
    s_arp.amp_scale          = 1.0f; /* unity; memset zeroes, so explicit init */
    if (s_arp.octaves < 1) s_arp.octaves = 1;
    if (s_arp.octaves > ARP_OCT_MAX) s_arp.octaves = ARP_OCT_MAX;
    if (s_arp.scale_index >= quantizer_scale_count()) s_arp.scale_index = 0;
    for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++) s_arp.slots[i] = -1;

    ARP_HEAP_CHECK("arp_init: before arp_configure");
    arp_rebuild();
    ARP_HEAP_CHECK("arp_init: after arp_configure");
    /* If the arp boots enabled (with seeded slots), schedule an initial emit on
     * the first service tick rather than emitting inline during init. */
    arp_mark_dirty();
    ESP_LOGI(TAG, "arp_core initialized (synth %u)", sequencer_core_arp_synth());
}

void arp_core_refresh(void)
{
    arp_core_clear_all();

    if (!s_arp.enabled) {
        return;
    }

    /* ── SLOT mode: play slots in stored order, rests preserved ── */
    if (s_arp.dir == ARP_SLOT) {
        uint8_t active = 0;
        for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++)
            if (s_arp.slots[i] != -1) active++;
        if (active == 0) return;

        uint32_t rate    = s_rate_ticks[s_arp.rate];
        uint8_t  steps   = (uint8_t)(active * s_arp.octaves);
        uint32_t period  = (uint32_t)steps * rate;
        uint32_t gate    = (rate * s_arp.gate_pct) / 100u;
        if (gate < 1) gate = 1;

        uint32_t tag_base = sequencer_core_arp_tag_base();
        uint8_t  step_i   = 0;

        /* Apply per-target amp trim; clamp so velocity stays ≤1 (AMY cap). */
        float arp_vel = 0.9f * s_arp.amp_scale;
        if (arp_vel > 1.0f) arp_vel = 1.0f;

        for (uint8_t oct = 0; oct < s_arp.octaves; oct++) {
            for (uint8_t si = 0; si < ARP_MAX_SLOTS; si++) {
                int16_t v = s_arp.slots[si];
                if (v == -1) continue;   /* unused: not part of the cycle */

                uint32_t tick_on = 1u + (uint32_t)step_i * rate;

                if (v != ARP_REST) {
                    int32_t chromatic = (int32_t)v + (int32_t)oct * 12;
                    uint8_t play_note = arp_snap(sequencer_core_clamp_melodic_note(chromatic));
                    sequencer_core_arp_emit_note(tag_base + (uint32_t)step_i * 2u,
                                                 play_note, arp_vel, tick_on, gate, period);
                }
                /* REST: tag stays cleared (arp_clear_all already ran); step_i still advances. */
                step_i++;
            }
        }
        return;
    }

    /* ── UP / DOWN: sorted pitch order ── */
    uint8_t sorted[ARP_MAX_SLOTS];
    uint8_t count = (s_arp.dir == ARP_DOWN)
                    ? arp_collect_down(sorted)
                    : arp_collect_up(sorted);
    if (count == 0) {
        return;  /* nothing to play */
    }

    uint32_t rate    = s_rate_ticks[s_arp.rate];
    uint8_t  steps   = (uint8_t)(count * s_arp.octaves);
    uint32_t period  = (uint32_t)steps * rate;
    uint32_t gate    = (rate * s_arp.gate_pct) / 100u;
    if (gate < 1) gate = 1;

    uint32_t tag_base = sequencer_core_arp_tag_base();

    /* Apply per-target amp trim for UP/DOWN modes. */
    float arp_vel = 0.9f * s_arp.amp_scale;
    if (arp_vel > 1.0f) arp_vel = 1.0f;

    for (uint8_t i = 0; i < steps; i++) {
        /* index within the sorted set, advancing by octave every `count`. */
        uint8_t note_idx = i % count;
        uint8_t octave   = i / count;
        /* Direction: UP walks sorted ascending; DOWN walks it in reverse. */
        uint8_t pick = (s_arp.dir == ARP_DOWN)
                       ? sorted[count - 1 - note_idx]
                       : sorted[note_idx];
        int32_t chromatic = (int32_t)pick + (int32_t)octave * 12;
        uint8_t play_note = arp_snap(sequencer_core_clamp_melodic_note(chromatic));

        uint32_t tick_on = 1u + (uint32_t)i * rate;
        sequencer_core_arp_emit_note(tag_base + (uint32_t)i * 2u,
                                     play_note, arp_vel, tick_on, gate, period);
    }
}

void arp_set_enabled(bool enabled)
{
    if (s_arp.enabled == enabled) return;
    s_arp.enabled = enabled;
    arp_mark_dirty();
    ESP_LOGI(TAG, "arp %s", enabled ? "enabled" : "disabled");
}

void arp_set_direction(arp_dir_t dir)
{
    if (s_arp.dir == dir) return;
    s_arp.dir = dir;
    arp_mark_dirty();
}

void arp_set_octaves(uint8_t octaves)
{
    octaves = SEQ_CLAMP_U8(octaves, 1, ARP_OCT_MAX);
    if (s_arp.octaves == octaves) return;
    s_arp.octaves = octaves;
    arp_mark_dirty();
}

void arp_set_rate(arp_rate_t rate)
{
    if (rate >= ARP_RATE_COUNT) return;
    if (s_arp.rate == rate) return;
    s_arp.rate = rate;
    arp_mark_dirty();
}

void arp_set_gate_pct(uint8_t gate_pct)
{
    gate_pct = SEQ_CLAMP_U8(gate_pct, 10, 100);
    if (s_arp.gate_pct == gate_pct) return;
    s_arp.gate_pct = gate_pct;
    arp_mark_dirty();
}

void arp_set_scale(uint8_t scale_index)
{
    if (scale_index >= quantizer_scale_count()) scale_index = 0;
    if (s_arp.scale_index == scale_index) return;
    s_arp.scale_index = scale_index;
    arp_mark_dirty();
}

void arp_set_root_note(uint8_t root_note)
{
    root_note = SEQ_CLAMP_U8(root_note, 0, 127);
    if (s_arp.root_note == root_note) return;
    s_arp.root_note = root_note;
    arp_mark_dirty();
}

void arp_set_chord(uint8_t root_midi, uint8_t scale_index)
{
    arp_set_root_note(root_midi);
    arp_set_scale(scale_index);
}

void arp_set_patch(uint16_t patch_number)
{
    /* Full shared catalog (Juno/DX7/piano/waves/bass/wavetable/FM) — the same
     * ceiling as melodic; kind dispatch happens in sequencer_core_arp_configure(). */
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_FULL_MAX);
    if (s_arp.patch == patch_number) return;
    s_arp.patch = patch_number;
    /* In WAVE mode: store the new patch number but leave the synth slot alone.
     * It will be applied when the source is switched back to ARP_SRC_PATCH. */
    if (s_arp.source == ARP_SRC_PATCH) {
        arp_rebuild();
    }
    /* Patch reconfig does not change scheduling; no re-emit needed. */
}

void arp_core_fm_voice_changed(void)
{
#if CONFIG_SYNTH_CUSTOM_FM
    if (s_arp.source == ARP_SRC_PATCH && s_arp.patch == SEQ_PATCH_FM_CUSTOM) {
        fm_voice_push_live(sequencer_core_arp_synth(), &s_fm_voice);
    }
#endif
}

void arp_get_envelope(seq_env_t *out)
{
    if (out) *out = s_arp.env;
}

void arp_set_envelope(const seq_env_t *env)
{
    if (!env) return;
    s_arp.env = *env;
    if (s_arp.env.attack_ms < 2) s_arp.env.attack_ms = 2;  /* 2 ms floor */
    s_arp.env_authored = true;
    sequencer_core_push_envelope(sequencer_core_arp_synth(), &s_arp.env);
    ESP_LOGI(TAG, "arp env -> A%u D%u S%u%% R%u",
             (unsigned)s_arp.env.attack_ms, (unsigned)s_arp.env.decay_ms,
             (unsigned)s_arp.env.sustain_pct, (unsigned)s_arp.env.release_ms);
}

void arp_get_envelope2(seq_env_t *out)
{
    if (out) *out = s_arp.env1;
}

void arp_set_envelope2(const seq_env_t *env)
{
    if (!env) return;
    s_arp.env1 = *env;
    if (s_arp.env1.attack_ms < 2) s_arp.env1.attack_ms = 2;  /* 2 ms floor */
    s_arp.env1_authored = true;
    sequencer_core_push_envelope_eg1(sequencer_core_arp_synth(), 0, &s_arp.env1);
    ESP_LOGI(TAG, "arp env1 -> A%u D%u S%u%% R%u",
             (unsigned)s_arp.env1.attack_ms, (unsigned)s_arp.env1.decay_ms,
             (unsigned)s_arp.env1.sustain_pct, (unsigned)s_arp.env1.release_ms);
}

void arp_get_filter(seq_filter_t *out)
{
    if (out) *out = s_arp.filter;
}

void arp_set_filter(const seq_filter_t *f)
{
    if (!f) return;
    s_arp.filter = *f;
    s_arp.filter_authored = true;
    sequencer_core_push_filter(sequencer_core_arp_synth(), &s_arp.filter,
                               s_arp.wave == KS);
    ESP_LOGI(TAG, "arp filter -> type%u %.0fHz Q%.2f en=%d",
             s_arp.filter.filter_type, (double)s_arp.filter.cutoff_hz,
             (double)s_arp.filter.resonance, s_arp.filter.enabled);
}

void arp_get_lfo(seq_lfo_t *out)
{
    if (out) *out = s_arp.lfo;
}

void arp_set_lfo(const seq_lfo_t *lfo)
{
    if (!lfo) return;
    s_arp.lfo = *lfo;
    /* Only mark authored and rebuild in WAVE mode: PATCH mode stores the config
     * for later but does not activate the native LFO (patches own their osc
     * layout).  Setting lfo_authored while in PATCH mode causes ghost-activation
     * when the user subsequently switches to WAVE mode. */
    if (s_arp.source == ARP_SRC_WAVE) {
        s_arp.lfo_authored = true;
        arp_rebuild();
    }
    ESP_LOGI(TAG, "arp LFO -> en=%d wave=%u rate=%u depth=%u tgt=%u",
             lfo->enabled, (unsigned)lfo->wave, (unsigned)lfo->rate,
             (unsigned)lfo->depth, (unsigned)lfo->target);
}

void arp_set_slot(uint8_t idx, int16_t chromatic_note)
{
    if (idx >= ARP_MAX_SLOTS) return;
    if (chromatic_note >= 0) {
        chromatic_note = (int16_t)sequencer_core_clamp_melodic_note(chromatic_note);
    } else if (chromatic_note == ARP_REST) {
        /* leave as -2: deliberate silent step */
    } else {
        chromatic_note = -1;
    }
    if (s_arp.slots[idx] == chromatic_note) return;
    s_arp.slots[idx] = chromatic_note;
    arp_mark_dirty();
}

void arp_set_source(arp_source_t src)
{
    if (src != ARP_SRC_WAVE && src != ARP_SRC_PATCH) return;
    if (s_arp.source == src) return;
    s_arp.source = src;
    arp_rebuild();
    ESP_LOGI(TAG, "arp source -> %s", src == ARP_SRC_WAVE ? "WAVE" : "PATCH");
}

void arp_set_wave(uint16_t amy_wave)
{
    if (s_arp.wave == amy_wave) return;
    s_arp.wave = amy_wave;
    /* Reconfigure the slot immediately only when WAVE mode is active. */
    if (s_arp.source == ARP_SRC_WAVE) {
        arp_rebuild();
    }
}

/* Perform a pending re-emit, if any. Called once per UI frame so rapid edits
 * coalesce into a single refresh. Cheap no-op when nothing changed. */
void arp_core_service(void)
{
    if (!s_arp_dirty) return;
    s_arp_dirty = false;
    arp_core_refresh();
}

/* ── Getters ─────────────────────────────────────────────────────────── */

bool         arp_get_enabled(void)    { return s_arp.enabled; }
arp_dir_t    arp_get_direction(void)  { return s_arp.dir; }
uint8_t      arp_get_octaves(void)    { return s_arp.octaves; }
arp_rate_t   arp_get_rate(void)       { return s_arp.rate; }
uint8_t      arp_get_gate_pct(void)   { return s_arp.gate_pct; }
uint8_t      arp_get_scale(void)      { return s_arp.scale_index; }
uint8_t      arp_get_root_note(void)  { return s_arp.root_note; }
uint16_t     arp_get_patch(void)      { return s_arp.patch; }
arp_source_t arp_get_source(void)     { return s_arp.source; }
uint16_t     arp_get_wave(void)       { return s_arp.wave; }

const char *arp_rate_name(arp_rate_t rate)
{
    if (rate >= ARP_RATE_COUNT) return "?";
    return s_rate_names[rate];
}

int16_t arp_get_slot(uint8_t idx)
{
    if (idx >= ARP_MAX_SLOTS) return -1;
    return s_arp.slots[idx];
}

int16_t arp_get_slot_snapped(uint8_t idx)
{
    if (idx >= ARP_MAX_SLOTS || s_arp.slots[idx] < 0) return -1;
    return (int16_t)arp_snap((uint8_t)s_arp.slots[idx]);
}

uint8_t arp_active_slot_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++) {
        if (s_arp.slots[i] < 0) break;
        n++;
    }
    return n;
}

uint8_t arp_active_step_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++)
        if (s_arp.slots[i] != -1) n++;
    return n;
}

/* ── Per-target amplitude trim (graph editor amp mode) ──────────────────── */

void arp_set_amp_scale(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (fabsf(s_arp.amp_scale - v) < 0.001f) return;
    s_arp.amp_scale = v;
    arp_mark_dirty();   /* coalesced re-emit on next arp_core_service() */
}

float arp_get_amp_scale(void) { return s_arp.amp_scale; }

/* ── Portamento / glide ──────────────────────────────────────────────────── */

void arp_set_portamento_ms(uint16_t ms)
{
    ms = SEQ_CLAMP_U16(ms, 0, ARP_PORTAMENTO_MAX_MS);
    if (s_arp.portamento_ms == ms) return;
    s_arp.portamento_ms = ms;
    arp_push_portamento();   /* not a scheduling change: push directly, no re-emit */
}

uint16_t arp_get_portamento_ms(void) { return s_arp.portamento_ms; }
