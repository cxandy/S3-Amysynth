<img width="1135" height="121" alt="sadb0y" src="https://github.com/user-attachments/assets/a1bc0b42-7432-451b-908b-dc17cde8047c" />

## AMY i2s.c runtime note (USB/UAC mode)

This project currently runs AMY with `audio = AMY_AUDIO_IS_NONE` and uses a custom render task (`amy_update`) to feed USB UAC output.

### What changed in AMY

File changed: `components/amy/src/i2s.c`

In the ESP multithread path (`amy_render_audio`), we now re-bind `amy_update_handle` to the task that is actually calling `amy_update` at runtime.

Why this was needed:
- `amy_platform_init` originally captures `amy_update_handle` during `amy_start` (inside `app_main`).
- In this project, `amy_update` is called from a separate FreeRTOS task (`amy_usb_render_task`).
- Without re-binding, the AMY fill-buffer task notifies the wrong task handle, causing a deadlock in `amy_update`.
- Symptom was: sequencer ticks stuck at 0 and render blocks stuck at 0.

### Safety / impact when switching back to hardware I2S

This change is safe for I2S mode and should not require removal.

- In hardware I2S mode, AMY typically owns the audio pipeline and this re-bind is effectively a no-op unless task ownership changes.
- If you move back to AMY-managed I2S output, keep using normal AMY startup config and verify logs show stable audio task startup.
- If you later run `amy_update` from a different task again, this patch remains necessary and correct.

### Quick regression check

After any audio routing change (USB UAC vs I2S), confirm in monitor logs:
- render blocks increase over time
- sequencer tick count increases over time
- playhead advances on UI

# Bug: `uart_read_bytes()` errors when AMY runs with `AMY_MIDI_IS_NONE` but still polls MIDI on ESP-IDF

## Environment

| Field | Value |
|---|---|
| Framework | ESP-IDF 6.0 |
| Target | ESP32-S3 |
| AMY version | latest `main` (as of 2026-04-02) |
| Audio mode | `AMY_AUDIO_IS_NONE` with custom USB audio render loop |

## Symptom

The serial monitor shows repeated UART errors during startup and runtime:

```text
E (3105) uart: uart_read_bytes(1727): uart driver error
E (3129) uart: uart_read_bytes(1727): uart driver error
E (3140) uart: uart_read_bytes(1727): uart driver error
```

The error appears even though this project is not using UART MIDI. The noise can be mistaken for a log-level problem, but it is a real driver error.

## Root Cause

`amy_default_config()` sets:

```c
c.midi = AMY_MIDI_IS_NONE;
```

for ESP32 builds, but it also sets:

```c
c.midi_uart = 1;
```

In the ESP-IDF path, `amy_update_tasks()` in `components/amy/src/i2s.c` called `esp_poll_midi()` whenever `platform.multithread == 0`, regardless of whether UART MIDI was actually enabled.

That reaches `esp_poll_midi()` in `components/amy/src/amy_midi.c`, which does:

```c
int length = uart_read_bytes(uart_num, data, MAX_MIDI_BYTES_TO_PARSE, 1/portTICK_PERIOD_MS);
```

with `uart_num` resolved from `c.midi_uart`. Because UART MIDI was never initialised, the ESP-IDF UART driver reports `uart driver error`.

## Fix

Guard the poll in `components/amy/src/i2s.c` so it only runs when UART MIDI is enabled:

```c
if (amy_global.config.midi & AMY_MIDI_IS_UART) {
    esp_poll_midi();
}
```

This is the minimal fix because it preserves the existing MIDI path for builds that intentionally enable UART MIDI, while avoiding a bogus driver call in USB-audio-only mode.

## Why This Is Safe

- It does not change the AMY audio path.
- It does not affect builds that explicitly enable UART MIDI.
- It only removes an invalid poll when the UART MIDI feature is off.

## Rollback

Remove the `AMY_MIDI_IS_UART` guard and restore the unconditional call to `esp_poll_midi()`.

