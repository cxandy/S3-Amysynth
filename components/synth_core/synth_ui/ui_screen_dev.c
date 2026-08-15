#include "sdkconfig.h"
#if CONFIG_SYNTH_DEV_MENU

#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "voice_config.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#include "synth_slots.h"
#include "seq_clamp.h"
#include "amy.h"
#include "esp_heap_caps.h"
#include "core_load.h"
#include "dropout_stats.h"
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  DEV menu — quarantine zone for temporary controls, diagnostics, readouts
 * ════════════════════════════════════════════════════════════════════════
 * Deliberately free of the UX constraints real screens carry: the ONLY design
 * goals are cheap authoring and trivial navigation (turn = move, click = act,
 * "< back" row = up). Controls graduate to real screens if they prove out.
 *
 *  Adding a control = one table row + (usually) a fmt/adjust pair:
 *    readout   { .label="X", .fmt=... }                    click: nothing
 *    toggle    { .label="X", .fmt=..., .fire=... }         click: fire
 *    value     { .label="X", .fmt=..., .adjust=... }       click: edit mode,
 *                                                          turn adjusts
 *    action    { .label="X", .fire=... }                   click: fire
 *    submenu   { .label="X", .sub=&page }                  click: enter
 *              (+ .fmt previews state on the row: "VALUE >")
 *  `arg` is passed to every callback so one handler serves N rows.
 *  Adding a submenu = one dev_item_t[] + one dev_page_t + one submenu row.
 *
 * DEV state is VOLATILE by design - no snapshot fields, no persistence.
 * (Controls whose backing state persists, like the PCM mode, own that in
 * their real module.) Everything here runs on the UI task. */

typedef struct dev_page dev_page_t;

typedef struct {
    const char *label;
    void (*fmt)(char *buf, size_t n, int arg);   /* value text; NULL = none  */
    void (*adjust)(int delta, int arg);          /* encoder while editing    */
    void (*fire)(int arg);                       /* one-shot on click        */
    const dev_page_t *sub;                       /* submenu target           */
    int arg;
} dev_item_t;

struct dev_page {
    const char       *title;
    const dev_item_t *items;
    uint8_t           count;
};

/* ── Controls ──────────────────────────────────────────────────────────── */

/* PCM playback mode per drum track, targeting L1 (test surface; the core
 * setter no-ops gracefully when L1 is not a drum layer). */
static const char *PCM_MODE_NAMES[] = { "DFLT", "PLAY", "LOOP", "LOOPST", "FRVR" };
#define PCM_MODE_COUNT 5

static void pcm_mode_fmt(char *buf, size_t n, int arg)
{
    uint8_t m = sequencer_core_get_drum_pcm_mode(0, (uint8_t)arg);
    if (m >= PCM_MODE_COUNT) m = 0;
    snprintf(buf, n, "%s", PCM_MODE_NAMES[m]);
}

static void pcm_mode_adjust(int delta, int arg)
{
    int m = (int)sequencer_core_get_drum_pcm_mode(0, (uint8_t)arg) + delta;
    m %= PCM_MODE_COUNT;
    if (m < 0) m += PCM_MODE_COUNT;
    sequencer_core_set_drum_pcm_mode(0, (uint8_t)arg, (uint8_t)m);
}

/* ── WAVE-voice distortion (backing state lives in voice_config) ─────────
 * One TYPE/DRIVE/BITS/RATE/MIX set per domain (arp / drone / melodic), each
 * on its own subpage, so the three subsystems can be shaped independently.
 * Rebuilds re-apply the owning set automatically (voice_build_wave); the push
 * below covers targets that are already built and audible, so edits are heard
 * live. PATCH-mode targets are skipped.
 *
 * One fmt/adjust pair serves all 15 rows: `arg` packs domain and param, since
 * dev_item_t carries a single int (DIST_ARG / DIST_ARG_DOMAIN / DIST_ARG_P). */
static const char *DIST_TYPE_NAMES[] = { "OFF", "CLIP", "FOLD", "CRUSH" };
#define DIST_TYPE_COUNT 4

enum { DIST_P_TYPE, DIST_P_DRIVE, DIST_P_BITS, DIST_P_RATE, DIST_P_MIX };

#define DIST_ARG(dom, param)  (((int)(dom) << 4) | (int)(param))
#define DIST_ARG_DOMAIN(arg)  ((voice_dist_domain_t)((arg) >> 4))
#define DIST_ARG_P(arg)       ((arg) & 0xF)

