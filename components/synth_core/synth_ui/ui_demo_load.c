#include "synth_ui/synth_ui_internal.h"   /* seq_state, synth_ui_reload_mirror_from_core */
#include "custompatches/demo_billiejean.h"
#include "custompatches/demo_aurora.h"
#include "custompatches/jukebox_demo.h"
#include "esp_log.h"

static const char *TAG = "demos";

/* Deferred built-in demo load (registry index +1, so 0 still means none and
 * the drain's truthy check can't skip registry slot 0), consumed on the
 * synth_ui_task drain: a demo rebuilds layer topology via core add/delete,
 * which only that task - the single s_layers applier - may call. Menu clicks
 * run on the button task, so they only set this flag. */
static volatile uint8_t s_demo_pending = 0;

/* Demo registry: labels drive the DEMOS submenu (ui_screen_demos.c reads them
 * via synth_ui_demo_label), the loader runs on the UI task's drain.
 * Registering a demo here is all it takes to get a selectable row. */
typedef bool (*demo_loader_fn)(void);

typedef struct {
    const char    *label;
    demo_loader_fn load;
} demo_entry_t;

static const demo_entry_t s_demos[] = {
    { "Aurora",             demo_aurora_load },
    { "JukeBox",            jukebox_demo_load },
    { "Billie Jean",        demo_billiejean_load },
    { "BJ Chords + Juno",   demo_billiejean_scheduled_load },
};
#define DEMO_COUNT ((uint8_t)(sizeof(s_demos) / sizeof(s_demos[0])))
_Static_assert(DEMO_COUNT <= SYNTH_UI_DEMOS_MAX,
               "demo registry exceeds SYNTH_UI_DEMOS_MAX; bump it in synth_ui.h");

void synth_ui_request_demo(uint8_t which)
{
    if (which < DEMO_COUNT) s_demo_pending = (uint8_t)(which + 1u);
}

uint8_t synth_ui_demo_count(void)
{
    return DEMO_COUNT;
}

const char *synth_ui_demo_label(uint8_t idx)
{
    if (idx >= DEMO_COUNT) return "";
    return s_demos[idx].label;
}

/* Deferred demo load drain, called from synth_ui_task each frame after its
 * add/delete drains (a demo replaces the layers, so pending structural edits
 * resolve first). The loader starts the transport, so re-sync the UI mirror
 * and mark it playing. */
void synth_ui_demo_drain(void)
{
    if (!s_demo_pending) return;
    uint8_t demo = (uint8_t)(s_demo_pending - 1u);
    s_demo_pending = 0;
    bool ok = false;
    if (demo < DEMO_COUNT) ok = s_demos[demo].load();
    if (!ok) ESP_LOGW(TAG, "demo: load failed (which=%u)", (unsigned)demo);
    /* A non-jukebox load hands the layers back to the player: drop the
     * running generator so it never rewrites the new song's steps. */
    if (s_demos[demo].load != jukebox_demo_load) {
        jukebox_demo_deactivate();
    }
    synth_ui_reload_mirror_from_core();
    seq_state.playing = true;   /* sample_rec/project loads stop; a demo plays */
}

/* JukeBox demo live service: the generator re-rolls its own steps; when it
 * rewrote a bar this frame, re-sync the UI mirror so the grid follows. */
void synth_ui_demo_service(void)
{
    if (jukebox_demo_service()) {
        synth_ui_reload_mirror_from_core();
    }
}