## Notes

The error was easy to misread because other startup logs were also printing at debug level. The underlying UART failure is independent of log verbosity.

# Bug: `AMY_RENDER_TASK_PRIORITY` crashes on ESP-IDF 6.0 — `ESP_TASK_PRIO_MAX` equals `configMAX_PRIORITIES`, which is an invalid FreeRTOS priority

## Environment

| Field | Value |
|---|---|
| Framework | ESP-IDF 6.0 (not Arduino) |
| Target | ESP32-S3 |
| AMY version | latest `main` (as of 2026-03-22) |

## Problem

AMY crashes immediately at boot with:

```
assert failed: prvInitialiseNewTask tasks.c:1111 (uxPriority < ( 25 ))

Backtrace:
--- 0x4200fe9e: amy_platform_init at components/amy/src/i2s.c:293
--- 0x4200fc31: amy_start at components/amy/src/api.c:361
```

The crash happens inside `amy_platform_init` → `xTaskCreatePinnedToCore` in `src/i2s.c:293`, before any audio initialisation occurs. The device boot-loops unconditionally.

## Root Cause

In `src/amy.h`, the non-Arduino ESP-IDF path defines:

```c
// src/amy.h (non-Arduino path)
#define AMY_RENDER_TASK_PRIORITY      (ESP_TASK_PRIO_MAX)
#define AMY_FILL_BUFFER_TASK_PRIORITY (ESP_TASK_PRIO_MAX)
```

`ESP_TASK_PRIO_MAX` is defined by ESP-IDF as:

```c
// esp_system/include/esp_task.h
#define ESP_TASK_PRIO_MAX (configMAX_PRIORITIES)   // = 25
```

FreeRTOS's `prvInitialiseNewTask` asserts **strict less-than**:

```c
// FreeRTOS-Kernel/tasks.c:1111
configASSERT( uxPriority < configMAX_PRIORITIES );
```

Passing `configMAX_PRIORITIES` (25) is therefore **always invalid** on any standard ESP-IDF build and crashes unconditionally. This is not a configuration issue — it is a hardcoded invalid value.

### The SDK documents the correct pattern

The `esp_task.h` header itself carries this comment immediately above the definition:

> *otherwise use `ESP_TASK_PRIO_MAX - X` style*

Every other ESP-IDF internal task follows this convention, e.g.:

```c
#define ESP_TASK_BT_CONTROLLER_PRIO   (ESP_TASK_PRIO_MAX - 2)
#define ESP_TASK_TIMER_PRIO           (ESP_TASK_PRIO_MAX - 3)
```

### AMY's own MIDI task already does this correctly

```c
// src/amy_midi.h
#define MIDI_TASK_PRIORITY (ESP_TASK_PRIO_MAX - 2)  // ✓ correct
```

This inconsistency between `amy.h` and `amy_midi.h` suggests the render/fill-buffer priorities were set without this constraint in mind.

## Proposed Fix

```c
// src/amy.h — non-Arduino ESP-IDF path
#define AMY_RENDER_TASK_PRIORITY      (ESP_TASK_PRIO_MAX - 1)
#define AMY_FILL_BUFFER_TASK_PRIORITY (ESP_TASK_PRIO_MAX - 1)
```

Priority 24 is the highest valid user-space priority on ESP-IDF. It still preempts all normal application tasks with no practical difference in real-time audio behaviour.

## Steps to Reproduce

1. Use AMY as a pure ESP-IDF component (not via Arduino)
2. Call `amy_start()` from `app_main`
3. Observe immediate boot crash — no audio output ever begins

## When Was This Introduced?

This is a **regression introduced in AMY commit `0aee666`** (2025-05-15), titled *"moving task names to amy.h for tulip"*.

Before that commit, `AMY_RENDER_TASK_PRIORITY` and `AMY_FILL_BUFFER_TASK_PRIORITY` did not exist in `amy.h` — priorities were defined elsewhere. That commit added them as `ESP_TASK_PRIO_MAX` without accounting for the strict less-than constraint.

