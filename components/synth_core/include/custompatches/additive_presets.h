#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Fixed additive/partials presets (SEQ_PATCH_ADDITIVE_ORGAN/BELL) ─────────
 * Fixed-parameter voices built on additive_voice_configure_track(): a drawbar
 * organ (first 8 harmonics, 1/n roll-off, sustained by the parent envelope)
 * and an inharmonic bell (free-bar partial ratios with staggered per-partial
 * ring-down). Unused partials are silenced via level=0 rather than omitted so
 * the osc layout stays uniform.
 *
 * These are starting points, not final voicings: without on-hardware
 * listening the exact ratios/levels/decays are a best-effort sound-design
 * approximation (same caveat as fm_presets.h). */
void additive_preset_configure_track(uint8_t synth_id, uint16_t patch,
                                     uint16_t num_voices);

#ifdef __cplusplus
}
#endif
