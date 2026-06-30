/* amy_fx.c — Global FX cache and master volume for the AMY synthesizer.
 *
 * Holds the cached state for AMY's global effect bus (EQ, echo, chorus,
 * reverb) and the master output volume.  Extracted from synth_ui.c so that
 * sequencer/arp/drone cores can call synth_ui_fx_reassert_global() without
 * taking a dependency on the UI headers (u8g2, display_*). */

#include "amy_fx.h"
#include "amy.h"
#include "amy_helpers.h"
#include "sdkconfig.h"

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
    .presets_alter_global = false,
};

/* ── Master volume ──────────────────────────────────────────────────────── */
/* Range 0..2.0, unity=1.0.  Default matches AMY's own init (amy_start sets
 * amy_global.volume[bus]=1.0f), so no explicit push is needed at boot. */
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
    amy_helpers_event_send(e);
}

void fx_push_chorus(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->chorus_level = (float)s_fx.chorus_level / 100.0f;
    amy_helpers_event_send(e);
}

void fx_push_reverb(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->reverb_level = (float)s_fx.reverb_level / 100.0f;
    amy_helpers_event_send(e);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Re-impose the cached global FX after a patch load so a synth's preset
 * cannot hijack the shared EQ/chorus/echo/reverb.  No-op when the user has
 * opted into letting presets drive global FX.  Safe to call from the
 * sequencer/arp/drone task contexts: each fx_push_* serialises through the
 * shared amy_helpers mutex.
 *
 * Cost is four queued events per patch change (a rare, user-driven action)
 * and zero in the render loop.  The reassert events are queued AFTER the
 * patch's own FX deltas, so they win; both drain in the same render quantum
 * on Core 1, so there is no audible blip. */
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
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    s_master_volume = v;
    /* Write directly to amy_global.volume[] — an aligned float store,
     * atomic on Xtensa.  Called from synth_ui_task (not the render body),
     * so the AMY locking rule (no add_delta_to_queue inside the locked
     * render loop) is satisfied. */
    for (int b = 0; b < AMY_NUM_BUSES; b++) {
        amy_global.volume[b] = v;
    }
}

float amy_fx_get_master_volume(void)
{
    return s_master_volume;
}
