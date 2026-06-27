#include "arp_core.h"
#include "sequencer_core.h"
#include "quantizer.h"
#include "seq_clamp.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

/* DEBUG: bisect heap corruption inside arp init. Gated by
 * CONFIG_AMYSYNTH_HEAP_CHECK; compiles to nothing when off (default). */
#if CONFIG_AMYSYNTH_HEAP_CHECK
#define ARP_HEAP_CHECK(where) do { \
    if (!heap_caps_check_integrity_all(true)) { \
        ESP_LOGE(TAG, "HEAP CORRUPT detected at: %s", where); \
    } else { \
        ESP_LOGI(TAG, "HEAP OK at: %s", where); \
    } \
} while (0)
#else
#define ARP_HEAP_CHECK(where) do { (void)(where); } while (0)
#endif

#ifndef CONFIG_SEQ_ARP_DEFAULT_ENABLED
#define CONFIG_SEQ_ARP_DEFAULT_ENABLED 0
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_SCALE
#define CONFIG_SEQ_ARP_DEFAULT_SCALE 1   /* Major */
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_ROOT_NOTE
#define CONFIG_SEQ_ARP_DEFAULT_ROOT_NOTE 60  /* C4 */
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_GATE_PCT
#define CONFIG_SEQ_ARP_DEFAULT_GATE_PCT 75
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_OCTAVES
#define CONFIG_SEQ_ARP_DEFAULT_OCTAVES 1
#endif
#ifndef CONFIG_SEQ_ARP_DEFAULT_PATCH
#define CONFIG_SEQ_ARP_DEFAULT_PATCH 138  /* DX7 E.Piano, matches melodic default */
#endif

static const char *TAG = "arp_core";

/* AMY_SEQUENCER_PPQ = 48 → 1/16 = 12 ticks. */
static const uint32_t s_rate_ticks[ARP_RATE_COUNT] = {
    [ARP_RATE_1_1]  = 192,
    [ARP_RATE_1_4]  = 48,
    [ARP_RATE_1_8]  = 24,
    [ARP_RATE_1_16] = 12,
    [ARP_RATE_1_32] = 6,
    [ARP_RATE_1_4T]   = 32,
    [ARP_RATE_1_8T]   = 16,
    [ARP_RATE_1_16T]  = 8,
    [ARP_RATE_1_32T]  = 4,
};

static const char *s_rate_names[ARP_RATE_COUNT] = {
    [ARP_RATE_1_1]  = "1/1",
    [ARP_RATE_1_4]  = "1/4",
    [ARP_RATE_1_8]  = "1/8",
    [ARP_RATE_1_16] = "1/16",
    [ARP_RATE_1_32] = "1/32",
    [ARP_RATE_1_4T]   = "1/4T",
    [ARP_RATE_1_8T]   = "1/8T",
    [ARP_RATE_1_16T]  = "1/16T",
    [ARP_RATE_1_32T]  = "1/32T",
};

typedef struct {
    bool       enabled;
    arp_dir_t  dir;
    uint8_t    octaves;       /* 1..ARP_OCT_MAX */
    arp_rate_t rate;
    uint8_t    gate_pct;      /* 10..100 */
    int16_t    slots[ARP_MAX_SLOTS];  /* raw chromatic MIDI, -1 = empty */
    uint8_t    scale_index;
    uint8_t    root_note;
    uint16_t   patch;
    seq_env_t    env;            /* runtime-editable ADSR (shared graph editor) */
    bool         env_authored;   /* true once the user commits a custom env      */
    seq_filter_t filter;         /* runtime-editable filter (shared filter editor) */
    bool         filter_authored;/* true once the user commits a custom filter    */
} arp_state_t;

static arp_state_t s_arp;

/* Refresh coalescing: setters mark the arp dirty instead of re-emitting the
 * whole sequence inline. arp_core_service() (called once per UI frame) performs
 * a single re-emit when dirty, so a fast encoder spin through octaves/rate/gate
 * collapses many setter calls into at most one full re-emit per frame instead of
 * one per detent. Each re-emit clears + re-schedules up to ARP_MAX_STEPS events,
 * every one through the shared AMY event mutex, so this removes a real burst of
 * mutex traffic from Core 0 during edits. */
static volatile bool s_arp_dirty = false;

static inline void arp_mark_dirty(void) { s_arp_dirty = true; }

/* Max distinct arp steps we can schedule = slots * max octaves. Each step uses
 * two tags (on + on+1); we space step tags by 2. */
