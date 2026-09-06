#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── "JukeBox": a self-playing generative acid machine. ──────────────────
 * Imagined after the ACID-JUKEBOX of the AcidBox/AciduinoBox grooveboxes
 * (their AcidBanger.ino): no fixed song, just a small restorable-seed LFSR,
 * a minor-pentatonic note set and per-bar re-rolls of the box's four
 * sequencer layers (808-ish drums / acid bass / FM lead hook / pad stabs)
 * with auto-breaks and an energy ramp that builds across each phrase.
 *
 * Load it once from the DEMOS menu: it fixes a 4-layer topology, starts the
 * transport and then rewrites steps in place from the UI task while it plays
 * (voice/amp/gate stay constant; only step maps + track base notes change).
 * It deactivates itself when the transport stops, so an idle machine never
 * overwrites a project that was edited afterwards. */
bool jukebox_demo_load(void);

/* True while the generator is live and rewriting steps. */
bool jukebox_demo_is_active(void);

/* Drop the generator without touching layers (running service no-ops). */
void jukebox_demo_deactivate(void);

/* UI-task service: poll the transport, count bars and re-roll patterns.
 * Returns true when steps were rewritten this call (drain then re-syncs the
 * UI mirror so the sequencer grid shows the live pattern). No-op & false
 * while inactive. */
bool jukebox_demo_service(void);

#ifdef __cplusplus
}
#endif