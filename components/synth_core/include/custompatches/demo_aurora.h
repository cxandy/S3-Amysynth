#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── "Aurora": an original chill-synth arrangement for this box. ─────────
 * 100 BPM, A minor, two-bar loop: 4-on-floor drums with off-beat 8th hats,
 * a syncopated A/F bass walk, warm Juno-style pad stabs, and an FM E.Piano
 * hook on an eighth-note grid with a rest at each bar top. Light global
 * reverb + echo smear the pads and lead together.
 *
 * Built purely from the public sequencer_core API like the Billie Jean
 * ports, so it reloads/is editable identically. Destructive by design: the
 * melodic layers are deleted and the drum pattern overwritten (recoverable
 * by re-loading a saved project); runs on the UI task to satisfy the layer
 * applier contract. */
bool demo_aurora_load(void);

#ifdef __cplusplus
}
#endif