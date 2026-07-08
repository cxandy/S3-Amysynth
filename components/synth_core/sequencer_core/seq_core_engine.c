#include "sequencer_core/seq_core_internal.h"

/* ── State definitions — owns step cache, source notes, bar baseline ── */
uint8_t  s_cached_step[MAX_LAYERS];
uint8_t  s_track_source_note[MAX_LAYERS][SEQ_TRACKS];
uint32_t s_bar_baseline = 0;

/* ── Bar counter ─────────────────────────────────────────────────────────
 * sequencer_ticks() is monotonic (never resets on play/stop in normal use).
 * Capture a baseline at play-start; compute bars elapsed from the delta. */
uint32_t sequencer_bars_elapsed(void)
{
    uint32_t t = sequencer_ticks();
    if (t < s_bar_baseline) return 0;
    return (t - s_bar_baseline) / SEQ_TICKS_PER_BAR;
}

/* ── Tag helpers ─────────────────────────────────────────────────────── */
/*
 * Tag layout (uint32_t — tag space is effectively unlimited):
 *
 *   ON  tag = layer * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
 *             + track * SEQ_MAX_STEPS + step
 *
 *   OFF tag = ON tag + (SEQ_TRACKS * SEQ_MAX_STEPS)
 *
 *   Preview = MAX_LAYERS * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
 *             + layer * SEQ_TRACKS + track
 *
 * Per layer: 4*32*2 = 256 slots; all 4 layers occupy tags 0..1023.
 * Preview tags start at 1024.
 */
static inline uint32_t seq_tag_on(uint8_t layer, uint8_t track, uint8_t step)
{
    return (uint32_t)layer * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
         + (uint32_t)track * SEQ_MAX_STEPS
         + step;
}

static inline uint32_t seq_tag_off(uint8_t layer, uint8_t track, uint8_t step)
{
    return seq_tag_on(layer, track, step)
         + (uint32_t)(SEQ_TRACKS * SEQ_MAX_STEPS);
}

static inline uint32_t seq_preview_tag(uint8_t layer, uint8_t track)
{
    return (uint32_t)MAX_LAYERS * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
         + (uint32_t)layer * SEQ_TRACKS
         + track;
}

/* OFF tag for the preview note — occupies the block immediately after ON tags. */
static inline uint32_t seq_preview_off_tag(uint8_t layer, uint8_t track)
{
    return seq_preview_tag(layer, track) + (uint32_t)(MAX_LAYERS * SEQ_TRACKS);
}

/* Swing / shuffle: delay ODD 16th-steps by swing_pct% of one step so the
 * off-beats land late, giving a shuffled groove. Even steps (the on-beats) are
 * untouched. Pure function of the step index + the layer's swing_pct, so the
 * emitted schedule stays beat-locked and tempo-independent (it is expressed in
 * ticks, exactly like the drone stutter-grid swing in drone_core.c). Integer
 * math only — this runs on the Core-0 emit path, never in render/ISR.
 * swing_pct==0 (the memset-zero default) returns 0 => bit-identical to the
 * pre-swing schedule. swing_pct is clamped to SEQ_SWING_MAX (<100) so the
 * result is always a fraction of one step and never crosses the next step. */
static inline uint32_t sequencer_step_swing_offset(const seq_layer_t *layer,
                                                   uint8_t step)
{
    if ((step & 1u) == 0u || layer->swing_pct == 0) return 0;
    return ((uint32_t)SEQ_TICKS_PER_STEP * (uint32_t)layer->swing_pct) / 100u;
}

float sequencer_step_velocity(const seq_layer_t *layer,
                              uint8_t track, uint8_t step)
{
    /* Drums now share the melodic accent+jitter curve for a less "machine-gun"
     * groove (the old fixed 1.0 made every hit identical). Falls through to the
     * same expressive path as melodic below. */
    (void)layer;

#if !CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
    (void)track;
    (void)step;
    return 1.0f;
#else

    /* With the EG0 envelope now shaping onset/tail, we can use a wider dynamic
     * range without the notes sounding dull — the accent pattern provides the
     * groove that breaks up the old "machine-gun" monotony. Base level sits
     * mid-range so accents have room to push up and ghost notes can drop down.
     * Tracks are spread slightly so stacked voices don't all hit identically. */
    float velocity = 0.62f + (0.02f * (float)track);

    /* Metric accents: strong downbeat, lighter backbeat, weak off-beats. */
    if ((step % 4) == 0) {
        velocity += 0.30f; /* downbeat of each quarter-note */
    } else if ((step % 4) == 2) {
        velocity += 0.16f; /* backbeat emphasis */
    } else {
        velocity -= 0.04f; /* the in-between 8ths sit back as ghost notes */
    }

    /* Deterministic per-step jitter so repeated bars are not bit-identical
     * (light "humanization"). Cycles every 4 steps with a small +/- swing. */
    static const float jitter[4] = { 0.015f, -0.02f, 0.01f, -0.015f };
    velocity += jitter[step & 3];

    velocity = SEQ_CLAMP_F32(velocity, 0.45f, 1.0f);
    return velocity;
#endif
}

