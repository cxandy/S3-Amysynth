# AMY Local Edits

Edits applied on top of the upstream `shorepine/amy` submodule.
Upstream commit: `2516477` (v1.2.12, 2026-06-24).

All edits are marked `// LOCAL EDIT` in the source. ESP32-S3-specific edits are
permanent (upstream has no concept of IRAM/DRAM placement or FreeRTOS task
signatures); the two bug fixes below were PRd upstream and are no longer here.

## Dropped (merged upstream)

| PR | Description |
|----|-------------|
| [#740](https://github.com/shorepine/amy/pull/740) + [#743](https://github.com/shorepine/amy/pull/743) | `chained_osc` NULL guard in `render_osc_wave` (amy.c) |
| [#744](https://github.com/shorepine/amy/pull/744) | `init_stereo_reverb()` → `bool` return + OOM crash safety (delay.h, delay.c, amy.h, amy.c, api.c) |

## Active local edits

### `src/amy.h` — Kconfig-gated fixed-point toggle

`#define AMY_USE_FIXEDPOINT` replaced with a `#ifdef CONFIG_AMY_USE_FIXEDPOINT`
guard. Enabled via menuconfig: **AMY Synthesizer → Use fixed-point arithmetic**
(default off). Requires `components/amy/Kconfig` (new file, not upstreamed).

The ESP32-S3 LX7 FPU makes float equal-or-faster; fixed-point was designed for
RP2040. The option is preserved for comparison or future portability needs.

### `src/amy_fixedpoint.h` — float-mode fallbacks and `ldexpf` SHIFTL/SHIFTR

Two edits to the `#ifndef AMY_USE_FIXEDPOINT` (float mode) section:

1. **`MUL5A_SS` / `MUL6A_SS` float fallbacks** — upstream defines these only in
   the fixed-point path; `oscillators.c` uses them unconditionally so the float
   build fails without them. Added `(a) * (b)` fallbacks.

2. **`SHIFTL` / `SHIFTR` use `ldexpf` instead of `exp2f`** — the original float
   macros were `(s) * exp2f(b)`. When `b` is a runtime variable (e.g.
   `exp2_lut()` integer part), `exp2f(runtime_int)` cannot be constant-folded
   and emits a transcendental libcall. `ldexpf(s, b)` is the correct primitive
   for ×2^n scaling and is never worse. **Caveat (verified by objdump):** on
   this Xtensa LX7 toolchain GCC does *not* lower `ldexpf` (or `floorf`) to the
   hardware `FLOOR.S`/exponent ops — both remain `call8` libcalls. So this is a
   correctness/clarity win and a marginal speedup at most, NOT the fix for
   float-mode CPU cost. Float mode is dominated by per-sample `floorf` libcalls
   in `INT_OF_S` / `S_FRAC_OF_S` / `P_WRAPPED_SUM` (see note below); fixed-point
   remains the product mode on this target.

### `src/amy.h` — IRAM_ATTR / DRAM_ATTR macros

Added `AMY_IRAM_ATTR` and `AMY_DRAM_ATTR` macro definitions inside the
`#ifdef ESP_PLATFORM` block. On non-ESP they expand to nothing.

- `AMY_IRAM_ATTR` → `IRAM_ATTR` — places functions in internal IRAM. Used
  instead of a linker fragment because GCC LTO renames `.text.*` sections in
  ltrans, so fragment-based `noflash` rules silently miss. IRAM_ATTR on the
  symbol itself (`.iram1.*`) survives LTO.
- `AMY_DRAM_ATTR` → `DRAM_ATTR` — places const data tables in internal DRAM
  (fast SRAM), not IRAM which is instruction-only and unsafe for halfword reads.

### `src/clipping_lookup_table.h` — DRAM placement

`clipping_lookup_table[4914]` annotated with `AMY_DRAM_ATTR`. This 9.6 KB table
is read on every output sample in `amy_fill_buffer`. Without this it lives in
flash `.rodata` and is served via the PSRAM XIP cache, causing cache-miss stalls
in the inner sample loop.

### `src/amy.c` — Render lock

`amy_render()` annotated with `AMY_IRAM_ATTR` and wrapped with
`amy_grab_lock()` / `amy_release_lock()` spanning the entire function body.

**Why:** `synth[]` / `msynth[]` arrays are structurally mutated by
`free_osc` / `alloc_osc` / `reset_osc` / patch loads on Core 0, while the
render walks the same pointers on Core 1. Without the lock a patch toggle can
free `synth[osc]` between the NULL check and the deref in `hold_and_modify`,
producing a `LoadProhibited` fault (EXCVADDR=0x8).

### `src/amy.h` — lock accessor prototypes

Added `extern SemaphoreHandle_t amy_queue_lock;` (ESP_PLATFORM branch, missing
alongside the existing `_WIN32`/`_POSIX_THREADS` externs) plus unconditional
prototypes for `amy_grab_lock(void)` / `amy_release_lock(void)` /
`amy_init_lock(void)`, none of which upstream declares anywhere despite every
platform branch in `amy.c` defining them.

**Why:** the runtime PCM sampler (`custompatches/sample_rec.c`) needs to grab
the render lock around its own `pcm_load()`/`pcm_unload_preset()` calls —
`pcm.c`'s memory-preset linked list is walked unlocked by `render_pcm()`
inside the render body, so mutating it from another task without the lock
races the render task across cores. `add_delta_to_queue()` already does
exactly this internally; this edit just lets code outside `amy.c` do the same
without an implicit-declaration warning. **Upstream-PR candidate** — the gap
is platform-agnostic (every accessor is unprototyped on every platform), not
an ESP32-specific fix.

### `src/envelope.c` — IRAM hot path

`compute_mod_value`, `compute_mod_scale`, `compute_breakpoint_scale` annotated
with `AMY_IRAM_ATTR`. Called per-sample from the render hot path.

### `src/filters.c` — IRAM hot path

`dsps_biquad_f32_ansi`, `dsps_biquad_f32_ansi_split_fb`,
`dsps_biquad_f32_ansi_split_fb_once`, `dsps_biquad_f32_ansi_split_fb_twice`,
`scan_max`, `parametric_eq_process`, `filter_process` annotated with
`AMY_IRAM_ATTR`. All are in the per-block render loop.

### `src/log2_exp2.c` — IRAM hot path

`log2_lut`, `exp2_lut` annotated with `AMY_IRAM_ATTR`. Used in pitch/amp
calculations per sample.

### `src/oscillators.c` — IRAM hot path

`render_lut_fm_fb`, `render_lut_fb`, `render_lut_fm`, `render_lut`,
`render_lut_cub`, `render_lpf_lut` annotated with `AMY_IRAM_ATTR`. Core of the
per-sample wavetable rendering.

### `src/i2s.c` — IDF 6.0 task signature + UART MIDI guard

1. `esp_fill_audio_buffer_task()` → `esp_fill_audio_buffer_task(void *pvParameters)`.
   IDF 6.0 FreeRTOS requires `TaskFunction_t` (`void (*)(void*)`); the old
   no-parameter form causes a type mismatch warning/error on `xTaskCreatePinnedToCore`.

2. `amy_update_tasks()`: `esp_poll_midi()` is now guarded by
   `if (amy_global.config.midi & AMY_MIDI_IS_UART)`. Without the guard, calling
   `esp_poll_midi()` with an uninstalled UART driver emits `uart_read_bytes()`
   errors at runtime (this project uses `AMY_MIDI_IS_NONE`).

### `src/amy_midi.c` — IDF 6.0 task signature

`run_midi_task()` → `run_midi_task(void *pvParameters)`. Same FreeRTOS
`TaskFunction_t` fix as `i2s.c` above.

### `src/sequencer.c` — SEQ_LOCK + active-tag O(1) scan

Two related improvements:

1. **SEQ_LOCK mutex** (`s_seq_lock`): `sequences[]` is written by
   `sequencer_add_event()` (any task) and read by `sequencer_process_tick()`
   (esp_timer ISR context on Core 0). On SMP these run concurrently. A dedicated
   mutex prevents torn reads/writes. Cannot reuse `amy_queue_lock` because
   `add_delta_to_queue()` re-acquires it inside the tick scan — doing so with a
   non-recursive mutex would deadlock.

2. **Active-tag dense index** (`s_active_tags`, `s_tag_slot`): The old scan
   iterated `0..highest_tag` where `highest_tag` is a high-water mark that never
   decreases (the arp pushes it to ~1119 and it stays there). Every 500 µs tick
   on Core 0 was O(1119) scanning mostly-NULL slots. Replaced with a compact
   dense list of only the tags that currently have deltas: the tick is now
   O(active events) regardless of tag magnitude. `s_tag_slot[tag]` → position in
   the dense list (or -1) enables O(1) removal via swap-with-last.

### `src/amy.h` + `src/amy.c` — COARSE profiler mode

Upstream gates the whole profiler behind `AMY_DEBUG`, which times *every* tag —
including per-osc/inner tags that fire dozens of times per block **inside** the
`AMY_RENDER` window. Those nested timestamp calls inflate the `AMY_RENDER` total,
which would overstate the parallelizable fraction in a dual-core feasibility
measurement.

Added a second, lighter profiling level, `AMY_PROFILE_COARSE` (Kconfig:
`AMY_PROFILE_MODE`, see `components/amy/Kconfig` + `CMakeLists.txt`):

- The profiler tables / timers / `amy_profiles_init/print` now compile under
  `#if defined(AMY_DEBUG) || defined(AMY_PROFILE_COARSE)` (was `#ifdef AMY_DEBUG`).
- In coarse mode `AMY_PROFILE_START/STOP` act only on the outer stage tags
  (`AMY_RENDER`, `AMY_FILL_BUFFER`, `AMY_EXECUTE_DELTAS`, `AMY_ESP_FILL_BUFFER`)
  via `AMY_TAG_IS_COARSE(tag)`. Because `tag` is a compile-time enum literal at
  every call site, the guard folds to a constant and inner call sites compile to
  nothing — zero overhead and no inflation of `AMY_RENDER`.
- Macros expand to a complete statement with no trailing `;`, matching upstream
  call sites that omit the semicolon (e.g. `AMY_PROFILE_START(AMY_RENDER)`).

Full `AMY_DEBUG` behaviour is unchanged (select `AMY_PROFILE_FULL`). See
`docs/dual-core-render-analysis.md` for why and how this is used.

**Cross-core reset fix (`AMY_PROFILE_INIT`):** the dump runs on Core 0 (the
`app_main` idle loop) while render `START/STOP` run on Core 1. The upstream reset
zeroed `profiles[tag].start`; when that landed between a Core-1 `START` and
`STOP`, the `STOP` computed `(now - 0)` ≈ uptime, producing one ~uptime-µs spike
per window on whichever tag was mid-flight (see `docs/AMY-PROFILE-LOG.md` — every
window had exactly one tag reading billions of µs). Fix: `AMY_PROFILE_INIT` no
longer zeroes `.start` (it is only ever read by a `STOP` that follows a `START`,
so it never needs zeroing). `us_total`/`calls` are still reset each window; the
remaining `+=`-vs-`=0` race can at most carry one window's totals into the next
(both `us_total` and `calls` scale together, so **`us per call` stays correct**;
only that window's `% wall` may read high). Benefits coarse and full modes.

## Deferred / needs porting

| Edit | Status |
|------|--------|
| Block-processed ESP32 stereo reverb (`delay.c`, `#ifdef ESP_PLATFORM`) | **Not applied.** Upstream changed `stereo_reverb()` to take `reverb_params_t *rev` (all delay state inside struct); the block-processed optimization needs adapting to the new API before it can be reapplied. |