/* Live push for one domain only - a domain in PATCH mode has no WAVE voices
 * to receive the params (the next wave rebuild inherits them anyway). */
static void dist_push_domain(voice_dist_domain_t domain)
{
    switch (domain) {
    case VOICE_DIST_ARP:
        if (arp_get_source() == ARP_SRC_WAVE)
            voice_apply_dist(sequencer_core_arp_synth());
        break;
    case VOICE_DIST_DRONE:
        if (drone_get_source() == DRONE_SRC_WAVE) {
            voice_apply_dist(DRONE_SYNTH_MAIN);
            voice_apply_dist(DRONE_SYNTH_SUB);
        }
        if (drone_std_get_source() == DRONE_SRC_WAVE) {
            voice_apply_dist(DRONE_STD_SYNTH_MAIN);
            voice_apply_dist(DRONE_STD_SYNTH_SUB);
        }
        break;
    case VOICE_DIST_MELODIC:
        sequencer_core_apply_wave_dist();
        break;
    default:
        break;
    }
}

static void dist_fmt(char *buf, size_t n, int arg)
{
    const voice_dist_t *d = voice_dist_get(DIST_ARG_DOMAIN(arg));
    switch (DIST_ARG_P(arg)) {
    case DIST_P_TYPE:
        snprintf(buf, n, "%s",
                 DIST_TYPE_NAMES[d->type < DIST_TYPE_COUNT ? d->type : 0]);
        break;
    case DIST_P_DRIVE: snprintf(buf, n, "%u",   (unsigned)d->drive); break;
    case DIST_P_BITS:  snprintf(buf, n, "%u",   (unsigned)d->bits);  break;
    case DIST_P_RATE:  snprintf(buf, n, "%ux",  (unsigned)d->rate);  break;
    case DIST_P_MIX:   snprintf(buf, n, "%u%%", (unsigned)d->mix);   break;
    }
}

static void dist_adjust(int delta, int arg)
{
    voice_dist_domain_t domain = DIST_ARG_DOMAIN(arg);
    voice_dist_t d = *voice_dist_get(domain);
    switch (DIST_ARG_P(arg)) {
    case DIST_P_TYPE: {
        int t = (int)d.type + delta;
        t %= DIST_TYPE_COUNT;
        if (t < 0) t += DIST_TYPE_COUNT;
        d.type = (uint8_t)t;
        break;
    }
    case DIST_P_DRIVE: d.drive = SEQ_CLAMP_U8((int)d.drive + delta, 1, 16); break;
    case DIST_P_BITS:  d.bits  = SEQ_CLAMP_U8((int)d.bits + delta, 1, 16);  break;
    case DIST_P_RATE:  d.rate  = SEQ_CLAMP_U8((int)d.rate + delta, 1, 64);  break;
    case DIST_P_MIX:   d.mix   = SEQ_CLAMP_U8((int)d.mix + 5 * delta, 0, 100); break;
    }
    voice_dist_set(domain, &d);
    dist_push_domain(domain);
}

/* One-shot sequencer state dump to the console (seq_core_dump.c). */
static void seqdump_fire(int arg)
{
    (void)arg;
    sequencer_core_dump_state();
}

/* Readouts. */
static void oom_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    snprintf(buf, n, "%lu", (unsigned long)amy_get_oom_count());
}

static void heap_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    snprintf(buf, n, "%uK", (unsigned)(heap_caps_get_free_size(
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024u));
}

/* Per-core busy % as "c0/c1", read from the status-LED sampler's last result
 * (core_load_last(); "--" until it has published one, e.g. LED task absent). */
static void cpu_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    uint8_t busy[CORE_LOAD_NUM_CORES];
    if (core_load_last(busy))
        snprintf(buf, n, "%u/%u%%", (unsigned)busy[0], (unsigned)busy[1]);
    else
        snprintf(buf, n, "--");
}

/* Heap+CPU status bar: while enabled, replaces the bottom hint strip on EVERY
 * screen (including ones that normally hide the hint) so internal-heap and
 * CPU headroom can be watched live under real load. Sampled from the UI
 * task's pre-gate service pass at HEAPBAR_PERIOD frames (20 Hz loop ->
 * 500 ms); the sampled text feeds the render gate so the bar refreshes on
 * change. Dev-facing abbreviations (deliberately terse, not end-user text):
 *   TF  = total free internal heap (MALLOC_CAP_INTERNAL|8BIT), K
 *   LB  = largest free block of the same heap, K
 *   CPU = core0/core1 busy %, from the status-LED sampler via
 *         core_load_last() (1 s cadence; omitted until first publish) */