/* True when any track in the layer has solo engaged (scans all SEQ_TRACKS,
 * not just num_tracks, so a stale solo flag on an unused track slot can never
 * silently affect audibility of the tracks actually in use). */
static bool sequencer_layer_has_solo(const seq_layer_t *layer)
{
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (layer->solo[t]) return true;
    }
    return false;
}

/* Whether `track` will actually sound: solo overrides mute (even on the same
 * track) whenever any track in the layer is soloed; otherwise mute alone gates.
 * Not static: also called from seq_core_trig.c's decorated-step ratchet path. */
bool sequencer_track_audible(const seq_layer_t *layer, uint8_t track)
{
    if (sequencer_layer_has_solo(layer)) {
        return layer->solo[track];
    }
    return !layer->mute[track];
}

static uint8_t sequencer_clamp_layer_note(const seq_layer_t *layer, uint8_t note)
{
    if (layer->type == SEQ_LAYER_DRUM) {
        return SEQ_CLAMP_U8(note, SEQ_MIDI_NOTE_MIN, SEQ_MIDI_NOTE_MAX);
    } else {
        return SEQ_CLAMP_U8(note, SEQ_MEL_NOTE_MIN, SEQ_MEL_NOTE_MAX);
    }
}

static uint8_t sequencer_resolve_track_note(const seq_layer_t *layer,
                                            uint8_t source_note)
{
    if (layer->type != SEQ_LAYER_MELODIC) {
        return sequencer_clamp_layer_note(layer, source_note);
    }

    /* Chord mode overrides the global scale quantizer for this layer. */
    if (layer->chord_mode) {
        uint8_t snapped = quantizer_snap_to_chord(source_note,
                                                  layer->chord_root,
                                                  layer->chord_type);
        return sequencer_clamp_layer_note(layer, snapped);
    }

    if (!s_quantizer.enabled) {
        return sequencer_clamp_layer_note(layer, source_note);
    }

    const musical_scale_t *scale = quantizer_get_scale(s_quantizer.scale_index);
    uint8_t snapped = quantizer_snap_midi_note(source_note, s_quantizer.root_note, scale);
    return sequencer_clamp_layer_note(layer, snapped);
}

/* ── Low-level AMY helpers ───────────────────────────────────────────── */

void sequencer_emit_clear_tag(uint32_t tag)
{
    amy_event *e = amy_helpers_event_begin();
    e->sequence[SEQUENCE_TAG]    = tag;
    e->sequence[SEQUENCE_TICK]   = 0;
    e->sequence[SEQUENCE_PERIOD] = 0;
    amy_helpers_event_send(e);
}

/* Schedule (or cancel) one grid step as a pair of repeating AMY events: a
 * note-on at the step's position in the bar, and a note-off `gate` ticks later.
 * Both repeat every `bar_ticks` so the pattern loops automatically. AMY keys
 * each event by its tag, so re-emitting with the same tag updates in place. */
