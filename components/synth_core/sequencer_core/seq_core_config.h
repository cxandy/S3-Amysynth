#pragma once

/* ── Kconfig defaults ────────────────────────────────────────────────────── */
#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED
#define CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED 1
#endif
#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE
#define CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE 60
#endif
#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE
#define CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE 0
#endif
#ifndef CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
#define CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS 1
#endif
#ifndef CONFIG_SEQ_MELODIC_GATE_NUMERATOR
#define CONFIG_SEQ_MELODIC_GATE_NUMERATOR 5
#endif
#ifndef CONFIG_SEQ_MELODIC_GATE_DENOMINATOR
#define CONFIG_SEQ_MELODIC_GATE_DENOMINATOR 6
#endif
#ifndef CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED
#define CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED 1
#endif
#ifndef CONFIG_SEQ_MELODIC_PATCH
#define CONFIG_SEQ_MELODIC_PATCH 138
#endif
#ifndef CONFIG_SEQ_DRUM_GATE_NUMERATOR
#define CONFIG_SEQ_DRUM_GATE_NUMERATOR 1
#endif
#ifndef CONFIG_SEQ_DRUM_GATE_DENOMINATOR
#define CONFIG_SEQ_DRUM_GATE_DENOMINATOR 2
#endif
#ifndef CONFIG_SEQ_ENV_DEBUG_DUMP
#define CONFIG_SEQ_ENV_DEBUG_DUMP 0
#endif

/* ── Timing ──────────────────────────────────────────────────────────── */
#define SEQ_TICKS_PER_STEP    (AMY_SEQUENCER_PPQ / 4)
/* A musical bar = 16 steps. Fixed regardless of layer length so the bar
 * counter and repeat-rate are independent of which layers are active. */
#define SEQ_TICKS_PER_BAR     (16u * SEQ_TICKS_PER_STEP)
/* Per-step micro-timing (swing/humanize) range in sequencer ticks, folded in
 * only at step->absolute-tick conversion in sequencer_emit_step(). +-6 ticks
 * is half a step (SEQ_TICKS_PER_STEP=12) — a strong but musical push/drag, and
 * safely below the smallest loop period (16*12=192). Chosen range, NOT a
 * hardware-confirmed spec; the future step_nudge setter clamps to +-this. */
#define SEQ_STEP_NUDGE_MAX    6
/* Drum gate: fraction of a step the note is held before its note-off. Now that
 * drums are real Juno patches (note-offs honored), this controls choke vs. ring;
 * the patch's own release tail still plays out after note-off. Tunable via
 * Kconfig (default 1/2 step). */
#define SEQ_GATE_DRUM         ((SEQ_TICKS_PER_STEP * CONFIG_SEQ_DRUM_GATE_NUMERATOR) / CONFIG_SEQ_DRUM_GATE_DENOMINATOR)
#if CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
#define SEQ_GATE_MELODIC      ((SEQ_TICKS_PER_STEP * CONFIG_SEQ_MELODIC_GATE_NUMERATOR) / CONFIG_SEQ_MELODIC_GATE_DENOMINATOR)
#else
#define SEQ_GATE_MELODIC      ((SEQ_TICKS_PER_STEP * 2) / 3)
#endif
#define SEQ_MIN_BPM           40
#define SEQ_MAX_BPM           300
/* SEQ_DEFAULT_BPM is declared in sequencer_core.h (shared with synth_ui). */

/* ── Drum synth slots ────────────────────────────────────────────────────
 * Drums are now a per-track Juno-patch layer: each of the 4 tracks loads its
 * own AMY patch and gets its own synth slot, exactly like melodic rows. We
 * reserve a fixed block 6..9 (below the melodic base of 11) so the melodic
 * running allocator (11..62) and the arp slot (63) are untouched. */
#define SEQ_DRUM_SYNTH_BASE   6
#define SEQ_DRUM_VOICES       1  /* one voice per row; a row sounds one pitch at
                                  * a time (matches melodic). */
/* Drum tracks now play real pitches into a tonal patch, so clamp to the same
 * musical range as melodic rows rather than the old GM-drum note span. */
#define SEQ_MIDI_NOTE_MIN     24    /* C1 */
#define SEQ_MIDI_NOTE_MAX     96    /* C7 */

