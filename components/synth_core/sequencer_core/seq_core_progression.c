#include "sequencer_core/seq_core_internal.h"

/* ── State definitions — owns chord progression ──────────────────────── */
chord_progression_t s_prog = {
    .entries = {
        { .root = 0, .chord_type = CHORD_MAJ7, .duration_bars = 1 },  /* 1st chord -> 1b */
    },
    .count   = 1,
    .current = 0,
    .entry_start_bar = 0,
    .enabled = false,
};

/* Set by input-task entry points (encoder_task / button callback) that change
 * chord state; consumed once per tick by sequencer_core_progression_service()
 * which runs in synth_ui_task. This makes synth_ui_task the SINGLE task that
 * calls chord_progression_apply_current() -> sequencer_refresh_melodic_layers()
 * -> AMY emit, so progression/manual-chord edits never race the periodic
 * advance or each other across tasks. */
volatile bool s_prog_apply_pending = false;

/* Companion to s_prog_apply_pending: set alongside it when the apply is a
 * correction (play-start re-sync) rather than a musical edit, so it bypasses
 * the BAR launch-quantize hold and lands on the next service tick. */
volatile bool s_prog_apply_immediate = false;

/* BAR launch mode: bar index at which a held pending apply was armed;
 * UINT32_MAX = not armed. Drains once bars_elapsed moves past this value. */
static uint32_t s_apply_armed_bar = UINT32_MAX;

/* ── Private helpers ─────────────────────────────────────────────────── */

/* Map a chord type to the closest diatonic scale for arp snap quality. */
static uint8_t chord_type_to_scale_index(chord_type_t ct)
{
    /* Scale indices mirror s_scales[] in quantizer.c:
     * 0=Chromatic, 1=Major, 2=Natural Minor, 3=Dorian, 4=Phrygian,
     * 5=Lydian, 6=Mixolydian, 7=Minor Pent, 8=Major Pent,
     * 9=Harmonic Minor, 10=Locrian, 11=Whole Tone */
    switch (ct) {
        case CHORD_MAJ:  return 1;
        case CHORD_MIN:  return 2;
        case CHORD_MAJ7: return 1;
        case CHORD_MIN7: return 3;  /* Dorian has the natural 6 common in min7 contexts */
        case CHORD_DOM7: return 6;  /* Mixolydian */
        case CHORD_SUS2: return 8;  /* Major Pentatonic — open, no 3rd */
        case CHORD_SUS4: return 8;
        case CHORD_DIM:  return 10; /* Locrian — its b5 IS the chord tone; Phrygian's
                                       natural 5 clashed a semitone against it */
        case CHORD_AUG:  return 11; /* Whole Tone — contains the #5; Lydian's
                                       natural 5 clashed the same way */
        case CHORD_MIN9: return 3;  /* Dorian — same family as min7 */
        case CHORD_MAJ9: return 1;  /* Major */
        default:         return 1;
    }
}

void chord_progression_apply_current(void)
{
    if (s_prog.count == 0) return;
    const chord_prog_entry_t *e = &s_prog.entries[s_prog.current];

    /* Update every melodic layer's chord state and re-resolve all tracks. */
    for (uint8_t li = 0; li < s_num_layers; li++) {
        seq_layer_t *layer = &s_layers[li];
        if (layer->type != SEQ_LAYER_MELODIC) continue;
        layer->chord_mode  = true;
        layer->chord_root  = e->root;
        layer->chord_type  = e->chord_type;
    }
    sequencer_refresh_melodic_layers(false);

    /* Drive arp root + scale to match the new chord, capturing the user's own
     * values the first time the progression takes over. Restored when the
     * progression is disabled so a session never permanently clobbers them. */
    if (!s_prog.arp_saved) {
        s_prog.saved_arp_root  = arp_get_root_note();
        s_prog.saved_arp_scale = arp_get_scale();
        s_prog.arp_saved = true;
    }
    arp_set_root_note((uint8_t)(e->root + 60));   /* pitch class → MIDI octave 4 */
    arp_set_scale(chord_type_to_scale_index(e->chord_type));
}

/* Called from synth_ui_task at 20 Hz. This is the SINGLE task that emits chord
 * changes to AMY: it both (a) drains s_prog_apply_pending, which input-task entry
 * points (encoder_task / button callback) set after mutating chord state, and
 * (b) advances the progression when the current entry expires. Funnelling every
 * chord_progression_apply_current() / sequencer_refresh_melodic_layers() emit
 * through this one task means edits never race the periodic advance or each other
 * across tasks — no lock needed because there is only one writer of the emit. */