#define HEAPBAR_PERIOD 10

static bool     s_heapbar_on = false;
static char     s_heapbar_text[48];
static uint32_t s_heapbar_sig;
static uint8_t  s_heapbar_tick;

static bool     s_dropbar_on = false;
static char     s_dropbar_text[48];
static uint32_t s_dropbar_sig;
static uint8_t  s_dropbar_tick;

static void heapbar_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    snprintf(buf, n, "%s", s_heapbar_on ? "ON" : "OFF");
}

static void heapbar_fire(int arg)
{
    (void)arg;
    s_heapbar_on   = !s_heapbar_on;
    s_heapbar_tick = 0;   /* sample on the next service pass */
    s_dropbar_on   = false;   /* the two bars share the hint strip */
}

void synth_ui_dev_heapbar_poll(void)
{
    if (!s_heapbar_on) return;
    if (s_heapbar_tick == 0) {
        size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t b = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                    MALLOC_CAP_8BIT);
        uint8_t busy[CORE_LOAD_NUM_CORES];
        char cpu[12];
        if (core_load_last(busy))
            snprintf(cpu, sizeof cpu, "%u/%u",
                     (unsigned)busy[0], (unsigned)busy[1]);
        else
            snprintf(cpu, sizeof cpu, "--");
        snprintf(s_heapbar_text, sizeof s_heapbar_text,
                 "TF:%u.%uK LB:%u.%uK CPU:%s",
                 (unsigned)(f / 1024u), (unsigned)((f % 1024u) * 10u / 1024u),
                 (unsigned)(b / 1024u), (unsigned)((b % 1024u) * 10u / 1024u),
                 cpu);
        /* Hash the text, not the raw sizes: sub-0.1K jitter then never forces
         * a physically identical redraw through the gate. */
        s_heapbar_sig = fnv1a_bytes(FNV1A_OFFSET, s_heapbar_text,
                                    strlen(s_heapbar_text));
    }
    s_heapbar_tick = (uint8_t)((s_heapbar_tick + 1) % HEAPBAR_PERIOD);
}

bool synth_ui_dev_heapbar_active(void)
{
    return s_heapbar_on;
}

const char *synth_ui_dev_heapbar_text(void)
{
    return s_heapbar_text;
}

uint32_t synth_ui_dev_heapbar_sig(void)
{
    return s_heapbar_on ? s_heapbar_sig : 0;
}

/* Dropout status bar: same mechanism as the heap bar (claims the bottom hint
 * strip on every screen, sampled at the same throttle, text feeds the render
 * gate), showing the cumulative dropout counters so an audible gap can be
 * attributed live to its pipeline layer (see dropout_stats.h):
 *   Z = wire ZLPs (TinyUSB EP-IN FIFO dry - clock-beat fingerprint)
 *   U = ring underruns (render behind realtime, chunk zero-padded)
 *   O = render-clock overruns (blocks of realtime lost)
 *   D = ring-full drops (host stalled draining) */

static void dropbar_fmt(char *buf, size_t n, int arg)
{
    (void)arg;
    snprintf(buf, n, "%s", s_dropbar_on ? "ON" : "OFF");
}

static void dropbar_fire(int arg)
{
    (void)arg;
    s_dropbar_on   = !s_dropbar_on;
    s_dropbar_tick = 0;   /* sample on the next service pass */
    s_heapbar_on   = false;   /* the two bars share the hint strip */
}

void synth_ui_dev_dropbar_poll(void)
{
    if (!s_dropbar_on) return;
    if (s_dropbar_tick == 0) {
        dropout_stats_t d;
        dropout_stats_get(&d);
        snprintf(s_dropbar_text, sizeof s_dropbar_text,
                 "Z:%lu U:%lu O:%lu D:%lu",
                 (unsigned long)d.wire_zlp,
                 (unsigned long)d.ring_underrun,
                 (unsigned long)d.render_overrun,
                 (unsigned long)d.ring_overrun);
        s_dropbar_sig = fnv1a_bytes(FNV1A_OFFSET, s_dropbar_text,
                                    strlen(s_dropbar_text));
    }
    s_dropbar_tick = (uint8_t)((s_dropbar_tick + 1) % HEAPBAR_PERIOD);
}