#define ARP_MAX_STEPS (ARP_MAX_SLOTS * ARP_OCT_MAX)

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Shared ascending bubble sort for collect helpers. */
static void arp_sort_asc(uint8_t *out, uint8_t n)
{
    for (uint8_t i = 0; i + 1 < n; i++)
        for (uint8_t j = 0; j + 1 < n - i; j++)
            if (out[j] > out[j + 1]) { uint8_t t = out[j]; out[j] = out[j+1]; out[j+1] = t; }
}

/* UP mode: REST is a transparent skip — all non-empty notes are collected.
 * Returns sorted ascending; caller uses sorted[note_idx] directly. */
static uint8_t arp_collect_up(uint8_t out[ARP_MAX_SLOTS])
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++) {
        if (s_arp.slots[i] == -1) break;          /* empty sentinel: stop */
        if (s_arp.slots[i] == ARP_REST) continue; /* REST in UP: skip */
        out[n++] = (uint8_t)s_arp.slots[i];
    }
    arp_sort_asc(out, n);
    return n;
}

/* DOWN mode: identical skip-REST logic to UP; caller reverses with
 * sorted[count-1-note_idx]. */
static uint8_t arp_collect_down(uint8_t out[ARP_MAX_SLOTS])
{
    return arp_collect_up(out);
}

/* Snap a chromatic note to the arp's own scale/root (independent quantizer). */
static uint8_t arp_snap(uint8_t chromatic)
{
    const musical_scale_t *scale = quantizer_get_scale(s_arp.scale_index);
    uint8_t snapped = quantizer_snap_midi_note(chromatic, s_arp.root_note, scale);
    return sequencer_core_clamp_melodic_note((int32_t)snapped);
}

/* Clear every possible arp tag slot. */
static void arp_clear_all(void)
{
    uint32_t base = sequencer_core_arp_tag_base();
    for (uint8_t i = 0; i < ARP_MAX_STEPS; i++) {
        sequencer_core_arp_clear_note(base + (uint32_t)i * 2u);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void arp_core_init(void)
{
    memset(&s_arp, 0, sizeof(s_arp));
    s_arp.enabled     = CONFIG_SEQ_ARP_DEFAULT_ENABLED;
    s_arp.dir         = ARP_UP;
    s_arp.octaves     = CONFIG_SEQ_ARP_DEFAULT_OCTAVES;
    s_arp.rate        = ARP_RATE_1_16;
    s_arp.gate_pct    = CONFIG_SEQ_ARP_DEFAULT_GATE_PCT;
    s_arp.scale_index = CONFIG_SEQ_ARP_DEFAULT_SCALE;
    s_arp.root_note   = CONFIG_SEQ_ARP_DEFAULT_ROOT_NOTE;
    s_arp.patch       = CONFIG_SEQ_ARP_DEFAULT_PATCH;
    /* Default ADSR mirrors the melodic compile-time defaults; not authored until
     * the user commits in the graph editor (patch's own env wins until then). */
s_arp.env.attack_ms   = 4;    // Tiny curve to prevent an aggressive digital click
s_arp.env.decay_ms    = 250;  // Medium-short decay allows the note body to breathe
s_arp.env.sustain_pct = 30;   // Low sustain keeps the sequence energetic but audible if held
s_arp.env.release_ms  = 200;  // Controlled tail that fills space without causing a muddy low-end
    s_arp.env.eg_type     = 0;   /* ENVELOPE_NORMAL */
    s_arp.env_authored      = false;
    /* Default filter: bypass (not authored; patch's filter wins until user commits). */
    s_arp.filter.filter_type = 0;   /* SEQ_FILTER_NONE */
    s_arp.filter.cutoff_hz   = 800.0f;
    s_arp.filter.resonance   = 1.0f;
    s_arp.filter.enabled     = false;
    s_arp.filter_authored    = false;
    if (s_arp.octaves < 1) s_arp.octaves = 1;
    if (s_arp.octaves > ARP_OCT_MAX) s_arp.octaves = ARP_OCT_MAX;
    if (s_arp.scale_index >= quantizer_scale_count()) s_arp.scale_index = 0;
    for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++) s_arp.slots[i] = -1;

    ARP_HEAP_CHECK("arp_init: before arp_configure");
    sequencer_core_arp_configure(s_arp.patch, sequencer_core_arp_voices());
    ARP_HEAP_CHECK("arp_init: after arp_configure");
    /* If the arp boots enabled (with seeded slots), schedule an initial emit on
     * the first service tick rather than emitting inline during init. */
    arp_mark_dirty();
    ESP_LOGI(TAG, "arp_core initialized (synth %u)", sequencer_core_arp_synth());
}

void arp_core_refresh(void)
{
    arp_clear_all();

    if (!s_arp.enabled) {
        return;
    }

    /* ── SLOT mode: play slots in stored order, rests preserved ── */
    if (s_arp.dir == ARP_SLOT) {
        uint8_t active = 0;
        for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++)
            if (s_arp.slots[i] != -1) active++;
        if (active == 0) return;

        uint32_t rate    = s_rate_ticks[s_arp.rate];
        uint8_t  steps   = (uint8_t)(active * s_arp.octaves);
        uint32_t period  = (uint32_t)steps * rate;
        uint32_t gate    = (rate * s_arp.gate_pct) / 100u;
        if (gate < 1) gate = 1;

        uint32_t tag_base = sequencer_core_arp_tag_base();
        uint8_t  step_i   = 0;

        for (uint8_t oct = 0; oct < s_arp.octaves; oct++) {
            for (uint8_t si = 0; si < ARP_MAX_SLOTS; si++) {
                int16_t v = s_arp.slots[si];
                if (v == -1) continue;   /* unused: not part of the cycle */

                uint32_t tick_on = 1u + (uint32_t)step_i * rate;

                if (v != ARP_REST) {
                    int32_t chromatic = (int32_t)v + (int32_t)oct * 12;
                    uint8_t play_note = arp_snap(sequencer_core_clamp_melodic_note(chromatic));
                    sequencer_core_arp_emit_note(tag_base + (uint32_t)step_i * 2u,
                                                 play_note, 0.9f, tick_on, gate, period);
                }
                /* REST: tag stays cleared (arp_clear_all already ran); step_i still advances. */
                step_i++;
            }
        }
        return;
    }

    /* ── UP / DOWN: sorted pitch order ── */
    uint8_t sorted[ARP_MAX_SLOTS];
    uint8_t count = (s_arp.dir == ARP_DOWN)
                    ? arp_collect_down(sorted)
                    : arp_collect_up(sorted);
    if (count == 0) {
        return;  /* nothing to play */
    }

    uint32_t rate    = s_rate_ticks[s_arp.rate];
    uint8_t  steps   = (uint8_t)(count * s_arp.octaves);
    uint32_t period  = (uint32_t)steps * rate;
    uint32_t gate    = (rate * s_arp.gate_pct) / 100u;
    if (gate < 1) gate = 1;

    uint32_t tag_base = sequencer_core_arp_tag_base();

    for (uint8_t i = 0; i < steps; i++) {
        /* index within the sorted set, advancing by octave every `count`. */
        uint8_t note_idx = i % count;
        uint8_t octave   = i / count;
        /* Direction: UP walks sorted ascending; DOWN walks it in reverse. */
        uint8_t pick = (s_arp.dir == ARP_DOWN)
                       ? sorted[count - 1 - note_idx]
                       : sorted[note_idx];
        int32_t chromatic = (int32_t)pick + (int32_t)octave * 12;
        uint8_t play_note = arp_snap(sequencer_core_clamp_melodic_note(chromatic));

        uint32_t tick_on = 1u + (uint32_t)i * rate;
        sequencer_core_arp_emit_note(tag_base + (uint32_t)i * 2u,
                                     play_note, 0.9f, tick_on, gate, period);
    }
}

