#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Boot demo songs ("Billie Jean", ports of the upstream examples) ─────
 * One-tap auditions of the box's engine, no host: each rebuilds the song from
 * the public sequencer_core API and starts the transport.
 *
 *  demo_billiejean_load()          - dry 808 + G-minor bass (16-step)
 *  demo_billiejean_scheduled_load()- adds a 2-bar Juno pad progression
 *                                    (Fmin Amin Bb Amin) and global reverb,
 *                                    matching BillieJeanScheduled.ino. The
 *                                    example's staggered build-up needs
 *                                    host-side scheduling, so the native
 *                                    version plays the full arrangement from
 *                                    bar 1.
 *
 * Destructive by design: melodic layers are deleted and the drum-layer pattern
 * is overwritten, so the current song is replaced (recoverable by re-loading a
 * saved project). Runs on the UI task to satisfy the layer applier contract. */
bool demo_billiejean_load(void);
bool demo_billiejean_scheduled_load(void);

#ifdef __cplusplus
}
#endif