void sequencer_emit_step(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    seq_layer_t *layer  = &s_layers[layer_idx];
    /* Total ticks in one loop of this layer's pattern. */
    uint32_t bar_ticks  = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
    /* Repeat rate: fire every N bars. period scales bar_ticks accordingly.
     * note-off must wrap against the same period so it lands in the correct
     * half of the extended window (not just within the first bar). */
    uint32_t rr         = (layer->repeat_rate[track] >= SEQ_REPEAT_2)
                          ? (uint32_t)layer->repeat_rate[track] : 1u;
    uint32_t period     = bar_ticks * rr;
    /* How long the note is held: drums are short/percussive, melodic longer. */
    uint8_t  gate       = (layer->type == SEQ_LAYER_DRUM)
                          ? SEQ_GATE_DRUM : SEQ_GATE_MELODIC;
    /* Melodic groove: shorten the off-beat 8ths a touch so accented downbeats
     * feel longer/legato while the in-between notes are slightly detached. We
     * only ever shorten (never lengthen past SEQ_GATE_MELODIC) so the note-off
     * always lands before the next step's note-on and never cuts it off. */
    if (layer->type == SEQ_LAYER_MELODIC && (step % 2) == 1 && gate > 2) {
        gate -= 2;
    }
    uint32_t tag_on     = seq_tag_on(layer_idx, track, step);
    uint32_t tag_off    = seq_tag_off(layer_idx, track, step);
    /* +1 so tick 0 stays reserved (AMY treats tick 0 specially as "clear").
     * Swing shifts odd steps later; even steps are unchanged (offset 0). The
     * note-off below is derived from tick_on, so it drags along by the same
     * offset — no separate swing edit is needed on the note-off. */
    uint32_t tick_on    = (uint32_t)(1 + step * SEQ_TICKS_PER_STEP)
                          + sequencer_step_swing_offset(layer, step);
    /* Note-off wraps within the full period (not just bar_ticks) so a note at
     * repeat_rate=2 whose gate spills past bar_ticks still fires correctly. */
    uint32_t tick_off   = (tick_on + gate) % period;
    float note_velocity = sequencer_step_velocity(layer, track, step);
    /* Apply per-track amplitude trim (default 1.0; set by graph editor amp mode). */
    note_velocity *= layer->amp_scale[track];
    if (note_velocity > 1.0f) note_velocity = 1.0f;
    if (tick_off == 0) tick_off = 1; /* avoid the reserved tick 0 */

    /* If stopped, this step is off, the track is muted/soloed-out, or the
     * step carries a probability/ratchet/conditional decoration, cancel the
     * plain periodic tag pair instead of emitting a note-on. Decorated steps
     * are one-shot scheduled per loop-iteration by
     * sequencer_core_service_tick() (seq_core_trig.c) instead, since AMY's
     * own period-repeat has no hook to gate an individual repetition. */
    if (!s_playing || !layer->grid[track][step] ||
        !sequencer_track_audible(layer, track) ||
        sequencer_core_step_is_decorated(layer, track, step)) {
        sequencer_emit_clear_tag(tag_on);
        sequencer_emit_clear_tag(tag_off);
        return;
    }

    /* Both drum and melodic layers now have one synth slot per track. */
    uint8_t synth = layer->synth_id[track];

    amy_send_note_sched(synth, layer->step_note[track][step], note_velocity,
                        tag_on, tick_on, period);
    amy_send_note_sched(synth, layer->step_note[track][step], 0.0f,
                        tag_off, tick_off, period);
}

/* Re-emit all steps for a layer (used on play-resume). */
void sequencer_resync_layer(uint8_t layer_idx)
{
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < layer->num_tracks; t++) {
        for (uint8_t s = 0; s < layer->num_steps; s++) {
            sequencer_emit_step(layer_idx, t, s);
        }
    }
}

/* Cancel all scheduled tags for a layer (used on pause). */
void sequencer_clear_layer_tags(uint8_t layer_idx)
{
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < layer->num_tracks; t++) {
        for (uint8_t s = 0; s < layer->num_steps; s++) {
            sequencer_emit_clear_tag(seq_tag_on(layer_idx, t, s));
            sequencer_emit_clear_tag(seq_tag_off(layer_idx, t, s));
        }
    }
}

/* Re-resolve a track's note (clamp + optional scale quantization), update every
 * step on that track to the new note, and re-emit them. When `preview` is set
 * (interactive editing) also fire a short one-shot so the user hears the note
 * immediately, even if quantization left the resolved note unchanged. */
static void sequencer_refresh_track_note(uint8_t layer_idx, uint8_t track,
                                        bool preview)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks) return;

    uint8_t source_note = s_track_source_note[layer_idx][track];
    uint8_t resolved_note = sequencer_resolve_track_note(layer, source_note);

    /* No change: skip the grid rewrite, but still preview so scrolling within
     * one scale degree remains audible. */
    if (layer->track_base_note[track] == resolved_note) {
        if (preview) {
            /* Keep the preview path active even when the snapped note does not change. */
        } else {
            return;
        }
    }

    /* Apply the resolved note to the whole track (all steps play one pitch). */
    layer->track_base_note[track] = resolved_note;
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        layer->step_note[track][s] = resolved_note;
    }

    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }

    if (!preview) {
        return;
    }

    /* One-shot preview: fires a few ticks from now using the same tag slot.
     * Rapid scrolling overwrites the slot so only the last change is heard. */
    uint32_t fire_tick = sequencer_ticks() + SEQ_PREVIEW_DELAY_TICKS;
    amy_send_note_sched(layer->synth_id[track], resolved_note, 1.0f,
                        seq_preview_tag(layer_idx, track), fire_tick, 0);
    amy_send_note_sched(layer->synth_id[track], resolved_note, 0.0f,
                        seq_preview_off_tag(layer_idx, track),
                        fire_tick + SEQ_GATE_MELODIC, 0);

    ESP_LOGI(TAG, "layer %d track %d note -> %d (preview @ tick %lu)",
             layer_idx, track, resolved_note, (unsigned long)fire_tick);
}

