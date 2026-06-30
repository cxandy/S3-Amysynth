#pragma once
#include <stddef.h>
#include <stdint.h>

/* ── Patch-cycle descriptor ────────────────────────────────────────────────
 * Describes a named domain of patches that can be stepped through with a
 * single encoder click.  When `list` is NULL the domain operates in
 * full-range mode (0 .. full_max inclusive); otherwise it steps through the
 * curated list in order, wrapping at the ends.
 *
 * Off-list / out-of-range values snap to index 0 *before* the step is
 * applied, so the result is always the neighbour of index 0 — never index 0
 * itself — when the current patch is not part of the domain. */
typedef struct {
    const uint16_t *list;     /* curated patch array (NULL → full-range)    */
    uint16_t        count;    /* number of entries in list (0 if full-range) */
    uint16_t        full_max; /* highest valid index in full-range mode      */
} patch_domain_t;

/* Step `current` by `dir` (any sign; clamped to ±1 internally) through the
 * domain, wrapping at the ends.  Returns the new patch number. */
static inline uint16_t patch_domain_step(const patch_domain_t *d,
                                         uint16_t current, int dir)
{
    dir = (dir > 0) ? 1 : -1;
    if (d->list == NULL) {
        /* Full-range mode: step within [0 .. full_max]. */
        int n   = (int)d->full_max + 1;
        int cur = (int)current;
        if (cur > (int)d->full_max) cur = 0;   /* out-of-range → snap to start */
        return (uint16_t)((cur + dir + n) % n);
    } else {
        /* Curated mode: locate current in list (default idx 0 if absent). */
        int n   = (int)d->count;
        int idx = 0;
        for (int i = 0; i < n; i++) {
            if (d->list[i] == current) { idx = i; break; }
        }
        return d->list[(idx + dir + n) % n];
    }
}
