#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS

#include "live_play.h"
#include "amy_helpers.h"
#include "sequencer_core.h"
#include "seq_core_config.h"
#include "seq_clamp.h"
#include "esp_log.h"

static const char *TAG = "live_play";

/* Slot 10 is the gap between the drum block (6..9) and the melodic base (11)
 * in the fixed synth-slot map (seq_core_config.h); max_synths=68 already
 * covers it. 4 voices matches the arp - enough for chords under one hand,
 * ample osc headroom even for 7-osc FM voices against AMY's 250-osc pool. */
#define LIVE_SYNTH   10
#define LIVE_VOICES  4

static uint16_t s_patch = SEQ_MEL_PATCH;
static bool     s_ready = false;

/* Held-note bitmap (128 bits) so all_notes_off releases exactly what is
 * sounding. Written from the transport task, drained from synth_ui / NimBLE
 * host on stop/disconnect; per-bit races are harmless (a spurious note-off
 * to an idle voice is a no-op in AMY). */
static volatile uint32_t s_held[4];

static void live_note(uint8_t note, float velocity)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth     = LIVE_SYNTH;
    e->midi_note = (float)note;
    e->velocity  = velocity;
    amy_helpers_event_send(e);
}

void live_play_ensure_ready(void)
{
    if (s_ready) return;
    sequencer_core_configure_synth_slot(LIVE_SYNTH, s_patch, LIVE_VOICES);
    s_ready = true;
    ESP_LOGI(TAG, "live slot %u ready (patch %u, %u voices)",
             (unsigned)LIVE_SYNTH, (unsigned)s_patch, (unsigned)LIVE_VOICES);
}

void live_play_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    (void)channel;
    if (!s_ready || note > 127) return;
    s_held[note >> 5] |= 1u << (note & 31u);
    live_note(note, (float)velocity * (1.0f / 127.0f));
}

void live_play_note_off(uint8_t channel, uint8_t note)
{
    (void)channel;
    if (!s_ready || note > 127) return;
    s_held[note >> 5] &= ~(1u << (note & 31u));
    live_note(note, 0.0f);
}

void live_play_all_notes_off(void)
{
    if (!s_ready) return;
    for (uint8_t w = 0; w < 4; w++) {
        uint32_t bits = s_held[w];
        s_held[w] = 0;
        while (bits) {
            uint8_t bit = (uint8_t)__builtin_ctz(bits);
            bits &= bits - 1u;
            live_note((uint8_t)(w * 32u + bit), 0.0f);
        }
    }
}

uint16_t live_play_get_patch(void)
{
    return s_patch;
}

void live_play_set_patch(uint16_t patch_number)
{
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_FULL_MAX);
    if (s_patch == patch_number) return;
    s_patch = patch_number;
    if (s_ready) {
        /* Reconfigure kills sounding voices (osc topology may change);
         * clear the bitmap so stale note-offs are not replayed later. */
        for (uint8_t w = 0; w < 4; w++) s_held[w] = 0;
        sequencer_core_configure_synth_slot(LIVE_SYNTH, s_patch, LIVE_VOICES);
    }
}

#endif /* CONFIG_SYNTH_WIRELESS */