void sequencer_core_progression_service(void)
{
    /* Drain deferred chord applies first, regardless of playing/enabled state.
     * In BAR launch mode a musical edit is held until the next bar line while
     * playing; corrections (s_prog_apply_immediate) and drains while stopped
     * bypass the hold. */
    if (s_prog_apply_pending) {
        bool drain_now = true;
        if (s_prog.apply_at_bar && s_playing && !s_prog_apply_immediate) {
            uint32_t bars = sequencer_bars_elapsed();
            if (s_apply_armed_bar == UINT32_MAX) s_apply_armed_bar = bars; /* arm */
            drain_now = (bars != s_apply_armed_bar); /* crossed a bar line */
        }
        if (drain_now) {
            s_apply_armed_bar = UINT32_MAX;
            s_prog_apply_pending = false;
            s_prog_apply_immediate = false;
            if (s_prog.enabled && s_prog.count > 0) {
                chord_progression_apply_current();
            } else {
                /* Disabled (or empty): layers already had chord_mode cleared by
                 * the caller, or a manual per-layer chord was set/cleared;
                 * re-resolve all melodic layers against their own current
                 * chord/scale state. */
                sequencer_refresh_melodic_layers(false);
            }
        }
    } else {
        s_apply_armed_bar = UINT32_MAX;
    }

    if (!s_prog.enabled || s_prog.count == 0 || !s_playing) return;

    uint32_t bars = sequencer_bars_elapsed();
    /* Mid-session anchors point at the NEXT bar line (entry_start_bar = bars+1);
     * the unsigned subtraction below would wrap huge until that line passes. */
    if (bars < s_prog.entry_start_bar) return;
    const chord_prog_entry_t *e = &s_prog.entries[s_prog.current];

    if (bars - s_prog.entry_start_bar >= e->duration_bars) {
        uint8_t next = (uint8_t)((s_prog.current + 1) % s_prog.count);
        s_prog.current = next;
        /* Advance by the expiring entry's duration (never "= bars"): a service
         * stall longer than a bar then catches up over successive ticks instead
         * of permanently shifting the form. */
        s_prog.entry_start_bar += e->duration_bars;
        chord_progression_apply_current();
        ESP_LOGI(TAG, "progression -> entry %u (root=%u type=%u)",
                 next, s_prog.entries[next].root, (unsigned)s_prog.entries[next].chord_type);
    }
}

/* ── Progression public API ─────────────────────────────────────────────── */

void sequencer_core_progression_set_enabled(bool en)
{
    s_prog.enabled = en;
    if (en && s_prog.count > 0) {
        /* The chord applies now (or at the next bar line in BAR mode), but its
         * duration window counts from the NEXT bar line, so a mid-bar enable
         * still gives the first entry its full duration (the partial bar is a
         * free lead-in). The service guards bars < entry_start_bar. */
        s_prog.current = 0;
        s_prog.entry_start_bar = sequencer_bars_elapsed() + 1;
        /* Defer the AMY emit to the service tick (single-applier). */
        s_prog_apply_pending = true;
    } else if (!en) {
        /* Disable chord mode on all melodic layers so they return to scale
         * quantizer; defer the re-resolve emit to the service tick. */
        for (uint8_t li = 0; li < s_num_layers; li++) {
            s_layers[li].chord_mode = false;
        }
        /* Return the arp to the user's pre-progression root/scale. State-only
         * setters (the arp marks itself dirty; the re-emit happens in
         * arp_core_service on the UI task), so single-applier holds. */
        if (s_prog.arp_saved) {
            arp_set_root_note(s_prog.saved_arp_root);
            arp_set_scale(s_prog.saved_arp_scale);
            s_prog.arp_saved = false;
        }
        s_prog_apply_pending = true;
    }
}

bool sequencer_core_progression_get_enabled(void) { return s_prog.enabled; }

/* Launch quantization: false = chord applies land on the next service tick
 * (instant), true = musical edits hold until the next bar line while playing. */
void sequencer_core_progression_set_apply_at_bar(bool at_bar)
{
    s_prog.apply_at_bar = at_bar;
}

bool sequencer_core_progression_get_apply_at_bar(void)
{
    return s_prog.apply_at_bar;
}

/* Drop the captured pre-progression arp root/scale WITHOUT restoring it.
 * Project load calls this before applying the loaded progression state: the
 * arp values just loaded from the snapshot are the new user baseline, and a
 * stale capture from the pre-load session must not be restored over them. */
void sequencer_core_progression_reset_arp_capture(void)
{
    s_prog.arp_saved = false;
}

void sequencer_core_progression_set_entry(uint8_t idx, uint8_t root,
                                          chord_type_t chord_type,
                                          uint8_t duration_bars)
{
    if (idx >= CHORD_PROG_MAX_ENTRIES) return;
    /* Refuse holes: idx == count appends, anything past that would legalize the
     * zero-filled entries in between (duration_bars 0 -> the service would
     * advance through them every tick, a 20 Hz chord strobe). */
    if (idx > s_prog.count) return;
    s_prog.entries[idx].root          = root % 12;
    s_prog.entries[idx].chord_type    = (chord_type < CHORD_TYPE_COUNT)
                                        ? chord_type : CHORD_MAJ;
    s_prog.entries[idx].duration_bars = (duration_bars > 0) ? duration_bars : 4;
    if (idx >= s_prog.count) s_prog.count = (uint8_t)(idx + 1);
    /* If we edited the live (currently playing) entry, defer a re-apply so the
     * audible chord tracks the edit. */
    if (s_prog.enabled && idx == s_prog.current) s_prog_apply_pending = true;
}

