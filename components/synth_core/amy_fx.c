/* amy_fx.c - cached state for AMY's global effect bus (EQ, echo, chorus,
 * reverb) plus the master output volume. Kept out of synth_ui.c so the
 * sequencer/arp/drone cores can call synth_ui_fx_reassert_global() without
 * depending on the UI headers (u8g2, display_*). */

#include "amy_fx.h"
#include "amy.h"
#include "amy_helpers.h"
#include "sdkconfig.h"
#include "seq_clamp.h"

/* ── Default initialisation guards ─────────────────────────────────────── */
#ifndef CONFIG_SEQ_FX_DEFAULT_ECHO
#define CONFIG_SEQ_FX_DEFAULT_ECHO    0
#endif
#ifndef CONFIG_SEQ_FX_DEFAULT_REVERB
#define CONFIG_SEQ_FX_DEFAULT_REVERB  0
#endif
#ifndef CONFIG_SEQ_FX_DEFAULT_CHORUS
#define CONFIG_SEQ_FX_DEFAULT_CHORUS  0
#endif

/* ── FX state ───────────────────────────────────────────────────────────── */
fx_state_t s_fx = {
    .eq_low_db  = 0,
    .eq_mid_db  = 0,
    .eq_high_db = 0,
    .echo_level          = CONFIG_SEQ_FX_DEFAULT_ECHO,
    .chorus_level        = CONFIG_SEQ_FX_DEFAULT_CHORUS,
    .reverb_level        = CONFIG_SEQ_FX_DEFAULT_REVERB,
    /* Extended params start UNSET so AMY keeps its factory character until
     * edited. Load-bearing: designated-init would zero these, and 0
     * liveness/damping silently mangles the reverb. */
    .echo_delay_ms   = FX_PARAM_UNSET,
    .echo_feedback   = FX_PARAM_UNSET,
    .echo_tone       = FX_PARAM_UNSET,
    .reverb_liveness = FX_PARAM_UNSET,
    .reverb_damping  = FX_PARAM_UNSET,
    .reverb_xover_hz = FX_PARAM_UNSET,
    .chorus_rate     = FX_PARAM_UNSET,
    .chorus_depth    = FX_PARAM_UNSET,
    /* Mirrors bus_reset(): stage off, unity drive/mix, transparent crusher. */
    .bus_dist_type   = 0,
    .bus_dist_drive  = 1,
    .bus_dist_bits   = 16,
    .bus_dist_rate   = 1,
    .bus_dist_mix    = 100,
    .presets_alter_global = false,
};

/* ── Master volume ──────────────────────────────────────────────────────── */
/* Range 0..2.0, unity=1.0. Matches AMY's own init (amy_start sets
 * amy_global.volume[bus]=1.0f), so no push is needed at boot. */
static float s_master_volume = 1.0f;

/* ── FX push helpers ────────────────────────────────────────────────────── */
void fx_push_eq(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->eq_l = (float)s_fx.eq_low_db;   /* AMY interprets these as dB */
    e->eq_m = (float)s_fx.eq_mid_db;
    e->eq_h = (float)s_fx.eq_high_db;
    amy_helpers_event_send(e);
}

void fx_push_echo(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->echo_level = (float)s_fx.echo_level / 100.0f;
    /* Only send sub-params the user has set; unset ones stay AMY_UNSET so
     * config_echo keeps the bus's current value. */
    if (s_fx.echo_delay_ms != FX_PARAM_UNSET)
        e->echo_delay_ms   = (float)s_fx.echo_delay_ms;
    if (s_fx.echo_feedback != FX_PARAM_UNSET)
        e->echo_feedback   = (float)s_fx.echo_feedback / 100.0f;
    if (s_fx.echo_tone != FX_PARAM_UNSET)
        e->echo_filter_coef = (float)s_fx.echo_tone / 100.0f;
    amy_helpers_event_send(e);
}

void fx_push_chorus(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->chorus_level = (float)s_fx.chorus_level / 100.0f;
    if (s_fx.chorus_rate != FX_PARAM_UNSET)
        e->chorus_lfo_freq = (float)s_fx.chorus_rate / 100.0f;   /* centi-Hz -> Hz */
    if (s_fx.chorus_depth != FX_PARAM_UNSET)
        e->chorus_depth    = (float)s_fx.chorus_depth / 100.0f;
    amy_helpers_event_send(e);
}

void fx_push_reverb(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->reverb_level = (float)s_fx.reverb_level / 100.0f;
    if (s_fx.reverb_liveness != FX_PARAM_UNSET)
        e->reverb_liveness = (float)s_fx.reverb_liveness / 100.0f;
    if (s_fx.reverb_damping != FX_PARAM_UNSET)
        e->reverb_damping  = (float)s_fx.reverb_damping / 100.0f;
    if (s_fx.reverb_xover_hz != FX_PARAM_UNSET)
        e->reverb_xover_hz = (float)s_fx.reverb_xover_hz;
    amy_helpers_event_send(e);
}

void fx_push_dist(void)
{
    /* The full config travels together (like the per-osc 'C' wire): the type
     * delta restarts the crusher/DC-blocker state, which recaptures within
     * one hold period - inaudible at menu-edit rate. */
    amy_event *e = amy_helpers_event_begin();
    e->bus_dist_type  = (float)s_fx.bus_dist_type;
    e->bus_dist_drive = (float)s_fx.bus_dist_drive;
    e->bus_dist_bits  = (float)s_fx.bus_dist_bits;
    e->bus_dist_rate  = (float)s_fx.bus_dist_rate;
    e->bus_dist_mix   = (float)s_fx.bus_dist_mix / 100.0f;
    amy_helpers_event_send(e);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Re-impose the cached global FX after a patch load so a preset cannot hijack
 * the shared EQ/chorus/echo/reverb. No-op when the user opted into letting
 * presets drive global FX. Safe from the sequencer/arp/drone task contexts:
 * each fx_push_* serialises through the shared amy_helpers mutex.
 *
 * These events are queued AFTER the patch's own FX deltas, so they win, and
 * both drain in the same render quantum - no audible blip. */
void synth_ui_fx_reassert_global(void)
{
    if (s_fx.presets_alter_global) return;
    fx_push_eq();
    fx_push_chorus();
    fx_push_echo();
    fx_push_reverb();
}

void amy_fx_set_master_volume(float v)
{
    v = SEQ_CLAMP_F32(v, 0.0f, 2.0f);
    s_master_volume = v;
    /* Direct write to amy_global.volume[]: an aligned float store, atomic on
     * Xtensa. Called from synth_ui_task, never the render body. */
    for (int b = 0; b < amy_global.config.max_buses; b++) {
        amy_global.volume[b] = v;
    }
}

float amy_fx_get_master_volume(void)
{
    return s_master_volume;
}
