# AMY Local Edits

Track local, project-specific changes made against the upstream AMY component here.

## 2026-06-19

- **Performance: pin the per-sample clipping LUT to internal DRAM (`AMY_DRAM_ATTR`).**
  - **What:** `clipping_lookup_table` (`components/amy/src/clipping_lookup_table.h`,
    `const uint16_t[NONLIN_RANGE]` = 4914 entries ≈ 9.6 KB) is read for **every output
    sample** in `amy_fill_buffer` (`amy.c` soft-clip stage, ~line 1813). In flash
    `.rodata` it is served via the PSRAM XIP cache (`CONFIG_SPIRAM_RODATA=y`), so the
    inner output loop can take cache-miss stalls. Moved it to fast internal DRAM.
  - **How:** New `AMY_DRAM_ATTR` macro in `amy.h` (mirrors the existing `AMY_IRAM_ATTR`):
    `= DRAM_ATTR` on `ESP_PLATFORM` (after the already-present `#include <esp_attr.h>`),
    no-op elsewhere. Applied to the table declaration as
    `const uint16_t clipping_lookup_table[NONLIN_RANGE] AMY_DRAM_ATTR PROGMEM = {`.
    `DRAM_ATTR` (data section), **not** `IRAM_ATTR` — IRAM is instruction memory and a
    `uint16_t` table needs word-safe data placement. `PROGMEM` is empty on ESP, kept for
    upstream portability. Edit sites marked inline `// LOCAL EDIT (... 2026-06-19) ...`.
  - **Verified:** `.dram0.data` grew ~+9.8 KB (16,954 → 26,778 B); the table left flash
    rodata. Affordable only because the ring-buffer move below freed ~64 KB internal first.
  - **Risk:** Low. Pure placement change; the table is `const`, never written. Costs ~9.6 KB
    internal DRAM (covered by the freed ring-buffer space).
  - **Rollback:** Remove `AMY_DRAM_ATTR` from the table declaration; optionally remove the
    `AMY_DRAM_ATTR` macro block in `amy.h`.

- **Non-AMY-source change (in `components/usb_audio/usb_audio.c`):** `s_ring_buffer`
  (`int16_t[RING_BUFFER_SIZE]` = 32768 = **64 KB**) was a static array in internal DRAM
  `.bss`, consuming a large share of the scarce internal SRAM. Converted to a pointer
  allocated from PSRAM via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` in
  `usb_audio_init()` (with NULL-check + mutex cleanup on failure). The buffer is accessed at
  USB-frame / render-block granularity (memcpy chunks + mutex-guarded per-sample writes), not
  in the per-sample DSP inner loop, so PSRAM latency is irrelevant. Verified: `.dram0.bss`
  dropped 81,072 → 15,536 B (−64 KB). Net internal SRAM change for both edits: ≈ −55 KB used.

- **Bug fix (crash): reverb delay-line OOM caused NULL-deref panic (`LoadProhibited`).**
  - **Symptom:** Raising **Reverb** from 0 in the menu crashed with
    `Guru Meditation Error: Core 1 panic'ed (LoadProhibited)`, `EXCVADDR=0x00000000`,
    in `stereo_reverb` (via `amy_fill_buffer` / `amy_render_audio` / `amy_update` /
    `amy_usb_render_task`), preceded by `unable to alloc delay line of 4096 samples`.
  - **Root cause:** `init_stereo_reverb()` allocates ~10 delay lines via
    `new_delay_line(..., ram_caps_delay)`. When those allocations fail, the
    `delay_1..ref_6` pointers stay NULL, but `config_reverb()` still committed a
    nonzero `reverb.level`, and the render guard in `amy_fill_buffer` checked **only**
    `reverb.level > 0` — so `stereo_reverb()` ran and dereferenced NULL via
    `DEL_IN(ref_1, ...)`. Echo never crashed because its render guard already checks
    `echo_delay_lines[0] != NULL`; reverb had no equivalent guard. This is a strict
    upstream robustness bug: a failed allocation should disable the effect, not crash.
    (The underlying OOM trigger — `ram_caps_delay` pinned to internal SRAM — is fixed
    separately in `main/main.c`; see the non-AMY note below.)
  - **Fix (AMY source):**
    - `delay.c` / `delay.h`: `init_stereo_reverb()` return type `void` → `bool`. On any
      `new_delay_line()` failure it logs, frees every partial allocation
      (`free_stereo_reverb()`), leaves all pointers NULL, and returns false. New
      `stereo_reverb_ready()` returns `delay_1 != NULL` (init guarantees all-or-nothing).
    - `amy.c` `config_reverb()`: only enables reverb (commits nonzero level + calls
      `config_stereo_reverb`) when `init_stereo_reverb()` succeeds; on failure it forces
      `reverb.level = 0` and sets the new `reverb.alloc_failed` flag.
    - `amy.c` `amy_fill_buffer()`: reverb render guard now
      `reverb.level > 0 && stereo_reverb_ready()` (mirrors the echo guard).
    - `amy.h` `reverb_state_t`: new `bool alloc_failed` field.
    - `api.c`: new `bool amy_reverb_alloc_failed(void)` getter exposing the flag so the
      UI can show a no-serial diagnostics indicator.
  - All edit sites are marked inline with `// LOCAL EDIT (2026-06-19): ...`.
  - **Risk:** Low. Behaviour is unchanged when allocation succeeds (normal case). On
    failure, reverb is simply skipped instead of crashing. The added render-path check is
    one pointer comparison per block.
  - **Rollback:** Revert `init_stereo_reverb()` to `void` (drop `free_stereo_reverb()`,
    the rollback branch, and `stereo_reverb_ready()`); restore the original
    `config_reverb()` body; restore the `reverb.level > 0`-only render guard; remove the
    `reverb_state_t.alloc_failed` field and `amy_reverb_alloc_failed()`.

