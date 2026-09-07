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
 * loop. Channel drumch (default 10) is split into the drum layer's kick/snare/
 * hat1/perc tracks (GM note map); up to 3 non-drum channels become melodic
 * layers, single-note-per-step, top note wins. Tempo comes from set-tempo meta
 * (or 120 bpm), patch defaults to 256.
 *
 * Returns 0 and fills `out` (NUL-terminated, at most out_cap bytes) on
 * success; returns -1 and fills `err` with a short reason otherwise.
 */
int midi_amysong_convert(const uint8_t *data, size_t len,
                         int bars, int patch, const char *name,
                         char *out, size_t out_cap,
                         char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif