#pragma once

#include <stdint.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Audio dropout counters - one per distinct silence/gap mechanism in the
 * render -> ring -> USB pipeline, so a click heard on the host can be
 * attributed to its layer instead of guessed at:
 *
 *   wire_zlp        TinyUSB's EP-IN FIFO was dry when the next isochronous
 *                   frame was loaded -> a zero-length packet went on the wire
 *                   (host/device clock-beat fingerprint; the ring below it
 *                   may be perfectly healthy).
 *   ring_underrun   The UAC pull found fewer samples in the SPSC ring than
 *                   one 10 ms chunk; the shortfall was zero-padded (render
 *                   not keeping up with realtime).
 *   ring_overrun    A rendered block was dropped whole because the ring was
 *                   full while a host was consuming (host stalled draining).
 *   render_overrun  Render-clock ticks that fired while the previous block
 *                   was still rendering; strict 1:1 pacing never renders the
 *                   backlog, so each one is a block of realtime lost.
 *
 * Reading the pattern: ring_underrun/render_overrun climbing with wire_zlp
 * flat = local overload (zero-padded silence, full-length USB packets).
 * wire_zlp climbing alone = genuine UAC/clock-beat issue.
 *
 * Contract:
 *  - Each counter has exactly ONE writer task (wire_zlp: TinyUSB task,
 *    ring_underrun: usb_mic_task, ring_overrun + render_overrun: render
 *    task). A new call site for an increment must keep that single-writer
 *    rule or the lock-free counters tear.
 *  - Increments are a bare counter add: safe on the render path, but still
 *    task-context only - not ISR-safe by contract.
 *  - dropout_stats_get() is advisory: callable from any task, may lose a
 *    concurrent increment, never sees torn values (aligned 32-bit reads).
 *  - Counters are cumulative since boot and never reset at runtime.
 */
typedef struct {
    uint32_t wire_zlp;
    uint32_t ring_underrun;
    uint32_t ring_overrun;
    uint32_t render_overrun;
} dropout_stats_t;

void dropout_count_wire_zlp(void);
void dropout_count_ring_underrun(void);
void dropout_count_ring_overrun(void);
void dropout_count_render_overrun(uint32_t missed_ticks);

void dropout_stats_get(dropout_stats_t *out);

#if CONFIG_AMYSYNTH_DROPOUT_TS
/* Optional wire-ZLP event timestamps: every dropout_count_wire_zlp() also
 * records esp_timer microseconds into a ring of the most recent
 * DROPOUT_TS_RING events, so host tooling can resolve burst microstructure
 * (contiguous ~1 ms-spaced events = data-supply stall; sparse events =
 * clock-beat drain) instead of inferring from counter deltas.
 *
 * Contract:
 *  - Stamping inherits wire_zlp's single-writer rule (TinyUSB task) and
 *    task-context-only restriction.
 *  - dropout_ts_snapshot(): callable from any task. Copies the newest
 *    min(total, DROPOUT_TS_RING) timestamps oldest-first into out[] and
 *    returns that count; *seq_out (may be NULL) = total events since boot,
 *    monotonic, so hosts dedup across polls. Retries while the writer
 *    advances mid-copy, so returned slots are never torn; events
 *    overwritten between polls are lost - ring depth bounds the loss and
 *    seq exposes it.
 */
#define DROPOUT_TS_RING 128
uint32_t dropout_ts_snapshot(int64_t out[DROPOUT_TS_RING], uint32_t *seq_out);
#endif

#ifdef __cplusplus
}
#endif