The likely reason it wasn't caught: the change was made targeting the **Tulip CC board**, which runs a customised ESP-IDF build where `configMAX_PRIORITIES` is set to a higher value, making priority 25 valid in that environment. On a standard stock ESP-IDF project the default is `configMAX_PRIORITIES = 25`, so any value ≥ 25 is unconditionally invalid.

The ESP-IDF SDK itself has not changed — `ESP_TASK_PRIO_MAX = configMAX_PRIORITIES` has been defined that way for years. This is purely an AMY-side regression.

## Additional Notes

The Arduino path already hard-codes priority 20 as an apparent workaround:

```c
#ifdef ARDUINO
#define AMY_RENDER_TASK_PRIORITY      (20)
#define AMY_FILL_BUFFER_TASK_PRIORITY (20)
```

This suggests the issue has been encountered before on Arduino but the native IDF path was never corrected to match.

# Bug: Render task and audio permanently frozen when using `AMY_AUDIO_IS_NONE` with default `platform.multithread=1`

## Environment

| Field | Value |
|---|---|
| Framework | ESP-IDF 6.0 |
| Target | ESP32-S3 |
| AMY version | latest `main` (as of 2026-03-31) |
| Audio mode | `AMY_AUDIO_IS_NONE` (project drives audio manually via TinyUSB UAC) |

## Symptom

After a clean boot, the monitor shows `render_blocks=0` and `seq_tick=0` indefinitely.
No audio is produced, the sequencer playhead never advances, and all `amy_sysclock()`-derived timing is frozen:

```
I (1616) main: Main loop idle... seq_tick=0 tick_hook_calls=0 render_blocks=0 render_sysclock_ms=0
I (6626) main: Main loop idle... seq_tick=0 tick_hook_calls=0 render_blocks=0 render_sysclock_ms=0
I (11626) main: Main loop idle... seq_tick=0 tick_hook_calls=0 render_blocks=0 render_sysclock_ms=0
```

No assertion, no panic — the firmware boots successfully but produces no audio and no sequencer motion. Play/pause and BPM changes appear to work (callbacks fire, logs emit) but nothing plays.

## Root Cause

### The call graph

`amy_default_config()` returns a config with:
```c
c.platform.multicore   = 1;
c.platform.multithread = 1;
```

When `amy_start(cfg)` is called from `app_main`, it does:
```
amy_start()
  → global_init()
  → oscs_init()     // also calls sequencer_init → _sequencer_start (hardware timer OK)
  → amy_platform_init()   ← problem is here
```

Inside `amy_platform_init()` on ESP (`i2s.c`):
```c
void amy_platform_init() {
    amy_update_handle = xTaskGetCurrentTaskHandle();   // ← captures app_main's handle
    if (AMY_HAS_I2S) { esp32_setup_i2s(); }
    if (amy_global.config.platform.multicore) {
        xTaskCreatePinnedToCore(&esp_render_task, ...);
    }
    if (amy_global.config.platform.multithread) {
        xTaskCreatePinnedToCore(&esp_fill_audio_buffer_task, ...);  // ← spawns FABT
        if (!AMY_HAS_I2S) {
            xTaskNotifyGive(amy_fill_buffer_handle);   // ← kicks FABT once
        }
    }
}
```

`AMY_HAS_I2S` is false (audio mode is NONE), so FABT receives **one** initial notification and runs one block on startup. After rendering that block, at the bottom of `esp_fill_audio_buffer_task`:

```c
// at the end of every FABT iteration (non-I2S path):
xTaskNotifyGive(amy_update_handle);   // ← notifies app_main, NOT our render task
```

### Our render task path

The project's `amy_usb_render_task` calls `amy_update()`:

```c
// api.c
int16_t *amy_update() {
    amy_update_tasks();
    int16_t *block = amy_render_audio();   // ← blocks here
    ...
}
```