void arp_set_enabled(bool enabled)
{
    if (s_arp.enabled == enabled) return;
    s_arp.enabled = enabled;
    arp_mark_dirty();
    ESP_LOGI(TAG, "arp %s", enabled ? "enabled" : "disabled");
}

void arp_set_direction(arp_dir_t dir)
{
    if (s_arp.dir == dir) return;
    s_arp.dir = dir;
    arp_mark_dirty();
}

void arp_set_octaves(uint8_t octaves)
{
    octaves = SEQ_CLAMP_U8(octaves, 1, ARP_OCT_MAX);
    if (s_arp.octaves == octaves) return;
    s_arp.octaves = octaves;
    arp_mark_dirty();
}

void arp_set_rate(arp_rate_t rate)
{
    if (rate >= ARP_RATE_COUNT) return;
    if (s_arp.rate == rate) return;
    s_arp.rate = rate;
    arp_mark_dirty();
}

void arp_set_gate_pct(uint8_t gate_pct)
{
    gate_pct = SEQ_CLAMP_U8(gate_pct, 10, 100);
    if (s_arp.gate_pct == gate_pct) return;
    s_arp.gate_pct = gate_pct;
    arp_mark_dirty();
}

void arp_set_scale(uint8_t scale_index)
{
    if (scale_index >= quantizer_scale_count()) scale_index = 0;
    if (s_arp.scale_index == scale_index) return;
    s_arp.scale_index = scale_index;
    arp_mark_dirty();
}

void arp_set_root_note(uint8_t root_note)
{
    root_note = SEQ_CLAMP_U8(root_note, 0, 127);
    if (s_arp.root_note == root_note) return;
    s_arp.root_note = root_note;
    arp_mark_dirty();
}

