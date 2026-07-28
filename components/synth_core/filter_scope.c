#include "filter_scope.h"

#if CONFIG_FILTER_SCOPE

#include <stdatomic.h>

#include "amy.h"

/* The published cell. Ownership is split so that every word has exactly one
 * writer (see filter_scope.h): the UI owns read_epoch, the render tap owns
 * everything else except the armed flag and the list, which the UI rewrites
 * only while armed is false. */
static struct {
    atomic_bool armed;                  /* UI writes; tap reads once per block */
    uint16_t    oscs[FILTER_SCOPE_MAX_OSCS];  /* UI writes, only while disarmed */
    uint8_t     n_oscs;                 /* ditto */

    atomic_uint_least32_t read_epoch;   /* UI writes; tap compares */
    uint32_t    last_epoch;             /* tap-private */

    float       lo, hi;                 /* tap writes; UI reads */
    atomic_bool valid;                  /* tap writes; publishes lo/hi */
} s_cell;

void filter_scope_disarm(void)
{
    atomic_store_explicit(&s_cell.armed, false, memory_order_release);
}

void filter_scope_arm(const uint16_t *oscs, uint8_t n)
{
    /* 1. Clear the flag FIRST. It is the single word that transfers ownership
     *    of the list, so from here the tap will not start a new iteration. */
    atomic_store_explicit(&s_cell.armed, false, memory_order_release);

    if (oscs == NULL) {
        n = 0;
    }
    if (n > FILTER_SCOPE_MAX_OSCS) {
        n = FILTER_SCOPE_MAX_OSCS;
    }

    /* 2. Rewrite the list and its length. A tap that sampled the flag at block
     *    start may still be iterating, which is why the list is a fixed-size
     *    array that is never freed and the tap bounds-checks every index.
     *    Worst case: one frame reporting a stale but in-range oscillator. */
    for (uint8_t i = 0; i < n; i++) {
        s_cell.oscs[i] = oscs[i];
    }
    s_cell.n_oscs = n;

    /* Drop any band accumulated for the previous target, and force the tap to
     * restart its accumulator rather than fold new voices into old extremes. */
    atomic_store_explicit(&s_cell.valid, false, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_cell.read_epoch, 1, memory_order_relaxed);

    /* 3. Set the flag LAST. */
    if (n > 0) {
        atomic_store_explicit(&s_cell.armed, true, memory_order_release);
    }
}

void filter_scope_render_tick(void)
{
    /* Disarmed: one load and one predicted-not-taken branch per block. */
    if (!atomic_load_explicit(&s_cell.armed, memory_order_acquire)) {
        return;
    }
    if (synth == NULL || msynth == NULL) {
        return;
    }

    /* A bumped epoch means the UI consumed the previous window, so start a
     * fresh one instead of folding into already-drawn extremes: that keeps the
     * band "since you last looked" rather than monotonically widening. */
    uint32_t epoch = atomic_load_explicit(&s_cell.read_epoch, memory_order_relaxed);
    bool  have;
    float lo, hi;
    if (epoch != s_cell.last_epoch) {
        s_cell.last_epoch = epoch;
        have = false;
        lo = hi = 0.0f;
    } else {
        have = atomic_load_explicit(&s_cell.valid, memory_order_relaxed);
        lo = s_cell.lo;
        hi = s_cell.hi;
    }

    uint8_t n = s_cell.n_oscs;
    if (n > FILTER_SCOPE_MAX_OSCS) {
        n = FILTER_SCOPE_MAX_OSCS;
    }
    const uint16_t max_osc = AMY_OSCS;

    for (uint8_t i = 0; i < n; i++) {
        const uint16_t osc = s_cell.oscs[i];
        if (osc >= max_osc) {
            continue;   /* stale index from a concurrent re-arm */
        }
        /* synth[]/msynth[] entries are lazily allocated and individually
         * freeable, so a cached index can point at a NULL slot after a patch
         * change or an OOM retreat. This runs in the render task: a missed
         * check here is a LoadProhibited panic, not a glitched pixel. */
        const struct synthinfo *s = synth[osc];
        const struct mod_synthinfo *m = msynth[osc];
        if (s == NULL || m == NULL) {
            continue;
        }
        /* Audible voices only: excludes the voice-local native-LFO carrier
         * (SYNTH_IS_MOD_SOURCE, no filter of its own) and stops decayed voices
         * from holding the band open at their last cutoff. */
        if (s->status != SYNTH_AUDIBLE || s->filter_type == FILTER_NONE) {
            continue;
        }

        const float v = m->filter_logfreq;
        if (!have) {
            lo = hi = v;
            have = true;
        } else if (v < lo) {
            lo = v;
        } else if (v > hi) {
            hi = v;
        }
    }

    s_cell.lo = lo;
    s_cell.hi = hi;
    /* Release so the lo/hi stores are visible before valid is observed. */
    atomic_store_explicit(&s_cell.valid, have, memory_order_release);
}

bool filter_scope_read(float *lo_logfreq, float *hi_logfreq)
{
    /* A disarmed scope must never serve data. disarm() cannot clear `valid`
     * (the tap owns that word and an in-flight block could re-set it), so
     * without this gate a popup on a never-armed target would read the LAST
     * armed target's final band forever. arm/disarm/read all run on the
     * synth_ui task, so this load is coherent with the caller's arm state. */
    if (!atomic_load_explicit(&s_cell.armed, memory_order_relaxed)) {
        return false;
    }

    const bool valid = atomic_load_explicit(&s_cell.valid, memory_order_acquire);
    const float lo = s_cell.lo;
    const float hi = s_cell.hi;

    /* The only word the UI ever writes. Bumping it unconditionally is what
     * makes consecutive reads report disjoint windows. */
    atomic_fetch_add_explicit(&s_cell.read_epoch, 1, memory_order_relaxed);

    if (!valid) {
        return false;
    }
    if (lo_logfreq != NULL) {
        *lo_logfreq = lo;
    }
    if (hi_logfreq != NULL) {
        *hi_logfreq = hi;
    }
    return true;
}

#endif /* CONFIG_FILTER_SCOPE */