void sequencer_refresh_melodic_layers(bool preview)
{
    for (uint8_t layer_idx = 0; layer_idx < s_num_layers; layer_idx++) {
        seq_layer_t *layer = &s_layers[layer_idx];
        if (layer->type != SEQ_LAYER_MELODIC) {
            continue;
        }
        for (uint8_t track = 0; track < layer->num_tracks; track++) {
            sequencer_refresh_track_note(layer_idx, track, preview);
        }
    }
}

void sequencer_core_set_step(uint8_t layer_idx, uint8_t track,
                              uint8_t step, bool state)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    if (layer->grid[track][step] == state) return;
    layer->grid[track][step] = state;
    sequencer_emit_step(layer_idx, track, step);
}

/* Derive the currently-playing step from AMY's free-running tick counter:
 * position within the bar divided by ticks-per-step. When paused we return the
 * frozen value captured at pause time so the UI playhead stops in place. */
uint8_t sequencer_core_get_current_step(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    if (!s_playing) return s_cached_step[layer_idx];
    seq_layer_t *layer = &s_layers[layer_idx];
    uint32_t bar_ticks = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
    s_cached_step[layer_idx] =
        (uint8_t)((sequencer_ticks() % bar_ticks) / SEQ_TICKS_PER_STEP);
    return s_cached_step[layer_idx];
}

/* Start/stop playback. On start, every step is re-emitted so AMY repopulates
 * its schedule; on stop, each layer's playhead is captured (for a frozen UI)
 * and all scheduled events are cancelled so nothing keeps triggering. */
void sequencer_core_set_playing(bool p)
{
    if (s_playing == p) return;
    s_playing = p;
    if (s_playing) {
        /* Anchor the bar counter so bars_elapsed is relative to this play-start. */
        s_bar_baseline = sequencer_ticks();
        s_prog.entry_start_bar = 0;
        s_prog.current = 0;
        for (uint8_t i = 0; i < s_num_layers; i++) {
            sequencer_core_trig_reset(i);
            sequencer_resync_layer(i);
        }
    } else {
        /* Bug 1.1: clear arp scheduled events FIRST so repeating arp tags
         * don't keep firing while the sequencer is paused. */
        arp_core_clear_all();

        /* Freeze display positions before clearing scheduled events. */
        for (uint8_t i = 0; i < s_num_layers; i++) {
            seq_layer_t *layer = &s_layers[i];
            uint32_t bar_ticks = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
            s_cached_step[i] =
                (uint8_t)((sequencer_ticks() % bar_ticks) / SEQ_TICKS_PER_STEP);
            sequencer_clear_layer_tags(i);
            /* Bug-1.1-style fix, same rationale as arp_core_clear_all() above:
             * decorated steps' one-shot ratchet tags are not touched by
             * sequencer_clear_layer_tags() (different tag space), so clear
             * them explicitly or a pending sub-hit could still fire after
             * the user hits stop. */
            sequencer_core_trig_clear_all(i);
        }

        /* Bug 1.2: silence only the sequencer's own synth slots (melodic/drum
         * per-layer, plus arp slot 63). Drone slots 64-65 are intentionally
         * spared — they manage their own lifecycle and do not auto-resume, so
         * a global notes-off would permanently silence the drone. */
        for (uint8_t i = 0; i < s_num_layers; i++) {
            seq_layer_t *layer = &s_layers[i];
            for (uint8_t t = 0; t < layer->num_tracks; t++) {
                sequencer_kill_synth_voices(layer->synth_id[t]);
            }
        }
        sequencer_kill_synth_voices(SEQ_ARP_SYNTH);
    }
}

void sequencer_core_set_track_midi_note(uint8_t layer_idx, uint8_t track,
                                         uint8_t midi_note)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    midi_note = sequencer_clamp_layer_note(layer, midi_note);

    s_track_source_note[layer_idx][track] = midi_note;
    sequencer_refresh_track_note(layer_idx, track, true);
}

uint8_t sequencer_core_get_track_midi_note(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    return s_layers[layer_idx].track_base_note[track];
}