With `multithread=1`, `amy_render_audio()` on ESP does:
```c
int16_t *amy_render_audio() {
    if (amy_global.config.platform.multithread) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // ← waits for notification on THIS task's handle
        buf = last_audio_buffer;
        if (!AMY_HAS_I2S) {
            xTaskNotifyGive(amy_fill_buffer_handle);  // re-arm FABT for next block
        }
    }
    ...
}
```

### The deadlock

| Handle | Who holds it | Who notifies it |
|---|---|---|
| `amy_update_handle` | `app_main` (captured at `amy_platform_init` time) | FABT (every block) |
| `amy_usb_render_task` handle | our render task (waiting in `ulTaskNotifyTake`) | nobody |
| `amy_fill_buffer_handle` | FABT | `amy_render_audio()` ... which never returns |

- `amy_usb_render_task` blocks in `ulTaskNotifyTake` forever — nobody ever notifies it.
- FABT renders one block (the startup kick), notifies `app_main` (which ignores it), then waits for our render task to re-arm it via `xTaskNotifyGive(amy_fill_buffer_handle)` — which never happens.
- `total_blocks` stops at 1 (the single startup block). `amy_sysclock()` → 0 ms. Sequencer timer fires but `sequencer_check_and_fill()` always sees `next_amy_tick_us` in the past and stalls.

### Why it looked like step 0 only

The single startup FABT render advances `total_blocks` to 1 and `amy_sysclock()` to ~5 ms. The drum synth's first scheduled event (step 0, tick 1) may fire and produce sound once. After that, everything is frozen. From the user's perspective: sequencer appears stuck on the first step, plays audio on step 0 only (or on the first press of play), then no further sequencer motion.

### Why it regressed with the pot removal

Before, `amy_update_handle` may have been initialised earlier in the boot (if the pot reader task happened to be the one touching AMY), creating a timing lucky-path. After removing the pot reader task, `app_main` calls `amy_start()` directly, so `amy_update_handle` is permanently `app_main`'s handle. The deadlock became unconditional.

## Fix

Disable AMY's internal background tasks before calling `amy_start()`. In `main/main.c`:

```c
amy_config_t amy_cfg = amy_default_config();
amy_cfg.audio = AMY_AUDIO_IS_NONE;
amy_cfg.platform.multicore   = 0;   // ← add this
amy_cfg.platform.multithread = 0;   // ← add this
amy_cfg.amy_external_sequencer_hook = main_sequencer_tick_hook;
amy_start(amy_cfg);
```

With both flags cleared:
- `amy_platform_init()` creates neither FABT nor the secondary render task.
- `amy_render_audio()` takes the synchronous `else` branch: renders all oscillators in-task.
- `amy_update()` returns a valid block every call. `total_blocks` increments. `amy_sysclock()` advances.
- The sequencer hardware timer (500 µs) drives `sequencer_check_and_fill()` correctly.
- `amy_usb_render_task` pushes each block to `usb_audio_write_stereo()` as designed.

## Performance Impact

Without multicore AMY rendering, all oscillator rendering runs on whichever core `amy_usb_render_task` is pinned to (core 1). At 160 MHz and 48 kHz / 256-sample blocks, this leaves ~5.3 ms per block for synthesis. The project uses 4 sequencer tracks with a percussion synth — well within single-core budget. If the osc count is raised significantly in future, re-enabling `multicore=1` (but still keeping `multithread=0`) can split rendering across both cores without reintroducing the deadlock, as long as `amy_update_handle` is set to the render task's handle before calling `amy_start()`.

## Upstream Note

The `AMY_AUDIO_IS_NONE` + custom render loop pattern is valid and documented, but the interaction with the default `multithread=1` is a footgun: `amy_update_handle` is always the task that calls `amy_platform_init()`, and on ESP-IDF that is typically `app_main`, not the user's render task. A minimal upstream fix would be: when `AMY_AUDIO_IS_NONE && platform.multithread`, skip spawning FABT entirely, since there is no I2S sink to pace the render loop.
