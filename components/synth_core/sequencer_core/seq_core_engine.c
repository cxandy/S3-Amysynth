#include "sequencer_core/seq_core_internal.h"
#include "seq_clamp.h"

/* ── State definitions — owns step cache, source notes, bar baseline ── */
uint8_t  s_cached_step[MAX_LAYERS];
uint8_t  s_track_source_note[MAX_LAYERS][SEQ_TRACKS];
uint32_t s_bar_baseline = 0;
/* Last plain source note per track — chord-slot-delete fallback (see the
 * declaration comment in seq_core_internal.h). */
uint8_t  s_track_prev_plain[MAX_LAYERS][SEQ_TRACKS];

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

/* Chord edit-preview tags: extra-tone (1..SEQ_CHORD_MAX_NOTES-1) on/off pairs
 * for the one-shot preview path; tone 0 stays on the legacy preview pair
 * above. Layout comment: seq_core_config.h chord tag space. */
static inline uint32_t seq_chord_preview_on_tag(uint8_t layer, uint8_t track,
                                                uint8_t tone /* 1.. */)
{
    return SEQ_CHORD_PREVIEW_TAG_BASE
         + (((uint32_t)layer * SEQ_TRACKS + track) * (SEQ_CHORD_MAX_NOTES - 1)
            + (uint32_t)(tone - 1)) * 2u;
}

static inline uint32_t seq_chord_preview_off_tag(uint8_t layer, uint8_t track,
                                                 uint8_t tone /* 1.. */)
{
    return seq_chord_preview_on_tag(layer, track, tone) + 1u;
}

/* Clear the extra-tone preview pairs from `first_tone` up — run on every
 * chord preview (a shrink between two previews must not let a pending
 * higher-tone pair from the previous voicing fire) and when a track's
 * resolved note leaves the chord zone. */
static void seq_chord_preview_clear_from(uint8_t layer, uint8_t track,
                                         uint8_t first_tone)
{
    for (uint8_t tone = first_tone ? first_tone : 1; tone < SEQ_CHORD_MAX_NOTES; tone++) {
        sequencer_emit_clear_tag(seq_chord_preview_on_tag(layer, track, tone));
        sequencer_emit_clear_tag(seq_chord_preview_off_tag(layer, track, tone));
    }
}

/* ── Chord expansion (shared with seq_core_trig.c) ─────────────────────── */

/* Progression transpose for chord presets: presets are authored as the "I"
 * voicing (progression entry 0); each advance moves them by the delta between
 * the live chord root and entry 0's root, so the whole voicing tracks the bar
 * exactly as the single-note tracks do — but as a rigid transpose, never
 * per-tone re-quantization. With the progression off (including manual layer
 * chord snap), chords play exactly as entered. Computed at fire time, so a
 * progression advance re-pitches chords with zero re-emit plumbing. */
int sequencer_chord_transpose(const seq_layer_t *layer)
{
    if (!s_prog.enabled || s_prog.count == 0) return 0;
    if (!layer->chord_mode) return 0;
    return (int)layer->chord_root - (int)s_prog.entries[0].root;
}

