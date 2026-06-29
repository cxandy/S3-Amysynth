# Per-Layer Melodic Patch Isolation

**Date:** 2026-06-29  
**Status:** Approved  
**Scope:** `sequencer_core.c`, `synth_ui.c` — no struct or AMY changes

---

## Problem

All melodic layers currently share a single sound profile. Changing the patch on any melodic layer changes it on every other melodic layer simultaneously. This makes it impossible to run two melodic layers with different timbres.

### Root cause

`sequencer_core.c` holds `static uint16_t s_melodic_patch` as a **runtime shared state** (not just a boot default). `sequencer_core_set_melodic_patch()` broadcasts the new patch to every melodic layer and calls `sequencer_configure_synth()` on each. `synth_ui.c` mirrors this global into all display-layer cache entries every 50 ms via `synth_ui_refresh_layer_state()`.

### What is already per-layer (no changes needed)

`seq_layer_t` already stores the following independently per-layer:

| Field | Per-layer? |
|-------|-----------|
| `patch` | Yes — field exists; always overwritten by global |
| `env[SEQ_TRACKS]` + `env_authored[]` | Yes — ADSR per-row |
| `filter[SEQ_TRACKS]` + `filter_authored[]` | Yes — filter per-row |
| `lfo[SEQ_TRACKS]` + `lfo_authored[]` | Yes — LFO per-row |
| `amp_scale[SEQ_TRACKS]` | Yes — amplitude trim per-row |
| `synth_id[SEQ_TRACKS]` | Yes — distinct AMY slots |

The ADSR graph editor in `synth_ui.c` already records `s_graph_layer = seq_state.active_layer_idx` on open and commits via `sequencer_core_set_melodic_envelope(s_graph_layer, track, ...)`. ADSR is already correctly isolated.

The only structural problem is `s_melodic_patch` / `set_melodic_patch()` broadcasting at the patch level.

---

## Design

### Approach: Targeted API swap (minimal change)

Demote `s_melodic_patch` from runtime shared state to **new-layer boot default only**. Replace the broadcast setter/getter pair with per-layer equivalents. No struct changes; no AMY changes.

---

### Core changes — `sequencer_core.c`

#### Remove from public API

```c
// Removed — replaced by per-layer equivalents below
void     sequencer_core_set_melodic_patch(uint16_t patch_number);
uint16_t sequencer_core_get_melodic_patch(void);
```

These are declared in `sequencer_core.h` and must be removed there too.

#### Add to public API

```c
void     sequencer_core_set_layer_patch(uint8_t layer_idx, uint16_t patch);
uint16_t sequencer_core_get_layer_patch(uint8_t layer_idx);
```

**`set_layer_patch(layer_idx, patch)`**:
- Clamp `patch` to `[0, SEQ_PATCH_BASS_MAX]` (same range as the old setter).
- Guard: `layer_idx >= s_num_layers` or `layer->type != SEQ_LAYER_MELODIC` → return early, no-op.
- Early-exit if `layer->patch == patch` (no change).
- Set `s_layers[layer_idx].patch = patch`.
- Call `sequencer_configure_synth(layer_idx)` — touches only this one layer.
- Log: `"melodic L%u patch -> %u"`.

**`get_layer_patch(layer_idx)`**:
- Return `s_layers[layer_idx].patch` if `layer_idx < s_num_layers`.
- Return `SEQ_MEL_PATCH` as safe fallback for out-of-range index.

#### `s_melodic_patch` — kept as boot default only

`s_melodic_patch` is reset to `SEQ_MEL_PATCH` in `sequencer_core_init()` (already happens at line 805). It is used **only** in `sequencer_core_add_layer()` to initialize `layer->patch = s_melodic_patch`. It is never written after that point.

> Note: The user plans to change `SEQ_MEL_PATCH` from 128 to 257 (start of wave primitives). That change is independent of this feature and requires only editing the constant definition in `seq_defaults.h`.

#### `sequencer_core_add_layer()` — no change

```c
layer->patch = s_melodic_patch;   // unchanged: boot default
```

New layers always start at `SEQ_MEL_PATCH`. Each layer's patch diverges independently after creation.

---

### UI changes — `synth_ui.c`

#### `synth_ui_cycle_melodic_patch(int delta)`

**Before:** calls `sequencer_core_get_melodic_patch()` + `sequencer_core_set_melodic_patch()`.

**After:** scoped to the active layer:

```c
void synth_ui_cycle_melodic_patch(int delta)
{
    uint8_t li  = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return;
    if (seq_state.layers[li].type != SEQ_LAYER_MELODIC) return;

    int dir = (delta > 0) ? 1 : -1;
    uint16_t cur  = sequencer_core_get_layer_patch(li);
    uint16_t next = next_patch_in_cycle(cur, dir);
    sequencer_core_set_layer_patch(li, next);

    uint16_t applied = sequencer_core_get_layer_patch(li);
    seq_state.layers[li].patch = applied;   // sync display cache

    const char *name = patch_name_for(applied);
    if (name)
        ESP_LOGI(TAG, "L%u melodic patch -> %u (%s)", li, (unsigned)applied, name);
    else
        ESP_LOGI(TAG, "L%u melodic patch -> %u", li, (unsigned)applied);
}
```

#### `synth_ui_sync_melodic_patch_cache()` — remove

This function's sole purpose was to read the global and apply it to all layer display caches. With the global eliminated, it has no role. Its only caller is `synth_ui_cycle_melodic_patch()`, which now handles the cache inline.

Remove the static function and its declaration/call sites.

#### `synth_ui_refresh_layer_state()` (line 1448)

**Before:**
```c
layer->patch = sequencer_core_get_melodic_patch();
```

**After:**
```c
layer->patch = sequencer_core_get_layer_patch(li);
```

This is the 50 ms display-refresh path. After the fix it reads the per-layer patch, so the OLED header shows the correct patch for each layer as you cycle through them.

---

### Header changes — `sequencer_core.h`

- Remove declarations of `sequencer_core_set_melodic_patch()` and `sequencer_core_get_melodic_patch()`.
- Add declarations of `sequencer_core_set_layer_patch()` and `sequencer_core_get_layer_patch()`.

---

## Behaviour after the fix

| Action | Before | After |
|--------|--------|-------|
| Patch knob on L1 | Changes L1 + L2 + L3 | Changes L1 only |
| Patch knob on L2 | Changes L1 + L2 + L3 | Changes L2 only |
| Add new melodic layer | Gets current `s_melodic_patch` | Gets `SEQ_MEL_PATCH` constant |
| OLED header patch display | Same number on all layers | Per-layer patch number |
| ADSR/filter/LFO editing | Already per-layer | Unchanged |

---

## Files changed

| File | Change |
|------|--------|
| `components/synth_core/include/sequencer_core.h` | Remove old API; add new per-layer API |
| `components/synth_core/sequencer_core.c` | Replace `set/get_melodic_patch()` with `set/get_layer_patch()`; demote `s_melodic_patch` to boot-default-only role |
| `components/synth_core/synth_ui.c` | Scope patch cycle to active layer; remove `synth_ui_sync_melodic_patch_cache()`; fix `refresh_layer_state()` read |

---

## Out of scope

- `num_voices` and `synth_flags` per-layer UI controls (currently fixed at `SEQ_MEL_VOICES`/`0` for all layers; a future feature)
- "Broadcast to all layers" gesture — explicitly excluded; strictly per-layer
- Arp, drone, drum patch APIs — unchanged
- `SEQ_MEL_PATCH` constant value change (128 → 257) — separate, independent edit
