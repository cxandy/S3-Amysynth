#pragma once
/*
 * patch_names — optional human-readable names for the AMY built-in patch banks.
 *
 * Lives in the display component so both the OLED renderer (display_seq.c) and
 * the synth UI layer can use it without a dependency cycle.
 *
 * Fully gated behind CONFIG_SEQ_PATCH_SHOW_NAMES: with it off the table is not
 * compiled in and patch_name_for() becomes a NULL-returning inline, costing
 * zero flash.
 *
 * Patch number map (matches components/amy/src/patches.h):
 *   0..127  : Roland Juno-106 presets
 *   128..255: Yamaha DX7 presets
 *   256     : AMY built-in heterodyne piano
 */

#include <stdint.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_SEQ_PATCH_SHOW_NAMES

/* Static human-readable name for the patch number, or NULL when out of the
 * known range. The pointer is valid for the program's lifetime. */
const char *patch_name_for(uint16_t patch);

/* Static name for a PCM ROM drum preset, or NULL if out of range (wavetable
 * and runtime memory presets have no names). Matches the compiled-in ROM bank:
 * gamma808 (CONFIG_AMY_PCM_GAMMA808) or the legacy pcm_tiny set. */
const char *pcm_preset_name_for(uint16_t preset);

#else  /* names compiled out */

/* No name table linked; callers fall back to the bare number. */
static inline const char *patch_name_for(uint16_t patch)
{
    (void)patch;
    return (const char *)0;
}

static inline const char *pcm_preset_name_for(uint16_t preset)
{
    (void)preset;
    return (const char *)0;
}

#endif /* CONFIG_SEQ_PATCH_SHOW_NAMES */

#ifdef __cplusplus
}
#endif
