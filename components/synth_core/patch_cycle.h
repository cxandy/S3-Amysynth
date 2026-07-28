#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Patch-cycle descriptor ────────────────────────────────────────────────
 * A domain of patches steppable with one encoder click. `list` NULL = full
 * range (0 .. full_max); otherwise it steps the curated list, wrapping.
 *
 * All consumers share ONE catalog (ui_patch_cycle.c); a consumer that cannot
 * play some patches EXCLUDES them via the `excluded` predicate rather than
 * keeping its own list. patch_domain_step() skips excluded entries, so new
 * catalog entries reach every consumer unless a predicate opts out.
 *
 * Off-list / out-of-range values snap to index 0 BEFORE the step, so a patch
 * outside the domain always lands on index 0's neighbour, never index 0. */
typedef bool (*patch_excluded_fn)(uint16_t patch);

typedef struct {
    const uint16_t   *list;     /* curated patch array (NULL → full-range)    */
    uint16_t          count;    /* number of entries in list (0 if full-range) */
    uint16_t          full_max; /* highest valid index in full-range mode      */
    patch_excluded_fn excluded; /* NULL → whole domain allowed                 */
} patch_domain_t;

static inline bool patch_domain_allows(const patch_domain_t *d, uint16_t p)
{
    return d->excluded == NULL || !d->excluded(p);
}

/* Step `current` by `dir` (any sign, clamped to +/-1) through the domain,
 * wrapping and skipping excluded entries. Returns `current` unchanged if
 * every entry is excluded. */
static inline uint16_t patch_domain_step(const patch_domain_t *d,
                                         uint16_t current, int dir)
{
    dir = (dir > 0) ? 1 : -1;
    if (d->list == NULL) {
        /* Full-range mode: step within [0 .. full_max]. */
        int n   = (int)d->full_max + 1;
        int cur = (int)current;
        if (cur > (int)d->full_max) cur = 0;   /* out-of-range → snap to start */
        for (int i = 0; i < n; i++) {
            cur = (cur + dir + n) % n;
            if (patch_domain_allows(d, (uint16_t)cur)) return (uint16_t)cur;
        }
        return current;
    } else {
        /* Curated mode: locate current in list (default idx 0 if absent). */
        int n   = (int)d->count;
        int idx = 0;
        for (int i = 0; i < n; i++) {
            if (d->list[i] == current) { idx = i; break; }
        }
        for (int i = 0; i < n; i++) {
            idx = (idx + dir + n) % n;
            if (patch_domain_allows(d, d->list[idx])) return d->list[idx];
        }
        return current;
    }
}
