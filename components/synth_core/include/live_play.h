#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Live-play voice: a dedicated AMY synth slot played directly from a MIDI
 * transport (BLE MIDI in Phase 1), independent of the sequencer tracks.
 * Compiled only under CONFIG_SYNTH_WIRELESS.
 *
 * The note entry points match the midi_sink_t signatures (wireless/midi_core)
 * so main can wire them up without an adapter; they run on the transport's
 * task and emit untagged apply-now AMY events through amy_helpers (the drone
 * precedent - no sequencer tags, applied next render block). */

/* Lazy first configure of the live slot (patch load). Heavy: call from the
 * synth_ui task (radio session_start hook), not from input/transport tasks. */
void live_play_ensure_ready(void);

/* midi_sink_t-compatible note entry points. Channel is ignored (omni). */
void live_play_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void live_play_note_off(uint8_t channel, uint8_t note);

/* Release every held note (session stop / central disconnect - stuck-note
 * safety). Cross-task safe. */
void live_play_all_notes_off(void);

/* Patch selection (Wireless menu page; same catalog as melodic/arp). */
uint16_t live_play_get_patch(void);
void     live_play_set_patch(uint16_t patch_number);

#ifdef __cplusplus
}
#endif
