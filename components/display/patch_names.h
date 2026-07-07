#pragma once
/*
 * patch_names — optional human-readable names for the AMY built-in patch banks.
 *
 * Lives in the display component so both the OLED renderer
 * (display_seq.c) and the synth UI layer can use it without a dependency
 * cycle. The Kconfig option CONFIG_SEQ_PATCH_SHOW_NAMES is global, so it gates
 * this table regardless of which component defines the symbol.
 *
 * The name strings are large-ish (≈257 short strings + a pointer table) so they
 * are fully gated behind CONFIG_SEQ_PATCH_SHOW_NAMES. When that option is off,
 * the table is NOT compiled in and patch_name_for() resolves (via the macro
 * below) to a no-op that returns NULL, costing zero flash.
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

/* Returns a static human-readable name for the given patch number, or NULL if
 * the number is out of the known 0..256 range. The returned pointer is valid
 * for the lifetime of the program (points into a const table). */
const char *patch_name_for(uint16_t patch);

/* Returns a static name for a PCM ROM drum preset, or NULL if out of range
 * (wavetable entries and runtime memory presets have no names — callers show
 * the bare number). The table matches the compiled-in ROM bank: gamma808
 * (CONFIG_AMY_PCM_GAMMA808) or the legacy pcm_tiny set. */
const char *pcm_preset_name_for(uint16_t preset);

#else  /* names compiled out */

/* No name table linked; callers fall back to showing the bare number. The
 * static inline keeps call sites identical without pulling in any data. */
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
