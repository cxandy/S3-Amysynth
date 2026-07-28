#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Runtime PCM sampler ("record into a one-shot pad") ──────────────────
 * Captures AMY's final mixed output into a single runtime PCM preset slot,
 * then wires it into one drum-layer track so it plays back like the built-in
 * 808 samples. Fixed-length mono buffer, one slot - not a multi-slot looper.
 *
 * Flow, driven from the menu overlay's "Sample" item:
 *   IDLE --arm--> ARMED --start--> RECORDING --(auto)--> READY --assign--> IDLE
 * CANCEL aborts from any non-IDLE state and frees the slot.
 *
 * ARM pre-allocates the PCM buffer so START never allocates; START just flips
 * a flag the render task observes, so capture begins on a block edge. */
typedef enum {
    SAMPLE_REC_IDLE = 0,
    SAMPLE_REC_ARMED,
    SAMPLE_REC_RECORDING,
    SAMPLE_REC_READY,
} sample_rec_state_t;

void sample_rec_init(void);

/* ── UI-task (Core 0) control surface ── */

/* IDLE -> ARMED. Allocates the PCM preset for (layer_idx, track); false on
 * OOM or if not IDLE. The CALLER must ensure layer_idx is a SEQ_LAYER_DRUM
 * layer - sample_rec has no UI state to check it. */
bool sample_rec_arm(uint8_t layer_idx, uint8_t track);

/* ARMED -> RECORDING. false if not currently ARMED. */
bool sample_rec_start(void);

/* READY -> IDLE. Wires the finished preset into the armed track (forcing
 * the drum layer into PCM engine mode if needed). false if not READY. */
bool sample_rec_assign(void);

/* Any non-IDLE state -> IDLE. Frees the preset. No-op if already IDLE. */
void sample_rec_cancel(void);

sample_rec_state_t sample_rec_get_state(void);
uint8_t sample_rec_get_progress_pct(void);   /* 0..100, meaningful only while RECORDING */

/* ── Render-task (Core 1) hook ──
 * Call once per rendered block, immediately after amy_update() returns, i.e.
 * with amy_queue_lock already released. Alloc-free and non-blocking except on
 * the rare block that tears down a cancel, which briefly re-acquires
 * amy_queue_lock as add_delta_to_queue() does from any other task. */
void sample_rec_render_tick(const int16_t *interleaved_stereo, uint16_t frames);

#ifdef __cplusplus
}
#endif
