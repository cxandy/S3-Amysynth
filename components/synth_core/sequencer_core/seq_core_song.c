#include "sequencer_core/seq_core_internal.h"

/* ── State definitions —— owns song-mode (mute-scene chain) ───────────── */
song_state_t s_song = {
    .scenes = {
        { .bars = SONG_DEFAULT_BARS, .layer_mask = 0x0F },
    },
    .count            = 1,
    .current          = 0,
    .scene_start_bar  = 0,
    .enabled          = false,
    .loop             = true,
};

/* Set by input/UI entry points that change song state, consumed once per tick
 * by sequencer_core_song_service() on synth_ui_task. That makes synth_ui_task
 * the SINGLE task calling the mute-apply path, so song edits never race the
 * periodic advance or each other - same discipline as the chord progression. */
volatile bool s_song_apply_pending = false;

/* ── Private helpers ─────────────────────────────────────────────────── */

/* Apply `mask` by muting every track of a layer whose bit is clear and
 * unmuting every track of a layer whose bit is set. The song owns layer
 * audibility while enabled (set_track_mute kills voices on the muting edge,
 * so a section change also chokes any ringing note). */
static void song_set_layer_masks(uint8_t mask)
{
    for (uint8_t li = 0; li < s_num_layers; li++) {
        seq_layer_t *layer = &s_layers[li];
        bool audible = (mask & (1u << li)) != 0;
        for (uint8_t t = 0; t < layer->num_tracks; t++) {
            sequencer_core_set_track_mute(li, t, !audible);
        }
    }
}

/* Disable path: the song stops driving audibility, so restore every track.
 * set_track_mute is a no-op on already-unmuted tracks (dedup), so this is
 * cheap on a song that never ended muted. */
static void song_restore_all(void)
{
    for (uint8_t li = 0; li < s_num_layers; li++) {
        seq_layer_t *layer = &s_layers[li];
        for (uint8_t t = 0; t < layer->num_tracks; t++) {
            sequencer_core_set_track_mute(li, t, false);
        }
    }
}

/* ── Service ──────────────────────────────────────────────────────────── */

/* Called from synth_ui_task at 20 Hz. Drains s_song_apply_pending (set by
 * input tasks after mutating song state) and advances the scene when the
 * current entry expires. Funnelling every emit through one task removes the
 * need for a lock: there is only one writer. */
void sequencer_core_song_service(void)
{
    /* Drain deferred applies first, regardless of playing/enabled state. When
     * disabled or empty, restore every track so no stale mute survives. */
    if (s_song_apply_pending) {
        s_song_apply_pending = false;
        if (s_song.enabled && s_song.count > 0) {
            song_set_layer_masks(s_song.scenes[s_song.current].layer_mask);
        } else {
            song_restore_all();
        }
    }

    if (!s_song.enabled || s_song.count == 0 || !s_playing) return;

    uint32_t bars = sequencer_bars_elapsed();

    /* Fresh play-start self-heal. sequencer_core_set_playing() re-anchors the
     * bar baseline to the NEXT bar boundary, which makes bars_elapsed() drop
     * below the stale anchor from the previous run - exactly once, on the
     * first service tick after play. That is the engine-free signal to rewind
     * the song to scene 0: predictable arrangement start, no transport hook. */
    if (bars < s_song.scene_start_bar) {
        s_song.current         = 0;
        s_song.scene_start_bar = bars;
        song_set_layer_masks(s_song.scenes[0].layer_mask);
        return;
    }

    const song_scene_t *sc = &s_song.scenes[s_song.current];
    if (bars - s_song.scene_start_bar < sc->bars) return;

    /* Advance to the next scene. Non-looping songs park on the last scene
     * (the arrangement plays through once and holds). Bars are clamped to
     * >= 1 at every edit site, so sc->bars is never 0 here. */
    uint8_t next = (uint8_t)(s_song.current + 1);
    if (next >= s_song.count) {
        if (!s_song.loop) return;
        next = 0;
    }
    s_song.current         = next;
    s_song.scene_start_bar += sc->bars;   /* incremental, like the progression */

    /* A service stall longer than a scene mustn't off-by-one drift the form:
     * if we are already past the new scene's own window, re-anchor so the
     * catch-up happens over successive ticks instead of skipping scenes. */
    uint32_t now = sequencer_bars_elapsed();
    if (now - s_song.scene_start_bar >= s_song.scenes[next].bars) {
        s_song.scene_start_bar = now;
    }

    song_set_layer_masks(s_song.scenes[next].layer_mask);
    ESP_LOGI(TAG, "song -> scene %u (mask=0x%x, bars=%u)",
             next, s_song.scenes[next].layer_mask, s_song.scenes[next].bars);
}

