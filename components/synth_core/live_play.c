#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS

#include "live_play.h"
#include "amy_helpers.h"
#include "sequencer_core.h"
#include "seq_core_config.h"
#include "voice_config.h"   /* voice_params_init_defaults, env bounds, LFO apply */
#include "seq_clamp.h"
#include "esp_log.h"

static const char *TAG = "live_play";

/* Slot 10 is the gap between the drum block (6..9) and the melodic base (11)
 * in the fixed synth-slot map (seq_core_config.h); max_synths=68 already
 * covers it. 4 voices matches the arp - enough for chords under one hand,
 * ample osc headroom even for 7-osc FM voices against AMY's 250-osc pool. */
#define LIVE_SYNTH   10
#define LIVE_VOICES  4

static uint16_t s_patch = SEQ_MEL_PATCH;
static bool     s_ready = false;

/* Runtime-editable voice params (ADSR/EG1/filter/LFO/amp trim + authored
 * flags). Same block every other engine embeds; defaults installed on first
 * ensure_ready so amp_trim starts at unity rather than silent. */
static voice_params_t s_vp;
static bool           s_vp_inited = false;

/* The editors can open before the radio session ever starts, i.e. before
 * ensure_ready() has installed the defaults - and a zero-initialised block has
 * amp_trim 0.0, which the graph editor would read as 0% and commit as silence.
 * Every accessor below goes through this. UI task only (single-threaded), so
 * the check needs no guard; live_note() deliberately does NOT call it, since it
 * runs on the transport task and ensure_ready() always precedes any note. */
static voice_params_t *live_vp(void)
{
    if (!s_vp_inited) {
        voice_params_init_defaults(&s_vp);
        s_vp_inited = true;
    }
    return &s_vp;
}

/* Held-note bitmap (128 bits) so all_notes_off releases exactly what is
 * sounding. Written from the transport task, drained from synth_ui / NimBLE
 * host on stop/disconnect; per-bit races are harmless (a spurious note-off
 * to an idle voice is a no-op in AMY). */
static volatile uint32_t s_held[4];

static void live_note(uint8_t note, float velocity)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth     = LIVE_SYNTH;
    e->midi_note = (float)note;
    /* Amp trim rides the note velocity (the graph editor's amp mode). Note-offs
     * pass velocity 0 and must stay 0. */
    e->velocity  = (velocity > 0.0f) ? velocity * s_vp.amp_trim : 0.0f;
    amy_helpers_event_send(e);
}

/* Push the stored filter including the bipolar EG1->cutoff sweep depth.
 * Mirrors arp_apply_filter() minus its KS branch: the live slot is always a
 * patch voice, never a WAVE-mode osc build. Valid EG1 breakpoints go out
 * alongside a nonzero sweep so filter_freq_coefs[COEF_EG1] never reads AMY's
 * never-configured always-open unity gate. */
static void live_apply_filter(const seq_filter_t *f)
{
    if (!f) return;
    amy_event *e = amy_helpers_event_begin();
    e->synth = LIVE_SYNTH;
    if (f->enabled) {
        e->filter_type = f->filter_type;
        e->filter_freq_coefs[COEF_CONST] = f->cutoff_hz;
        e->resonance = f->resonance;
        if (f->filter_env_amount != 0.0f) {
            e->filter_freq_coefs[COEF_EG1] = f->filter_env_amount;
        }
    } else {
        e->filter_type = FILTER_NONE;
    }
    amy_helpers_event_send(e);

    if (f->enabled && f->filter_env_amount != 0.0f) {
        sequencer_core_push_envelope_eg1(LIVE_SYNTH, 0, &s_vp.env1);
    }
}

/* Re-push whatever the user has authored. Called after every slot configure,
 * since a patch load rebuilds the voice and drops our overrides. */
static void live_apply_authored(void)
{
    if (s_vp.env_authored)    sequencer_core_push_envelope(LIVE_SYNTH, &s_vp.env);
    if (s_vp.env1_authored)   sequencer_core_push_envelope_eg1(LIVE_SYNTH, 0, &s_vp.env1);
    if (s_vp.filter_authored) live_apply_filter(&s_vp.filter);
}

void live_play_ensure_ready(void)
{
    if (s_ready) return;
    if (!s_vp_inited) {
        voice_params_init_defaults(&s_vp);
        s_vp_inited = true;
    }
    sequencer_core_configure_synth_slot(LIVE_SYNTH, s_patch, LIVE_VOICES);
    live_apply_authored();
    s_ready = true;
    ESP_LOGI(TAG, "live slot %u ready (patch %u, %u voices)",
             (unsigned)LIVE_SYNTH, (unsigned)s_patch, (unsigned)LIVE_VOICES);
}