uint8_t sequencer_core_get_track_source_note(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    return s_track_source_note[layer_idx][track];
}

void sequencer_core_arp_emit_note(uint32_t tag_base, uint8_t midi_note,
                                  float velocity, uint32_t tick_on,
                                  uint32_t gate_ticks, uint32_t period)
{
    /* Defensive: never let an out-of-range tag reach AMY. Its sequences[] table
     * is sized to max_sequencer_tags and add_event has a `tag > max` off-by-one,
     * so a stray tag would smash the heap. Cap to the reserved arp window. */
    if (tag_base + 1 > SEQ_ARP_TAG_MAX) {
        ESP_LOGE(TAG, "arp tag %u out of range (max %u) - dropped",
                 (unsigned)tag_base, (unsigned)SEQ_ARP_TAG_MAX);
        return;
    }

    uint32_t tick_off = (period > 0) ? ((tick_on + gate_ticks) % period)
                                     : (tick_on + gate_ticks);
    if (tick_off == 0) tick_off = 1; /* tick 0 is reserved (clear) */
    if (tick_on  == 0) tick_on  = 1;

    amy_send_note_sched(SEQ_ARP_SYNTH, midi_note, velocity,
                        tag_base, tick_on, period);
    amy_send_note_sched(SEQ_ARP_SYNTH, midi_note, 0.0f,
                        tag_base + 1, tick_off, period);
}

void sequencer_core_arp_clear_note(uint32_t tag_base)
{
    sequencer_emit_clear_tag(tag_base);
    sequencer_emit_clear_tag(tag_base + 1);
}

void sequencer_core_set_track_repeat_rate(uint8_t layer_idx, uint8_t track,
                                          seq_repeat_rate_t rate)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    layer->repeat_rate[track] = (uint8_t)rate;
    /* Re-emit all steps on this track so AMY picks up the new period. */
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }
}

seq_repeat_rate_t sequencer_core_get_track_repeat_rate(uint8_t layer_idx,
                                                        uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return SEQ_REPEAT_1;
    uint8_t rr = s_layers[layer_idx].repeat_rate[track];
    switch (rr) {
        case 2: return SEQ_REPEAT_2;
        case 4: return SEQ_REPEAT_4;
        case 8: return SEQ_REPEAT_8;
        default: return SEQ_REPEAT_1;
    }
}

/* ── Per-layer swing ─────────────────────────────────────────────────────
 * Swing is a whole-layer feel control (not per-track): every odd step across
 * all tracks shifts by the same fraction, so the layer grooves as a unit.
 * Re-emit the entire layer so AMY re-schedules every step at its swung tick.
 *
 * TODO(ui): no UI wiring yet — this ships engine + public API only. A layer-
 * level menu item (swing is per-layer, not per-track) should call these from
 * the TrackOpts/UI dispatch once someone is in that screen code. */
void sequencer_core_set_layer_swing(uint8_t layer_idx, uint8_t swing_pct)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    uint8_t clamped = (uint8_t)SEQ_CLAMP_U8((int)swing_pct, 0, SEQ_SWING_MAX);
    if (layer->swing_pct == clamped) return;
    layer->swing_pct = clamped;
    sequencer_resync_layer(layer_idx);
}

uint8_t sequencer_core_get_layer_swing(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    return s_layers[layer_idx].swing_pct;
}

void sequencer_core_set_track_mute(uint8_t layer_idx, uint8_t track, bool mute)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->mute[track] == mute) return;
    layer->mute[track] = mute;
    /* Mute only ever changes this one track's own audibility. */
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }
    if (!sequencer_track_audible(layer, track)) {
        sequencer_kill_synth_voices(layer->synth_id[track]);
    }
}

bool sequencer_core_get_track_mute(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    return s_layers[layer_idx].mute[track];
}

void sequencer_core_set_track_solo(uint8_t layer_idx, uint8_t track, bool solo)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->solo[track] == solo) return;
    layer->solo[track] = solo;
    /* Solo changes every track's audibility in this layer, not just this
     * one's, so re-emit the whole layer and hard-kill whichever tracks just
     * became inaudible (a note already sounding would otherwise ring out
     * until its scheduled note-off, which re-emit alone does not force). */
    sequencer_resync_layer(layer_idx);
    for (uint8_t t = 0; t < layer->num_tracks; t++) {
        if (!sequencer_track_audible(layer, t)) {
            sequencer_kill_synth_voices(layer->synth_id[t]);
        }
    }
}

bool sequencer_core_get_track_solo(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    return s_layers[layer_idx].solo[track];
}