void sequencer_core_progression_get_entry(uint8_t idx, uint8_t *root,
                                          chord_type_t *chord_type,
                                          uint8_t *duration_bars)
{
    /* Safe defaults first: an out-of-range idx must not leave the caller's
     * (possibly uninitialized) locals unwritten. */
    if (root)          *root          = 0;
    if (chord_type)    *chord_type    = CHORD_MAJ;
    if (duration_bars) *duration_bars = 1;
    if (idx >= s_prog.count || idx >= CHORD_PROG_MAX_ENTRIES) return;
    if (root)          *root          = s_prog.entries[idx].root;
    if (chord_type)    *chord_type    = s_prog.entries[idx].chord_type;
    if (duration_bars) *duration_bars = s_prog.entries[idx].duration_bars;
}

void sequencer_core_progression_set_count(uint8_t count)
{
    if (count > CHORD_PROG_MAX_ENTRIES) count = CHORD_PROG_MAX_ENTRIES;
    s_prog.count = count;
    /* If the active entry fell out of range, wrap to 0, restart its bar window
     * (counting from the next bar line, matching the enable anchor), and
     * re-apply so the audible chord follows the new active entry. */
    if (s_prog.current >= s_prog.count && s_prog.count > 0) {
        s_prog.current = 0;
        s_prog.entry_start_bar = sequencer_bars_elapsed() + 1;
        if (s_prog.enabled) s_prog_apply_pending = true;
    }
}

uint8_t sequencer_core_progression_get_count(void) { return s_prog.count; }
uint8_t sequencer_core_progression_get_current(void) { return s_prog.current; }
uint8_t sequencer_core_progression_get_max(void) { return CHORD_PROG_MAX_ENTRIES; }

/* Bars elapsed within the currently-playing entry (0-based), for the UI status bar. */
uint8_t sequencer_core_progression_bars_in_current(void)
{
    /* Ticks are monotonic through stop, so without this gate the counter keeps
     * climbing (and uint8-wraps) while the sequencer is paused. */
    if (!s_playing) return 0;
    if (!s_prog.enabled || s_prog.count == 0) return 0;
    uint32_t bars = sequencer_bars_elapsed();
    if (bars < s_prog.entry_start_bar) return 0;   /* baseline just moved */
    return (uint8_t)(bars - s_prog.entry_start_bar);
}

/* Append a default entry (Cmaj) if room remains. Returns true on success.
 * Convenience: the Nth chord defaults to an N-bar duration (1st=1b, 2nd=2b, …),
 * so a fresh progression staircases 1/2/3/4 bars without manual editing. Beyond
 * the 4th entry the duration clamps to 4 bars — 5/6/7 aren't in the selectable
 * duration set {1,2,3,4,8,16}, and the user still edits any of these by hand. */
bool sequencer_core_progression_add_entry(void)
{
    if (s_prog.count >= CHORD_PROG_MAX_ENTRIES) return false;
    uint8_t idx = s_prog.count;
    uint8_t entry_num = (uint8_t)(idx + 1);         /* 1-based position */
    s_prog.entries[idx].root          = 0;          /* C */
    s_prog.entries[idx].chord_type    = CHORD_MAJ;
    s_prog.entries[idx].duration_bars = entry_num <= 4 ? entry_num : 4;
    s_prog.count = entry_num;
    return true;
}

/* Delete entry idx, shifting later entries down. Keeps at least one entry. */
void sequencer_core_progression_delete_entry(uint8_t idx)
{
    if (idx >= s_prog.count || s_prog.count <= 1) return;
    for (uint8_t i = idx; i + 1 < s_prog.count; i++) {
        s_prog.entries[i] = s_prog.entries[i + 1];
    }
    s_prog.count--;
    /* Fix up the active index/window if it was at or past the deletion point. */
    bool active_changed = false;
    if (s_prog.current == idx) {
        if (s_prog.current >= s_prog.count) s_prog.current = 0;
        active_changed = true;
    } else if (s_prog.current > idx) {
        s_prog.current--;   /* same entry, new slot — no audible change */
    }
    if (active_changed) {
        s_prog.entry_start_bar = sequencer_bars_elapsed() + 1;
        if (s_prog.enabled) s_prog_apply_pending = true;
    }
}

void sequencer_core_progression_set_layer_chord(uint8_t layer_idx,
                                                uint8_t root,
                                                chord_type_t chord_type)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;
    layer->chord_mode = true;
    layer->chord_root = root % 12;
    layer->chord_type = chord_type;
    /* Defer the re-resolve emit to the service tick (single-applier). */
    s_prog_apply_pending = true;
}

void sequencer_core_progression_clear_layer_chord(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return;
    s_layers[layer_idx].chord_mode = false;
    /* Defer the re-resolve emit to the service tick (single-applier). */
    s_prog_apply_pending = true;
}

void sequencer_core_get_layer_chord(uint8_t layer_idx, bool *chord_mode,
                                    uint8_t *root, chord_type_t *chord_type)
{
    if (layer_idx >= s_num_layers) return;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (chord_mode) *chord_mode = layer->chord_mode;
    if (root)       *root       = layer->chord_root;
    if (chord_type) *chord_type = layer->chord_type;
}
