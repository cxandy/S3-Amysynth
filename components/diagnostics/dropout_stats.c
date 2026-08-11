#include "dropout_stats.h"

/* Single-writer-per-counter (see header contract), read cross-core by the UI
 * task and the idle diagnostics loop: volatile keeps every increment a real
 * store, and aligned 32-bit loads are atomic on Xtensa, so readers may lag by
 * one update but never tear. Always compiled - four rare-branch increments
 * cost nothing, and the DEV-menu dropout bar must work in every build. */
static volatile uint32_t s_wire_zlp;
static volatile uint32_t s_ring_underrun;
static volatile uint32_t s_ring_overrun;
static volatile uint32_t s_render_overrun;

void dropout_count_wire_zlp(void)
{
    s_wire_zlp++;
}

void dropout_count_ring_underrun(void)
{
    s_ring_underrun++;
}

void dropout_count_ring_overrun(void)
{
    s_ring_overrun++;
}

void dropout_count_render_overrun(uint32_t missed_ticks)
{
    s_render_overrun += missed_ticks;
}

void dropout_stats_get(dropout_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->wire_zlp       = s_wire_zlp;
    out->ring_underrun  = s_ring_underrun;
    out->ring_overrun   = s_ring_overrun;
    out->render_overrun = s_render_overrun;
}
