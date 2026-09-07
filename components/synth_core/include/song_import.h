#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── AMYSONG import (WiFi / any text source) ────────────────────────────────
 * A tiny line-oriented song format that round-trips into the sequencer's
 * live model and then a project slot, so a song arrives as text instead of
 * manual step entry. Grammar (one statement per line, '#' comments):
 *
 *   amysong 1              optional version gate (any other value rejected)
 *   name "Title"           optional, <= 15 chars (PROJECT_NAME_LEN - 1)
 *   bpm 120                optional, 40..240 (default 120)
 *   pattern 32             optional, 16 or 32 (default 16)
 *   layer melodic <patch>  opens a melodic layer; patch = AMY/preset number
 *     base <0..127>        optional (default 60): current track's MIDI base
 *     notes <tok>...       one row = one track (max 4/layer; max 3 layers):
 *                          `.` rest, `0` base note, `+n`/`-n` offset
 *   layer drum             opens the drum block (at most one)
 *     hit <track> <tok>..  track 0..3 (kick/snare/hat1/hat2), `.` off /
 *                          `x` on; max 4 rows
 *
 * Steps read up to `pattern` per row; extra tokens are ignored, missing
 * tokens are rests. Parse-then-apply is atomic: on any error nothing is
 * applied and NO save happens.
 *
 * Layer-wise it mirrors what the sequencer UI builds: layer 0 is always the
 * drum layer, melodic layers sit above it. The importer clears every layer
 * above the drum, rebuilds from the text, then saves the result into the
 * requested project slot (a later Load replays it exactly). */

/* Parse `text`, apply it to live state and save into project `slot`.
 *
 * MUST run on the sequencer single-applier task (synth_ui_task): layer
 * rebuild goes through sequencer_core_add/delete_layer, which only that
 * task may call. On failure returns false with a human-readable reason in
 * `err` (line number included unless `err` is NULL). */
bool song_import_apply(uint8_t slot, const char *text, const char *name_fallback,
                       char *err, size_t errsz);

#ifdef __cplusplus
}
#endif