- **Non-AMY-source change for this fix (in `main/main.c`):** the actual OOM. The
  2026-06-18 perf pass pinned `amy_cfg.ram_caps_delay = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`,
  but the FX delay lines don't fit internally (reverb ~108 KB across 10 lines; echo a single
  65536-sample = 256 KB line) while the internal heap's largest free block is ~32 KB.
  Live heap confirmed PSRAM was nearly empty (psram free ≈ 7.7 MB, largest ≈ 7.6 MB), so
  `ram_caps_delay` was moved to `MALLOC_CAP_SPIRAM`. The hot per-block synth allocations
  (`ram_caps_events`/`synth`/`block`/`fbl`) stay internal. The OLED `OOM!` indicator for the
  Reverb menu lives in first-party `components/synth_core/synth_ui.c` (reads
  `amy_reverb_alloc_failed()`), not in AMY source.

## 2026-06-18

- **Performance pass: place the audio render hot path in internal IRAM/SRAM.** Goal was
  lower per-block render time / more CPU headroom (heap use is not a concern). No functional
  behavior change intended. See `.embedder/plans/1781754140057-shiny-moon.md` for the full
  analysis and rejected options.

  - **`AMY_IRAM_ATTR` macro (`components/amy/src/amy.h`):** new macro = `IRAM_ATTR` on
    `ESP_PLATFORM` (after `#include <esp_attr.h>`), no-op elsewhere. Used instead of an `.lf`
    linker fragment because this build uses GCC LTO, which renames/merges per-function
    `.text.*` sections in the ltrans phase — object/symbol-granularity `noflash` fragment
    rules silently miss and the code stays in flash (verified: symbols stayed at 0x4200…).
    `IRAM_ATTR` rides the symbol's `.iram1` section and survives LTO (verified at 0x4037…).
  - **Functions annotated `AMY_IRAM_ATTR`:**
    - `amy.c`: `combine_controls`, `combine_controls_mult`, `hold_and_modify`, `mix_with_pan`,
      `render_osc_wave`, `amy_render`, `amy_fill_buffer`.
    - `oscillators.c`: `render_lut`, `render_lut_cub`, `render_lut_fm`, `render_lut_fb`,
      `render_lut_fm_fb`, `render_lpf_lut`.
    - `filters.c`: `filter_process`, `dsps_biquad_f32_ansi`, `dsps_biquad_f32_ansi_split_fb`,
      `dsps_biquad_f32_ansi_split_fb_once`, `dsps_biquad_f32_ansi_split_fb_twice`, `scan_max`,
      `parametric_eq_process_top16block`.
    - `delay.c`: `apply_variable_delay`, `apply_fixed_delay`, `stereo_reverb`.
    - `envelope.c`: `compute_mod_value`, `compute_mod_scale`, `compute_breakpoint_scale`.
    - `log2_exp2.c`: `log2_lut`, `exp2_lut`.
    - Several of these (e.g. `filter_process`, `combine_controls*`, `mix_with_pan`, the biquad
      processors, delay walkers) get LTO-inlined into their IRAM callers, so they end up in
      IRAM regardless. `.iram0.text` grew ~57KB → ~71KB; DIRAM ~49.9% used, ~171KB free.
  - **Risk:** Low. IRAM_ATTR only relocates code; semantics unchanged. These functions call
    flash-resident helpers, which is fine while the instruction cache is enabled (normal
    operation — not a cache-disabled / flash-erase context). String literals in the gated
    debug `fprintf` branches stay in flash rodata (only reachable via never-taken `osc==999`
    / debug_flag paths). Coexists with the 2026-06-17 render-lock fix (lock calls are in the
    now-IRAM `amy_render`, calling the flash-resident lock helpers — fine with cache on).
  - **Rollback:** Remove the `AMY_IRAM_ATTR` prefixes from the listed functions and the macro
    + `#include <esp_attr.h>` in `amy.h`.

- **Performance pass: drop `assert()` from pure-DSP hot files (`components/amy/CMakeLists.txt`).**
  The build enables assertions globally (`CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE`),
  which emits runtime checks inside per-sample loops (e.g. `filters.c`
  `assert(FILTER_SCALEUP_BITS == 0)` in the biquad split functions). Appended `-DNDEBUG` via
  `set_property(... APPEND_STRING PROPERTY COMPILE_FLAGS " -DNDEBUG")` to **only**
  `filters.c`, `oscillators.c`, `envelope.c`, `delay.c`, `log2_exp2.c` — files with no
  structural/cross-core state. `amy.c` and the rest of the project keep their asserts
  (osc alloc/free, delta queue, render lock invariants).
  - **Risk:** Low. Only removes always-true asserts from leaf DSP files. Structural safety
    asserts elsewhere are untouched.
  - **Rollback:** Remove the `AMY_DSP_HOT_FILES` block in `components/amy/CMakeLists.txt`.

- **Considered but NOT changed (recorded so we don't re-investigate):**
  - LUT wavetable data placement in DRAM — `PROGMEM` (the tag) is shared with the 102KB
    `pcm` table + piano data, so a blanket redefine is unsafe; per-table `DRAM_ATTR` is
    broad/fragile; and with flash QIO + 32KB I/D-cache the ≤4KB LUTs cache well. Skipped.
  - Active-oscillator index in `amy_render` — would require mutating an active set from the
    zero-amp reaper *inside* the render loop (amy.c:1544), the same mid-iteration mutation
    behind the 2026-06-17 LoadProhibited race. After the IRAM + internal-SRAM moves each
    scan check is only a few cycles, so high risk / low gain. Rejected.
  - `-ffast-math` — AMY uses NaN sentinels (`nanf` / `AMY_IS_SET`); fast-math assumes no
    NaNs and would break those checks. Rejected.

  (Non-AMY-source changes for this pass live in `main/main.c` (ram_caps → internal SRAM) and
  `sdkconfig.defaults` (I-cache 16→32KB, flash QOUT→QIO).)

## 2026-06-17

- **Bug fix (SMP crash): unlocked render path races patch-toggle frees in `components/amy/src/amy.c`**
  - **Symptom:** Intermittent `Guru Meditation Error: Core 1 panic'ed (LoadProhibited)` while toggling patches from the sequencer UI. Backtrace: `combine_controls_mult` (amy.c:1340) → `hold_and_modify` (amy.c:1406) → `render_osc_wave` (amy.c:1486) → `amy_render` (amy.c:1563) → `amy_render_audio` (i2s.c) → `amy_update` → `amy_usb_render_task`. `EXCVADDR=0x00000008` = NULL `synth[osc]` base plus a struct field offset.
  - **Root cause:** `amy_render`/`render_osc_wave`/`hold_and_modify` walk `synth[]`/`msynth[]` holding **no lock**, while the delta path (`play_delta` → `ensure_osc_allocd` → `free_osc()`+`alloc_osc()`, plus reset/patch-load deltas) takes `amy_queue_lock` and `free()`s/reallocates those same structs. In this build (`multicore=0`, `multithread=0`) deltas are drained on Core 1 right before render, so the racing actor is **Core 0**: the sequencer `esp_timer` tick and UI calling `add_delta_to_queue()` and other osc alloc/free paths. The `synth[osc] != NULL` check in `amy_render` (line 1561) is a TOCTOU — `synth[osc]` can be freed between the check and the deref inside `hold_and_modify`.
  - **Fix (option 1):** Wrap the entire `amy_render` body in `amy_grab_lock()` / `amy_release_lock()` (the existing `amy_queue_lock` semaphore), so structural mutations cannot run mid-render. Verified deadlock-safe: no code reachable from the render path calls `add_delta_to_queue()` or `amy_grab_lock()` (would deadlock the non-recursive mutex), and with `multicore=0` there is no cross-core notify-while-holding-lock path. (If multicore is ever enabled, each `amy_render` invocation takes/releases the lock around its own work; the inter-core notify/wait happens in the caller, outside the locked region.)
  - **Risk:** Low–moderate. The lock is now held for a full render block (~hundreds of µs). The Core 0 sequencer enqueue (`add_delta_to_queue`) briefly blocks on it, but enqueue is a short list insert. No new allocation in the hot path.
  - **Rollback:** Remove the `amy_grab_lock()` after `AMY_PROFILE_START(AMY_RENDER)` and the matching `amy_release_lock()` before `AMY_PROFILE_STOP(AMY_RENDER)` in `amy_render`.

- **Bug fix (external AMY source change): missing `chained_osc` NULL guard in `render_osc_wave`, `components/amy/src/amy.c`**
  - **Root cause:** Upstream AMY dereferences `synth[chained_osc]->status` (the chained-osc recursion in `render_osc_wave`, ~line 1521) with **no NULL check**. A `chained_osc` can reference a slot that was freed or never allocated during a patch toggle, faulting the same way as above. This is a strict upstream bug, independent of the locking fix.
  - **Fix:** Added `synth[chained_osc] != NULL &&` to the existing `synth[chained_osc]->status == SYNTH_AUDIBLE` condition. Marked inline as a LOCAL EDIT.
  - **Risk:** Negligible. Adds one pointer comparison; when the chained slot is NULL the chained osc is simply skipped (correct — it cannot be audible).
  - **Rollback:** Remove the `synth[chained_osc] != NULL &&` clause.

## 2026-03-21

- Added ESP-IDF 6.0 compatibility comments and updated FreeRTOS task entry points in `components/amy/src/amy_midi.c` and `components/amy/src/i2s.c` so they use the required `void *` task signature.
- This is an external dependency compatibility edit, not a first-party AMY refactor.
- Excluded `components/amy/src/usb.c` from the ESP-IDF component build because this project provides its own USB implementation and does not need the MicroPython/Arduino USB path.

## 2026-03-31

- **Bug fix:** `AMY_SAMPLE_RATE` on `ESP_PLATFORM` defaulted to `44100` (the `#else` branch in `amy.h` lines 57–65), but `CONFIG_UAC_SAMPLE_RATE=48000` in `sdkconfig` means the TinyUSB UAC descriptor advertises 48 kHz to the Windows host. This caused AMY to render at 44.1 kHz while the host consumed samples as if they were 48 kHz — audio played ~88 cents flat and ~8% slow.
  - Added `#elif defined ESP_PLATFORM` → `48000` between the `__EMSCRIPTEN__` and `#else` cases in `amy.h`.
  - **Motivation:** UAC sample rate and AMY render rate must match; the minimal fix is a single added clause in the platform SR block.
  - **Risk:** Low. 48 kHz on S3 is well within hardware capability. PCM samples are internally stored at 22050 Hz and resampled — no change needed there. No time-sensitive path is altered; block size (256) stays the same.
  - **Rollback:** Remove the `#elif defined ESP_PLATFORM` / `48000` clause.

- **Config fix (not an AMY patch):** `platform.multithread` and `platform.multicore` must be set to `0` in `main.c` when using `AMY_AUDIO_IS_NONE`. With the defaults (`1`/`1`), `amy_platform_init()` spawns FABT and captures `app_main`'s task handle as `amy_update_handle`, causing a permanent deadlock between FABT and our `amy_usb_render_task` — `render_blocks` and `seq_tick` stay at 0. No change to AMY source required; fixed in `main/main.c`. See `amy-issue-fabt-deadlock.md` for full analysis.

## 2026-04-02

- **Bug fix:** Guarded `esp_poll_midi()` in `components/amy/src/i2s.c` so the ESP-IDF update path only touches UART MIDI when `AMY_MIDI_IS_UART` is enabled.
  - **Root cause:** `amy_default_config()` sets `c.midi = AMY_MIDI_IS_NONE` for ESP32 builds, but `amy_update_tasks()` still called `esp_poll_midi()` whenever `platform.multithread == 0`. That reached `uart_read_bytes()` on UART1 without a driver installed and produced `uart driver error` logs.
  - **Motivation:** Prevent spurious UART errors when this project runs AMY in USB-audio-only mode.
  - **Risk:** Low. The change is a narrow runtime guard around the existing MIDI poll path and does not affect builds that actually enable UART MIDI.
  - **Rollback:** Remove the `AMY_MIDI_IS_UART` condition and restore the unconditional poll.

## 2026-04-03

- **Bug fix (SMP crash): `sequences[]` race in `components/amy/src/sequencer.c`**
  - **Root cause:** On ESP32-S3 SMP, `sequencer_process_tick()` (called from AMY's `esp_timer` callback task) reads and walks `sequences[tag].deltas` without holding any lock, while `sequencer_add_event()` (called from `button_handler_task` via `amy_add_event()`) calls `delta_release_list(sequences[tag].deltas)` + rebuilds the chain, also without a lock. Concurrent execution on two cores causes a use-after-free: the timer task dereferences a delta node that the button task has already returned to the free pool and zeroed, producing `LoadProhibited` at `EXCVADDR=0x000002f0`.
  - `amy_queue_lock` was not usable here because `add_delta_to_queue()` is called *inside* `sequencer_process_tick()` and also grabs `amy_queue_lock` — wrapping the outer function would deadlock a non-recursive mutex.
  - **Fix:** Added `SEQ_LOCK` / `SEQ_UNLOCK` macros backed by a new `static SemaphoreHandle_t s_seq_lock` (ESP), `pthread_mutex_t` (POSIX), or no-op (bare-metal). Lock is created in `sequencer_init()` before `_sequencer_start()`. Held across the entire `sequences[tag]` mutation in `sequencer_add_event()` and across the full for-loop in `sequencer_process_tick()`. `add_delta_to_queue()` continues to independently grab `amy_queue_lock` (no nesting conflict).
  - **Risk:** Low. The mutex is a short critical section (one tag slot per `add_event` call, <1 ms per tick loop). No new allocation in hot path. Timer callback is at 500 µs cadence; mutex contention adds negligible latency.
  - **Rollback:** Remove the `SEQ_LOCK()`/`SEQ_UNLOCK()` calls and the lock variable block at the top of `sequencer.c`.

- **Bug fix (silent melodic layer): `max_sequencer_tags` too small — in `main/main.c`** *(not an AMY source patch)*
  - **Root cause:** `amy_default_config()` sets `max_sequencer_tags = 256`. Our tag formula assigns melodic layer (index 1) tags starting at `1 × (4×32×2) = 256`. `sequencer_add_event()` checks `tag > max_sequences` (strictly greater), so tag 256 slips through and writes to `sequences[256]` — one past the end of the 256-element array (UB/memory corruption). All tags >256 are silently dropped. Result: every melodic note event is either corrupted or discarded; no audio.
  - **Fix:** Added `amy_cfg.max_sequencer_tags = 1100` in `main.c` before `amy_start()`. Our highest tag is 1039 (preview slot for last layer/track), so 1100 gives safe headroom above the off-by-one in AMY's bound check.
  - **Rollback:** Remove the `amy_cfg.max_sequencer_tags = 1100` line (reverts to default 256).

## 2026-03-22

- **Bug fix:** `AMY_RENDER_TASK_PRIORITY` and `AMY_FILL_BUFFER_TASK_PRIORITY` in `components/amy/src/amy.h` changed from `ESP_TASK_PRIO_MAX` to `ESP_TASK_PRIO_MAX - 1`.
  - `ESP_TASK_PRIO_MAX` equals `configMAX_PRIORITIES` (25). FreeRTOS asserts `uxPriority < configMAX_PRIORITIES`, so passing 25 is unconditionally invalid and causes an immediate boot crash.
  - Filed upstream: see `amy-issue-task-priority.md`.
  - **Rollback:** revert the `- 1` subtraction in both defines.