uint8_t seq_track_fire_notes(const seq_layer_t *layer, uint8_t stored_note,
                             uint8_t out[SEQ_CHORD_MAX_NOTES])
{
    if (!SEQ_NOTE_IS_CHORD(stored_note)) {
        out[0] = stored_note;
        return 1;
    }
    return seq_chords_resolve(SEQ_CHORD_INDEX(stored_note),
                              sequencer_chord_transpose(layer), out);
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

    /* NoteFX GROOVE: blend between the full accent curve (100%) and flat
     * velocity 1.0 (0%). Applied after the clamp so 0% is exactly 1.0 and
     * 100% is bit-identical to the legacy curve. */
    velocity = 1.0f - ((float)layer->groove_pct * 0.01f) * (1.0f - velocity);
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

uint8_t sequencer_clamp_layer_note(const seq_layer_t *layer, uint8_t note)
{
    if (layer->type == SEQ_LAYER_DRUM) {
        return SEQ_CLAMP_U8(note, SEQ_MIDI_NOTE_MIN, SEQ_MIDI_NOTE_MAX);
    } else {
        return SEQ_CLAMP_U8(note, SEQ_MEL_NOTE_MIN, SEQ_MEL_NOTE_MAX);
    }
}

uint8_t sequencer_resolve_track_note(const seq_layer_t *layer,
                                     uint8_t source_note)
{
    if (layer->type != SEQ_LAYER_MELODIC) {
        return sequencer_clamp_layer_note(layer, source_note);
    }

    /* Chord preset sentinel: passes through untouched — no quantize, no clamp
     * (the clamp would smash it to SEQ_MEL_NOTE_MAX). Expansion to pitches
     * happens at fire time (seq_track_fire_notes); the quantizer/chord snap
     * below never applies to individual chord tones by design. */
    if (SEQ_NOTE_IS_CHORD(source_note)) {
        return source_note;
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
    /* Plain-trig note-hold incl. the melodic off-beat groove shortening;
     * shared with the ratchet n==1 path (seq_core_trig.c) via seq_step_gate. */
    uint8_t  gate       = (uint8_t)seq_step_gate(layer, step);
    uint32_t tag_on     = seq_tag_on(layer_idx, track, step);
    uint32_t tag_off    = seq_tag_off(layer_idx, track, step);
    /* +1 so tick 0 stays reserved (AMY treats tick 0 specially as "clear").
     * Two per-step timing offsets fold into the note-on tick: swing shifts odd
     * steps later, and micro-timing (patch-06) adds the signed per-step nudge on
     * top. Both are added into tick_on only; tick_off is derived from it below,
     * so gate length is preserved. A negative nudge on an early step can land
     * before the bar origin, so the combined value is wrapped into the tail of
     * the loop window (a late "drag"). Defaults (swing 0, nudge 0) leave the
     * on-grid tick byte-identical. */
    int32_t  tick_on_s  = (int32_t)(1 + step * SEQ_TICKS_PER_STEP)
                        + (int32_t)sequencer_step_swing_offset(layer, step)
                        + (int32_t)layer->step_nudge[track][step];
    while (tick_on_s < 1) tick_on_s += (int32_t)period;
    uint32_t tick_on    = (uint32_t)tick_on_s % period;
    if (tick_on == 0) tick_on = 1;
    /* Note-off wraps within the full period (not just bar_ticks) so a note at
     * repeat_rate=2 whose gate spills past bar_ticks still fires correctly. */
    uint32_t tick_off   = (tick_on + gate) % period;
    float note_velocity = sequencer_step_velocity(layer, track, step);
    /* Apply per-track amplitude trim (default 1.0; set by graph editor amp mode). */
    note_velocity *= layer->vp[track].amp_trim;
    /* Per-step velocity offset (patch-06): signed percentage points, default 0. */
    note_velocity += (float)layer->step_velocity_adj[track][step] * 0.01f;
    note_velocity = SEQ_CLAMP_F32(note_velocity, 0.0f, 1.0f);
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

    amy_helpers_note_send(synth, layer->step_note[track][step], note_velocity,
                        tag_on, tick_on, period);
    amy_helpers_note_send(synth, layer->step_note[track][step], 0.0f,
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
    } else if (SEQ_NOTE_IS_CHORD(layer->track_base_note[track])) {
        /* Leaving (or switching) a chord: kill this track's pending chord-tone
         * one-shots and extra-tone preview pairs so no scheduled tone from the
         * old voicing survives the transition (clear -> rebuild, every time). */
        sequencer_core_trig_clear_track_chord(layer_idx, track);
        seq_chord_preview_clear_from(layer_idx, track, 1);
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
     * Rapid scrolling overwrites the slot so only the last change is heard.
     * Velocity matches what a real downbeat step on this track would play
     * (groove velocity x amp trim) so the preview is honest about level.
     * A chord sentinel previews the full voicing (that IS the feature): tone 0
     * on the legacy preview pair, extra tones on the chord preview pairs, and
     * any pairs past the tone count cleared so a shrink between two previews
     * cannot leave a stale higher tone pending. */
    uint8_t tones[SEQ_CHORD_MAX_NOTES];
    uint8_t ntones = seq_track_fire_notes(layer, resolved_note, tones);
    if (ntones == 0) return;   /* undefined chord slot: nothing to audition */

    float preview_vel = sequencer_step_velocity(layer, track, 0)
                        * layer->vp[track].amp_trim;
    if (preview_vel > 1.0f) preview_vel = 1.0f;
    uint32_t fire_tick = sequencer_ticks() + SEQ_PREVIEW_DELAY_TICKS;
    uint32_t off_tick  = fire_tick + seq_step_gate(layer, 0);
    amy_helpers_note_send(layer->synth_id[track], tones[0], preview_vel,
                        seq_preview_tag(layer_idx, track), fire_tick, 0);
    amy_helpers_note_send(layer->synth_id[track], tones[0], 0.0f,
                        seq_preview_off_tag(layer_idx, track), off_tick, 0);
    for (uint8_t i = 1; i < ntones; i++) {
        amy_helpers_note_send(layer->synth_id[track], tones[i], preview_vel,
                            seq_chord_preview_on_tag(layer_idx, track, i),
                            fire_tick, 0);
        amy_helpers_note_send(layer->synth_id[track], tones[i], 0.0f,
                            seq_chord_preview_off_tag(layer_idx, track, i),
                            off_tick, 0);
    }
    if (SEQ_NOTE_IS_CHORD(resolved_note)) {
        seq_chord_preview_clear_from(layer_idx, track, ntones);
    }

    ESP_LOGI(TAG, "L%d T%d note -> %d (preview @ tick %lu)",
             layer_idx + 1, track + 1, resolved_note, (unsigned long)fire_tick);
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
    s_cached_step[layer_idx] = seq_playhead_step(layer, sequencer_ticks());
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
        /* Anchor the bar counter to the NEXT absolute bar boundary, not the raw
         * play-press tick. AMY fires periodic events on tick % period, so
         * pattern loops are phase-locked to the absolute tick grid; rounding up
         * makes every progression bar line coincide with step 0 of 16-step
         * patterns (32-step layers align every other loop boundary). The
         * partial pre-boundary stretch counts as bar 0 (bars_elapsed clamps),
         * so the first chord still gets its full duration. */
        uint32_t t = sequencer_ticks();
        s_bar_baseline = ((t + SEQ_TICKS_PER_BAR - 1) / SEQ_TICKS_PER_BAR)
                         * SEQ_TICKS_PER_BAR;
        s_prog.entry_start_bar = 0;
        s_prog.current = 0;
        /* The progression restarts from entry 0 on play, but the layers/arp may
         * still hold the chord of whichever entry was live at stop time —
         * request a re-apply so what is shown as active is also what sounds.
         * Drained by the progression service on synth_ui_task (single-applier).
         * Flagged immediate: this is a correction, not a musical edit, so it
         * must bypass the BAR launch-quantize hold. */
        if (s_prog.enabled) {
            s_prog_apply_immediate = true;
            s_prog_apply_pending = true;
        }
        for (uint8_t i = 0; i < s_num_layers; i++) {
            sequencer_core_trig_reset(i);
            sequencer_resync_layer(i);
        }
        /* The pause path cleared the arp schedule and emission is s_playing-
         * gated, so nothing re-armed it while stopped — request a coalesced
         * re-emit (drained by arp_core_service() on the UI task). */
        arp_core_mark_dirty();
    } else {
        /* Bug 1.1: clear arp scheduled events FIRST so repeating arp tags
         * don't keep firing while the sequencer is paused. */
        arp_core_clear_all();

        /* Freeze display positions before clearing scheduled events. */
        for (uint8_t i = 0; i < s_num_layers; i++) {
            seq_layer_t *layer = &s_layers[i];
            s_cached_step[i] = seq_playhead_step(layer, sequencer_ticks());
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
         * a global notes-off would permanently silence the drone.
         
         This is dumb - God
         
         */
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

    /* A defined chord sentinel is a valid melodic assignment and must not be
     * range-clamped (that would smash it to SEQ_MEL_NOTE_MAX). Anything else
     * — plain notes, sentinels on drum layers, sentinels for undefined slots
     * — takes the normal clamp. */
    bool chord_ok = layer->type == SEQ_LAYER_MELODIC &&
                    SEQ_NOTE_IS_CHORD(midi_note) &&
                    seq_chords_is_defined(SEQ_CHORD_INDEX(midi_note));
    if (!chord_ok) {
        midi_note = sequencer_clamp_layer_note(layer, midi_note);
        /* Remember the last plain choice: the fallback if a chord slot this
         * track later references gets deleted. */
        s_track_prev_plain[layer_idx][track] = midi_note;
    }

    s_track_source_note[layer_idx][track] = midi_note;
    sequencer_refresh_track_note(layer_idx, track, true);

    /* Chord assignment can widen (or release) this track's voice need beyond
     * the layer's configured count. Reconfigure through the proven paused
     * clear -> configure -> resync path — same discipline as patch cycling,
     * and only when the need actually changed (menu-time cost only). */
    if (layer->type == SEQ_LAYER_MELODIC && sequencer_layer_voices_stale(layer_idx)) {
        sequencer_reconfigure_layer_paused(layer_idx);
    }
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

/* Chord table edit sweep (called by seq_chords_set/clear on the UI task).
 * Voicing edits need no re-emit — fires read the table live — but pending
 * one-shots from the old voicing are cleared, a now-undefined slot drops
 * referencing tracks back to their last plain note, and a changed tone count
 * reconfigures the layer's voice budget. */
void sequencer_core_chord_slot_changed(uint8_t idx)
{
    if (idx >= SEQ_CHORD_SLOTS) return;
    uint8_t sentinel = SEQ_CHORD_NOTE(idx);
    bool defined = seq_chords_is_defined(idx);

    for (uint8_t li = 0; li < s_num_layers; li++) {
        seq_layer_t *layer = &s_layers[li];
        if (layer->type != SEQ_LAYER_MELODIC) continue;
        bool touched = false;
        for (uint8_t t = 0; t < layer->num_tracks; t++) {
            if (s_track_source_note[li][t] != sentinel) continue;
            touched = true;
            sequencer_core_trig_clear_track_chord(li, t);
            seq_chord_preview_clear_from(li, t, 1);
            if (!defined) {
                /* Never leave a track silently referencing an empty slot. */
                uint8_t fb = s_track_prev_plain[li][t];
                if (fb == 0) fb = 60;   /* C4 when no plain note was ever set */
                s_track_source_note[li][t] =
                    sequencer_clamp_layer_note(layer, fb);
            }
            sequencer_refresh_track_note(li, t, false);
        }
        if (touched && sequencer_layer_voices_stale(li)) {
            sequencer_reconfigure_layer_paused(li);
        }
    }
}

/* Chord-editor audition: one-shot the (possibly not-yet-saved) voicing
 * through a melodic track's synth via the preview tag machinery. Plays the
 * tones exactly as authored (no progression transpose — the editor edits the
 * "I" voicing). Honest caveat: a voicing wider than the track's current voice
 * count will voice-steal until it is actually assigned (assignment widens the
 * count); still honest about timbre, which is the point of the audition. */
void sequencer_core_audition_chord(uint8_t layer_idx, uint8_t track,
                                   const seq_chord_t *chord)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS || chord == NULL) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;

    uint8_t tones[SEQ_CHORD_MAX_NOTES];
    uint8_t ntones = 0;
    for (uint8_t i = 0; i < SEQ_CHORD_MAX_NOTES; i++) {
        if (chord->notes[i] == 0) continue;
        tones[ntones++] = sequencer_core_clamp_melodic_note(chord->notes[i]);
    }
    seq_chord_preview_clear_from(layer_idx, track, ntones ? ntones : 1);
    if (ntones == 0) return;

    float preview_vel = sequencer_step_velocity(layer, track, 0)
                        * layer->vp[track].amp_trim;
    if (preview_vel > 1.0f) preview_vel = 1.0f;
    uint32_t fire_tick = sequencer_ticks() + SEQ_PREVIEW_DELAY_TICKS;
    uint32_t off_tick  = fire_tick + seq_step_gate(layer, 0);
    amy_helpers_note_send(layer->synth_id[track], tones[0], preview_vel,
                        seq_preview_tag(layer_idx, track), fire_tick, 0);
    amy_helpers_note_send(layer->synth_id[track], tones[0], 0.0f,
                        seq_preview_off_tag(layer_idx, track), off_tick, 0);
    for (uint8_t i = 1; i < ntones; i++) {
        amy_helpers_note_send(layer->synth_id[track], tones[i], preview_vel,
                            seq_chord_preview_on_tag(layer_idx, track, i),
                            fire_tick, 0);
        amy_helpers_note_send(layer->synth_id[track], tones[i], 0.0f,
                            seq_chord_preview_off_tag(layer_idx, track, i),
                            off_tick, 0);
    }
}

void sequencer_core_arp_emit_note(uint32_t tag_base, uint8_t midi_note,
                                  float velocity, uint32_t tick_on,
                                  uint32_t gate_ticks, uint32_t period)
{
    /* Slaved to the transport, same as sequencer_emit_step(): while paused
     * nothing may (re)arm the arp's repeating schedule — otherwise any arp
     * refresh during a pause (param edit, progression apply, project load)
     * leaves the arp playing alone. The resume path in
     * sequencer_core_set_playing() marks the arp dirty so the schedule is
     * rebuilt when playback restarts; periodic events are phase-locked to the
     * absolute tick grid, so the rebuild lands back in sync. */
    if (!s_playing) return;

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

    amy_helpers_note_send(SEQ_ARP_SYNTH, midi_note, velocity,
                        tag_base, tick_on, period);
    amy_helpers_note_send(SEQ_ARP_SYNTH, midi_note, 0.0f,
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

/* ── Per-layer melodic NoteFX: gate length + glide (portamento) ────────────
 * Both are per-layer scalars edited from the NoteFX menu page. Gate is applied
 * at emit time (seq_step_gate), so a change must re-emit the layer's steps so
 * the new note-off ticks take effect immediately, exactly like swing. Glide is
 * an AMY per-osc setting, pushed straight to the row synths. Both are no-ops on
 * drum layers (gate uses the fixed SEQ_GATE_DRUM; drums don't glide). */
void sequencer_core_set_melodic_gate_pct(uint8_t layer_idx, uint8_t gate_pct)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;
    uint8_t clamped = (uint8_t)SEQ_CLAMP_U8((int)gate_pct, 10, 100);
    if (layer->gate_pct == clamped) return;
    layer->gate_pct = clamped;
    sequencer_resync_layer(layer_idx);   /* re-emit: gate changes note-off ticks */
}

uint8_t sequencer_core_get_melodic_gate_pct(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    return s_layers[layer_idx].gate_pct;
}

void sequencer_core_set_melodic_portamento_ms(uint8_t layer_idx, uint16_t ms)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;
    uint16_t clamped = (uint16_t)SEQ_CLAMP_U16((int)ms, 0, SEQ_MELODIC_PORTAMENTO_MAX_MS);
    if (layer->portamento_ms == clamped) return;
    layer->portamento_ms = clamped;
    sequencer_core_push_melodic_portamento(layer_idx);
}

uint16_t sequencer_core_get_melodic_portamento_ms(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    return s_layers[layer_idx].portamento_ms;
}

void sequencer_core_set_melodic_groove_pct(uint8_t layer_idx, uint8_t groove_pct)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;
    uint8_t clamped = (uint8_t)SEQ_CLAMP_U8((int)groove_pct, 0, 100);
    if (layer->groove_pct == clamped) return;
    layer->groove_pct = clamped;
    sequencer_resync_layer(layer_idx);   /* re-emit: velocity is baked at emit */
}

uint8_t sequencer_core_get_melodic_groove_pct(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    return s_layers[layer_idx].groove_pct;
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