void live_play_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    (void)channel;
    if (!s_ready || note > 127) return;
    s_held[note >> 5] |= 1u << (note & 31u);
    live_note(note, (float)velocity * (1.0f / 127.0f));
}

void live_play_note_off(uint8_t channel, uint8_t note)
{
    (void)channel;
    if (!s_ready || note > 127) return;
    s_held[note >> 5] &= ~(1u << (note & 31u));
    live_note(note, 0.0f);
}

void live_play_all_notes_off(void)
{
    if (!s_ready) return;
    for (uint8_t w = 0; w < 4; w++) {
        uint32_t bits = s_held[w];
        s_held[w] = 0;
        while (bits) {
            uint8_t bit = (uint8_t)__builtin_ctz(bits);
            bits &= bits - 1u;
            live_note((uint8_t)(w * 32u + bit), 0.0f);
        }
    }
}

uint16_t live_play_get_patch(void)
{
    return s_patch;
}

void live_play_set_patch(uint16_t patch_number)
{
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_FULL_MAX);
    if (s_patch == patch_number) return;
    s_patch = patch_number;
    if (s_ready) {
        /* Reconfigure kills sounding voices (osc topology may change);
         * clear the bitmap so stale note-offs are not replayed later. */
        for (uint8_t w = 0; w < 4; w++) s_held[w] = 0;
        sequencer_core_configure_synth_slot(LIVE_SYNTH, s_patch, LIVE_VOICES);
        live_apply_authored();
    }
}

/* ── Runtime-editable voice params (shared editors) ──────────────────────
 * Structural twin of the arp block (arp_core.c): one voice, no layer/track
 * scope, authored flags gating re-application after a patch change. */

void live_play_get_envelope(seq_env_t *out)
{
    if (out) *out = live_vp()->env;
}

void live_play_set_envelope(const seq_env_t *env)
{
    if (!env) return;
    voice_params_t *vp = live_vp();
    vp->env = *env;
    vp->env.attack_ms  = SEQ_CLAMP_U32(vp->env.attack_ms,
                                       VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    vp->env.release_ms = SEQ_CLAMP_U32(vp->env.release_ms,
                                       VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    vp->env_authored = true;
    if (s_ready) sequencer_core_push_envelope(LIVE_SYNTH, &vp->env);
}

void live_play_get_envelope2(seq_env_t *out)
{
    if (out) *out = live_vp()->env1;
}

void live_play_set_envelope2(const seq_env_t *env)
{
    if (!env) return;
    voice_params_t *vp = live_vp();
    vp->env1 = *env;
    vp->env1.attack_ms  = SEQ_CLAMP_U32(vp->env1.attack_ms,
                                        VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    vp->env1.release_ms = SEQ_CLAMP_U32(vp->env1.release_ms,
                                        VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    vp->env1_authored = true;
    if (s_ready) sequencer_core_push_envelope_eg1(LIVE_SYNTH, 0, &vp->env1);
}

void live_play_get_filter(seq_filter_t *out)
{
    if (out) *out = live_vp()->filter;
}

void live_play_set_filter(const seq_filter_t *f)
{
    if (!f) return;
    voice_params_t *vp = live_vp();
    vp->filter = *f;
    vp->filter_authored = true;
    if (s_ready) live_apply_filter(&vp->filter);
}

void live_play_get_lfo(seq_lfo_t *out)
{
    if (out) *out = live_vp()->lfo;
}

/* Stored but NOT applied: the live slot is always a patch voice, and a patch
 * owns its own osc layout, so the native LFO carrier cannot be grafted on the
 * way a WAVE-mode voice takes it. Driving it would need the PATCH-mode
 * software stepper (arp_core.c "PATCH-mode software LFO fallback" + a service
 * call on the UI tick). Until then the editors skip the LFO page for this
 * target, the same way they skip it for the drone. */
void live_play_set_lfo(const seq_lfo_t *lfo)
{
    if (!lfo) return;
    live_vp()->lfo = *lfo;
}

float live_play_get_amp_scale(void)
{
    return live_vp()->amp_trim;
}

void live_play_set_amp_scale(float v)
{
    live_vp()->amp_trim = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
}

/* ── Editor live-preview (AMY only; store + authored flags untouched) ────── */

void live_play_preview_envelope(const seq_env_t *env)
{
    if (env && s_ready) sequencer_core_push_envelope(LIVE_SYNTH, env);
}

void live_play_preview_envelope2(const seq_env_t *env)
{
    if (env && s_ready) sequencer_core_push_envelope_eg1(LIVE_SYNTH, 0, env);
}

void live_play_preview_filter(const seq_filter_t *f)
{
    if (s_ready) live_apply_filter(f);
}

#endif /* CONFIG_SYNTH_WIRELESS */