void arp_set_chord(uint8_t root_midi, uint8_t scale_index)
{
    arp_set_root_note(root_midi);
    arp_set_scale(scale_index);
}

void arp_set_patch(uint16_t patch_number)
{
    patch_number = SEQ_CLAMP_U16(patch_number, 0, 256);
    if (s_arp.patch == patch_number) return;
    s_arp.patch = patch_number;
    sequencer_core_arp_configure(s_arp.patch, sequencer_core_arp_voices());
    /* Reloading the patch resets the synth's osc envelopes; re-impose the user's
     * custom env if they have authored one (deferred authority, like melodic). */
    if (s_arp.env_authored) {
        sequencer_core_push_envelope(sequencer_core_arp_synth(), &s_arp.env);
    }
    if (s_arp.filter_authored) {
        sequencer_core_push_filter(sequencer_core_arp_synth(), &s_arp.filter);
    }
    /* Patch reconfig does not change scheduling; no re-emit needed. */
}

void arp_get_envelope(seq_env_t *out)
{
    if (out) *out = s_arp.env;
}

void arp_set_envelope(const seq_env_t *env)
{
    if (!env) return;
    s_arp.env = *env;
    s_arp.env_authored = true;
    sequencer_core_push_envelope(sequencer_core_arp_synth(), &s_arp.env);
    ESP_LOGI(TAG, "arp env -> A%u D%u S%u%% R%u",
             (unsigned)s_arp.env.attack_ms, (unsigned)s_arp.env.decay_ms,
             (unsigned)s_arp.env.sustain_pct, (unsigned)s_arp.env.release_ms);
}

void arp_get_filter(seq_filter_t *out)
{
    if (out) *out = s_arp.filter;
}

void arp_set_filter(const seq_filter_t *f)
{
    if (!f) return;
    s_arp.filter = *f;
    s_arp.filter_authored = true;
    sequencer_core_push_filter(sequencer_core_arp_synth(), &s_arp.filter);
    ESP_LOGI(TAG, "arp filter -> type%u %.0fHz Q%.2f en=%d",
             s_arp.filter.filter_type, (double)s_arp.filter.cutoff_hz,
             (double)s_arp.filter.resonance, s_arp.filter.enabled);
}

void arp_set_slot(uint8_t idx, int16_t chromatic_note)
{
    if (idx >= ARP_MAX_SLOTS) return;
    if (chromatic_note >= 0) {
        chromatic_note = (int16_t)sequencer_core_clamp_melodic_note(chromatic_note);
    } else if (chromatic_note == ARP_REST) {
        /* leave as -2: deliberate silent step */
    } else {
        chromatic_note = -1;
    }
    if (s_arp.slots[idx] == chromatic_note) return;
    s_arp.slots[idx] = chromatic_note;
    arp_mark_dirty();
}

/* Perform a pending re-emit, if any. Called once per UI frame so rapid edits
 * coalesce into a single refresh. Cheap no-op when nothing changed. */
void arp_core_service(void)
{
    if (!s_arp_dirty) return;
    s_arp_dirty = false;
    arp_core_refresh();
}

/* ── Getters ─────────────────────────────────────────────────────────── */

bool       arp_get_enabled(void)    { return s_arp.enabled; }
arp_dir_t  arp_get_direction(void)  { return s_arp.dir; }
uint8_t    arp_get_octaves(void)    { return s_arp.octaves; }
arp_rate_t arp_get_rate(void)       { return s_arp.rate; }
uint8_t    arp_get_gate_pct(void)   { return s_arp.gate_pct; }
uint8_t    arp_get_scale(void)      { return s_arp.scale_index; }
uint8_t    arp_get_root_note(void)  { return s_arp.root_note; }
uint16_t   arp_get_patch(void)      { return s_arp.patch; }

const char *arp_rate_name(arp_rate_t rate)
{
    if (rate >= ARP_RATE_COUNT) return "?";
    return s_rate_names[rate];
}

int16_t arp_get_slot(uint8_t idx)
{
    if (idx >= ARP_MAX_SLOTS) return -1;
    return s_arp.slots[idx];
}

int16_t arp_get_slot_snapped(uint8_t idx)
{
    if (idx >= ARP_MAX_SLOTS || s_arp.slots[idx] < 0) return -1;
    return (int16_t)arp_snap((uint8_t)s_arp.slots[idx]);
}

uint8_t arp_active_slot_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++) {
        if (s_arp.slots[i] < 0) break;
        n++;
    }
    return n;
}

uint8_t arp_active_step_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < ARP_MAX_SLOTS; i++)
        if (s_arp.slots[i] != -1) n++;
    return n;
}