/* ── Song public API ──────────────────────────────────────────────────── */

void sequencer_core_song_set_enabled(bool en)
{
    if (s_song.enabled == en) return;
    s_song.enabled = en;
    if (en && s_song.count > 0) {
        /* Apply from the next service tick; the scene window counts from the
         * next bar line so a mid-bar enable still gives the first scene its
         * full duration (the partial bar is a free lead-in). */
        s_song.current         = 0;
        s_song.scene_start_bar = sequencer_bars_elapsed() + 1;
    }
    s_song_apply_pending = true;
}

bool sequencer_core_song_get_enabled(void) { return s_song.enabled; }

void sequencer_core_song_set_loop(bool loop)
{
    if (s_song.loop == loop) return;
    s_song.loop = loop;
}

bool sequencer_core_song_get_loop(void) { return s_song.loop; }

void sequencer_core_song_set_scene(uint8_t idx, uint8_t bars, uint8_t layer_mask)
{
    if (idx >= s_song.count || idx >= SONG_MAX_SCENES) return;
    s_song.scenes[idx].bars       = (bars > 0) ? bars : SONG_DEFAULT_BARS;
    s_song.scenes[idx].layer_mask = layer_mask & 0x0F;
    /* Editing the live scene defers an apply so the audible section follows. */
    if (s_song.enabled && idx == s_song.current) s_song_apply_pending = true;
}

void sequencer_core_song_get_scene(uint8_t idx, uint8_t *bars, uint8_t *layer_mask)
{
    if (bars)       *bars       = SONG_DEFAULT_BARS;
    if (layer_mask) *layer_mask = 0x0F;
    if (idx >= s_song.count || idx >= SONG_MAX_SCENES) return;
    if (bars)       *bars       = s_song.scenes[idx].bars;
    if (layer_mask) *layer_mask = s_song.scenes[idx].layer_mask;
}

void sequencer_core_song_set_count(uint8_t count)
{
    if (count > SONG_MAX_SCENES) count = SONG_MAX_SCENES;
    if (count < 1) count = 1;
    s_song.count = count;
    /* An active scene that fell out of range wraps to 0, restarts its window
     * from the next bar line and re-applies. */
    if (s_song.current >= s_song.count) {
        s_song.current         = 0;
        s_song.scene_start_bar = sequencer_bars_elapsed() + 1;
        if (s_song.enabled) s_song_apply_pending = true;
    }
}

uint8_t sequencer_core_song_get_count(void)   { return s_song.count; }
uint8_t sequencer_core_song_get_current(void)  { return s_song.current; }
uint8_t sequencer_core_song_get_max(void)      { return SONG_MAX_SCENES; }

/* Bars elapsed within the current scene (0-based), for the UI status bar. */
uint8_t sequencer_core_song_bars_in_current(void)
{
    if (!s_playing) return 0;
    if (!s_song.enabled || s_song.count == 0) return 0;
    uint32_t bars = sequencer_bars_elapsed();
    if (bars < s_song.scene_start_bar) return 0;   /* anchor just moved */
    return (uint8_t)(bars - s_song.scene_start_bar);
}

/* Append a default scene (all layers, N bars) if room remains. */
bool sequencer_core_song_add_scene(void)
{
    if (s_song.count >= SONG_MAX_SCENES) return false;
    uint8_t idx = s_song.count;
    s_song.scenes[idx].bars       = SONG_DEFAULT_BARS;
    s_song.scenes[idx].layer_mask = 0x0F;
    s_song.count = (uint8_t)(idx + 1);
    return true;
}

/* Delete scene idx, shifting later scenes down. Keeps at least one scene. */
void sequencer_core_song_delete_scene(uint8_t idx)
{
    if (idx >= s_song.count || s_song.count <= 1) return;
    for (uint8_t i = idx; i + 1 < s_song.count; i++) {
        s_song.scenes[i] = s_song.scenes[i + 1];
    }
    s_song.count--;
    /* The active scene moved: re-anchor and re-apply so it results sound. */
    if (s_song.current == idx || (s_song.count > 0 && s_song.current > idx)) {
        if (s_song.current >= s_song.count) s_song.current = 0;
        else if (s_song.current > idx) s_song.current--;
        s_song.scene_start_bar = sequencer_bars_elapsed() + 1;
        if (s_song.enabled) s_song_apply_pending = true;
    }
}