/* ── Melodic synth defaults ──────────────────────────────────────────── */
#define SEQ_MEL_PATCH         CONFIG_SEQ_MELODIC_PATCH
/* Wave-patch ID constants (SEQ_PATCH_WAVE_BASE, SEQ_PATCH_WAVE_MAX, etc.)
 * are now in sequencer_core.h so arp_core and drone_core can use them. */
/* One AMY synth PER ROW (per track). A row only ever sounds one pitch at a
 * time, so a single voice suffices; bump to 2 to give note-off/note-on overlap
 * headroom at the boundary (2x osc cost). AMY default budget is 250 oscs. */
#define SEQ_MEL_VOICES        1
#define SEQ_MEL_SYNTH_BASE    11    /* first melodic synth slot (drum = 10) */
#define SEQ_MAX_SYNTH         62    /* melodic ceiling; slot 63 reserved for arp */
#define SEQ_MEL_NOTE_MIN      24    /* C1 */
#define SEQ_MEL_NOTE_MAX      96    /* C7 */

/* ── Arpeggiator synth slot ──────────────────────────────────────────────
 * Dedicated AMY slot for the standalone arp, reserved above the melodic
 * ceiling so it never collides with a melodic layer's per-row block. */
#define SEQ_ARP_SYNTH         63
#define SEQ_ARP_VOICES        4     /* allow note overlap at fast rates       */

/* One-shot preview fires this many ticks after an adjustment */
#define SEQ_PREVIEW_DELAY_TICKS 4

/* ── Arpeggiator tag space ───────────────────────────────────────────────
 * Arp tags sit just above the sequencer's tag space. The sequencer uses:
 *   step on/off : 0 .. MAX_LAYERS*SEQ_TRACKS*SEQ_MAX_STEPS*2 - 1   (0..1023)
 *   previews    : 1024 .. 1024 + MAX_LAYERS*SEQ_TRACKS*2 - 1       (..1055)
 * so the highest sequencer tag is 1055. We base the arp at 1056 and it needs
 * ARP_MAX_SLOTS*ARP_OCT_MAX*2 = 64 tags (1056..1119).
 *
 * IMPORTANT: AMY's sequencer_add_event guards with `tag > max_sequences`
 * (NOT >=), so `sequences[]` must have at least (highest_tag + 2) entries to
 * stay clear of that off-by-one. main.c sets amy_cfg.max_sequencer_tags
 * accordingly — keep these in sync. */
#define SEQ_ARP_TAG_BASE      1056u
#define SEQ_ARP_TAG_COUNT     (ARP_MAX_SLOTS * ARP_OCT_MAX * 2)  /* = 64 */
#define SEQ_ARP_TAG_MAX       (SEQ_ARP_TAG_BASE + SEQ_ARP_TAG_COUNT - 1)  /* 1119 */

/* ── Ratchet tag space ────────────────────────────────────────────────────
 * A "decorated" step (probability<100, ratchet>1, or a conditional trig) is
 * never scheduled on the plain per-step ON/OFF tag pair — instead
 * sequencer_core_service_tick() one-shot schedules up to SEQ_MAX_RATCHET
 * note-on/off pairs per firing, each on its own dedicated, statically
 * assigned tag so ratchet sub-hits never overwrite each other's schedule.
 * Sits just above the arp's tag space; needs
 * MAX_LAYERS*SEQ_TRACKS*SEQ_MAX_RATCHET*2 tags (4*4*4*2 = 128, 1120..1247).
 * Same off-by-one rule as the arp block above applies — main.c's
 * amy_cfg.max_sequencer_tags must stay >= SEQ_RATCHET_TAG_MAX + 2. */
#define SEQ_RATCHET_TAG_BASE  (SEQ_ARP_TAG_MAX + 1u)                          /* 1120 */
#define SEQ_RATCHET_TAG_COUNT (MAX_LAYERS * SEQ_TRACKS * SEQ_MAX_RATCHET * 2) /* 128 */
#define SEQ_RATCHET_TAG_MAX   (SEQ_RATCHET_TAG_BASE + SEQ_RATCHET_TAG_COUNT - 1) /* 1247 */

/* ── Global chord progression ────────────────────────────────────────────── */
#define CHORD_PROG_MAX_ENTRIES 8
