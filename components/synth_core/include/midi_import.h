#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Best-effort SMF (standard MIDI file) -> AMYSONG text.
 *
 * Mirror of tools/midi2amysong.py: the first `bars` bars (16 or 32 sixteenths)
 * are quantized to the 16th grid and the rest is dropped, so the result is a
 * loop. Channel drumch (default 9 = MIDI channel 10) is split into the drum
 * layer's kick/snare/
 * hat1/perc tracks (GM note map); up to 3 non-drum channels become melodic
 * layers, single-note-per-step, top note wins. Tempo comes from set-tempo meta
 * (or 120 bpm), patch defaults to 256.
 *
 * Returns 0 and fills `out` (NUL-terminated, at most out_cap bytes) on
 * success; returns -1 and fills `err` with a short reason otherwise.
 *
 * NOTE: moved here from wifi_importer (musical import lives in synth_core);
 * the WiFi and WebSerial importers both call it.
 */
int midi_amysong_convert(const uint8_t *data, size_t len,
                         int bars, int patch, const char *name,
                         char *out, size_t out_cap,
                         char *err, size_t err_cap);

/* Whole-song arrangement: 0xFF 0x06/0x07 section markers split the file into
 * scenes; each section's leading one/two bars become loop cells assigned to
 * the 3 melodic layers in first-appearance order (later sections reuse a cell
 * via the scene mask), the densest-drum section owns the drum cell, and the
 * output ends in a `scene`/`song` block that replays the section order.
 *
 * Requires at least 2 distinct in-time section markers; otherwise returns -1
 * with "need at least 2 section markers...". `patch` is the fallback melodic
 * patch when no program change precedes a section (prg changes win), `name`
 * is a fallback for the MIDI song-name meta.
 *
 * Workspace is module-static: call from the single sequencer-applier task
 * only (usb_import_service / wifi_import_service), like song_import_apply.
 */
int midi_amysong_arrange_convert(const uint8_t *data, size_t len, int patch,
                                 const char *name,
                                 char *out, size_t out_cap,
                                 char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif