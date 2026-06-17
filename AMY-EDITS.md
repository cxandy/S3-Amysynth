# AMY Local Edits

Track local, project-specific changes made against the upstream AMY component here.

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