bool synth_ui_dev_dropbar_active(void)
{
    return s_dropbar_on;
}

const char *synth_ui_dev_dropbar_text(void)
{
    return s_dropbar_text;
}

uint32_t synth_ui_dev_dropbar_sig(void)
{
    return s_dropbar_on ? s_dropbar_sig : 0;
}

/* ── Pages (leaf pages first - root last, so rows can reference them) ──── */

#define PCM_TRACK_ROW(n) \
    { .label = "T" #n, .fmt = pcm_mode_fmt, .adjust = pcm_mode_adjust, .arg = (n) - 1 }

static const dev_item_t s_pcm_items[] = {
    PCM_TRACK_ROW(1),
    PCM_TRACK_ROW(2),
    PCM_TRACK_ROW(3),
    PCM_TRACK_ROW(4),
};
static const dev_page_t s_page_pcm = { "PCM MODE L1", s_pcm_items,
                                       sizeof s_pcm_items / sizeof *s_pcm_items };

/* One five-row page per distortion domain; the rows differ only in the domain
 * packed into `arg`, so the page bodies are macro-generated. */
#define DIST_ROW(name, dom, param)                     \
    { .label = name, .fmt = dist_fmt,                  \
      .adjust = dist_adjust, .arg = DIST_ARG(dom, param) }

#define DIST_PAGE_ITEMS(dom)          \
    DIST_ROW("Type",  dom, DIST_P_TYPE),  \
    DIST_ROW("Drive", dom, DIST_P_DRIVE), \
    DIST_ROW("Bits",  dom, DIST_P_BITS),  \
    DIST_ROW("Rate",  dom, DIST_P_RATE),  \
    DIST_ROW("Mix",   dom, DIST_P_MIX)

static const dev_item_t s_dist_arp_items[]  = { DIST_PAGE_ITEMS(VOICE_DIST_ARP) };
static const dev_item_t s_dist_drn_items[]  = { DIST_PAGE_ITEMS(VOICE_DIST_DRONE) };
static const dev_item_t s_dist_mel_items[]  = { DIST_PAGE_ITEMS(VOICE_DIST_MELODIC) };

static const dev_page_t s_page_dist_arp = { "DIST ARP", s_dist_arp_items,
                                            sizeof s_dist_arp_items / sizeof *s_dist_arp_items };
static const dev_page_t s_page_dist_drn = { "DIST DRONE", s_dist_drn_items,
                                            sizeof s_dist_drn_items / sizeof *s_dist_drn_items };
static const dev_page_t s_page_dist_mel = { "DIST MELODIC", s_dist_mel_items,
                                            sizeof s_dist_mel_items / sizeof *s_dist_mel_items };

/* Index page: each row carries its domain's current TYPE, so the state of all
 * three is readable without entering them. */
static const dev_item_t s_dist_items[] = {
    { .label = "Arp",     .sub = &s_page_dist_arp, .fmt = dist_fmt,
      .arg = DIST_ARG(VOICE_DIST_ARP, DIST_P_TYPE) },
    { .label = "Drone",   .sub = &s_page_dist_drn, .fmt = dist_fmt,
      .arg = DIST_ARG(VOICE_DIST_DRONE, DIST_P_TYPE) },
    { .label = "Melodic", .sub = &s_page_dist_mel, .fmt = dist_fmt,
      .arg = DIST_ARG(VOICE_DIST_MELODIC, DIST_P_TYPE) },
};
static const dev_page_t s_page_dist = { "DIST (WAVE)", s_dist_items,
                                        sizeof s_dist_items / sizeof *s_dist_items };

static const dev_item_t s_root_items[] = {
    { .label = "Distortion",  .sub = &s_page_dist },
    { .label = "PCM Mode L1", .sub = &s_page_pcm },
    { .label = "AMY OOM",     .fmt = oom_fmt },
    { .label = "Heap int",    .fmt = heap_fmt },
    { .label = "CPU c0/c1",   .fmt = cpu_fmt },
    { .label = "Status bar",  .fmt = heapbar_fmt, .fire = heapbar_fire },
    { .label = "Drop bar",    .fmt = dropbar_fmt, .fire = dropbar_fire },
    { .label = "Dump seq",    .fire = seqdump_fire },
};
static const dev_page_t s_page_root = { "", s_root_items,
                                        sizeof s_root_items / sizeof *s_root_items };

/* ── Navigation (row 0 of every page is the synthetic "< back" row) ────── */

#define DEV_STACK_MAX 4

typedef struct { const dev_page_t *pg; uint8_t cur; } dev_lvl_t;

static dev_lvl_t s_stack[DEV_STACK_MAX] = { { &s_page_root, 0 } };
static uint8_t   s_depth   = 0;
static bool      s_editing = false;

static uint8_t dev_row_count(const dev_page_t *pg)
{
    return (uint8_t)(pg->count + 1);   /* +1 for "< back" */
}

bool synth_ui_dev_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_DEV && !seq_state.menu_open;
}

bool synth_ui_dev_handle_encoder(int delta)
{
    if (!synth_ui_dev_is_active()) return false;
    dev_lvl_t *lv = &s_stack[s_depth];
    if (s_editing && lv->cur > 0) {
        const dev_item_t *it = &lv->pg->items[lv->cur - 1];
        if (it->adjust) it->adjust(delta, it->arg);
    } else {
        int n = (int)dev_row_count(lv->pg);
        int c = (int)lv->cur + delta;
        c %= n;
        if (c < 0) c += n;
        lv->cur = (uint8_t)c;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_dev_handle_button(void)
{
    if (!synth_ui_dev_is_active()) return false;
    dev_lvl_t *lv = &s_stack[s_depth];
    if (s_editing) {
        s_editing = false;
    } else if (lv->cur == 0) {
        /* "< back": up one page, or from the root back to the main menu. */
        if (s_depth > 0) s_depth--;
        else             seq_state.menu_open = true;
    } else {
        const dev_item_t *it = &lv->pg->items[lv->cur - 1];
        if (it->sub && s_depth + 1 < DEV_STACK_MAX) {
            s_depth++;
            s_stack[s_depth] = (dev_lvl_t){ it->sub, 0 };
        } else if (it->fire) {
            it->fire(it->arg);
        } else if (it->adjust) {
            s_editing = true;
        }
        /* plain readout: click is a no-op */
    }
    s_force_redraw = true;
    return true;
}

/* ── View build + render-gate signature ────────────────────────────────── */

uint32_t dev_view_signature(dev_view_t *out)
{
    const dev_lvl_t  *lv = &s_stack[s_depth];
    const dev_page_t *pg = lv->pg;

    /* Zero first: rows are hashed full-width below, so bytes past each NUL
     * must be deterministic or the render gate never settles. */
    memset(out, 0, sizeof *out);
    out->title   = pg->title;
    out->cursor  = lv->cur;
    out->editing = s_editing;

    uint8_t n = dev_row_count(pg);
    if (n > DEV_VIEW_MAX_ROWS) n = DEV_VIEW_MAX_ROWS;
    out->count = n;

    snprintf(out->rows[0].label, DEV_LABEL_LEN, "%s",
             s_depth > 0 ? "< back" : "< menu");
    out->rows[0].value[0] = '\0';

    for (uint8_t i = 1; i < n; i++) {
        const dev_item_t *it = &pg->items[i - 1];
        snprintf(out->rows[i].label, DEV_LABEL_LEN, "%s", it->label);
        if (it->sub && it->fmt) {
            /* Submenu that also previews state: "<value> >". */
            char v[DEV_VALUE_LEN];
            it->fmt(v, sizeof v, it->arg);
            snprintf(out->rows[i].value, DEV_VALUE_LEN, "%.*s >",
                     DEV_VALUE_LEN - 3, v);
        }
        else if (it->sub) snprintf(out->rows[i].value, DEV_VALUE_LEN, ">");
        else if (it->fmt) it->fmt(out->rows[i].value, DEV_VALUE_LEN, it->arg);
        else              out->rows[i].value[0] = '\0';
    }

    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &s_depth,      sizeof s_depth);
    h = fnv1a_bytes(h, &out->cursor,  sizeof out->cursor);
    h = fnv1a_bytes(h, &out->editing, sizeof out->editing);
    h = fnv1a_bytes(h, &out->count,   sizeof out->count);
    for (uint8_t i = 0; i < n; i++) {
        h = fnv1a_bytes(h, out->rows[i].label, sizeof out->rows[i].label);
        h = fnv1a_bytes(h, out->rows[i].value, sizeof out->rows[i].value);
    }
    return h;
}

#endif /* CONFIG_SYNTH_DEV_MENU */
