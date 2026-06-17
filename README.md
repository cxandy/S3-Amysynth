# S3-Amysynth

A handheld synthesizer and step sequencer built on the ESP32-S3 with ESP-IDF.
Audio is generated on-device by the [AMY](https://github.com/shorepine/amy)
synth engine and streamed to a PC/DAW over USB Audio (with an I2S DAC path
wired for later standalone use), driven by an OLED + encoder UI for live editing.

## Prototype Video

https://github.com/user-attachments/assets/c1125515-0647-46e4-b6fc-ae1bccc93855

<video src="https://rt-rtos.github.io/assets/amybox%20(2).mp4" poster="assets/1.jpg" controls muted loop playsinline width="640">
  <a href="https://rt-rtos.github.io/assets/amybox%20(2).mp4"><img src="assets/1.jpg" alt="S3-Amysynth prototype demo" width="640"></a>
</video>

> Video not playing? [Watch the prototype demo](https://rt-rtos.github.io/assets/amybox%20(2).mp4)

## What it does

S3-Amysynth runs a multi-layer step sequencer that drives the AMY engine in
real time, plus two standalone instruments that sit alongside the grid:

- **Drum layer** — a 16/32-step grid across 4 tracks, live-editable while
  playing. Each track plays its own curated Juno patch.
- **Melodic layers** — added/removed at runtime, each with per-row synths so
  stacked notes don't collapse into one voice.
- **Arpeggiator** — a standalone arp on its own synth and its own screen: up to
  8 note slots, its own scale/root quantizer, direction, octaves, and a
  tempo-locked rate. See [ARP-ARCHITECTURE.md](components/synth_core/ARP-ARCHITECTURE.md).
- **Stutter drone** — a standalone tempo-synced drone: a chord played through a
  square-LFO amplitude gate ("stutter") and a sweeping filter, with a mono sub.
  See [DRONE.md](components/synth_core/custompatches/DRONE.md).
- **Envelope editor** — an on-OLED ADSR graph for shaping amplitude, shared by
  the melodic rows, the arp, and the drone.
- **Patch selection** — cycles AMY's built-in Juno/DX7/piano presets at runtime.
- **Scale quantizer** — snaps melodic notes to a selectable musical scale.
- **Global effects** — EQ, echo, chorus, and reverb levels from the main menu,
  applied across everything.

The arp and drone are reached through a menu overlay and stay isolated from the
sequencer grid, so each instrument keeps its own state and input.

Audio currently leaves the device over **USB Audio Class 2.0** (48 kHz stereo,
16-bit) via TinyUSB. The board also wires an I2S PCM5102 DAC for standalone
output, which is planned but not yet the active path.

## Hardware

- **MCU:** ESP32-S3-N16R8 (dual-core Xtensa LX7, 16 MB flash, 8 MB PSRAM)
- **DAC:** PCM5102 (I2S) — present, reserved for later standalone output
- **Display:** SSD1306 128×64 OLED (I2C, U8g2)
- **Input:** rotary encoder + push buttons, one ADC potentiometer
- **Audio out (active):** USB Audio Class 2.0 to host  |  **(inactive)** I2S DAC

## Software

- **SDK:** ESP-IDF 6.0, FreeRTOS (non-SMP dual-core)
- **Synthesis:** AMY engine (vendored, with a few documented local patches)
- **USB:** TinyUSB + Espressif UAC2 device
- **Graphics:** U8g2
- Application code in C, organized as ESP-IDF components

## Architecture notes

A few design decisions worth calling out:

- **Manual render loop.** AMY runs in `AMY_AUDIO_IS_NONE` mode; the project owns
  the render task, which calls `amy_update()` and pushes blocks to the USB ring
  buffer. This avoids AMY spawning its own audio tasks (which deadlocks in this
  configuration).
- **Core split.** The audio DSP task is pinned to core 1; USB, the sequencer
  tick, UI, and input live on core 0, so heavy synthesis doesn't starve the
  USB/timer path.
- **Per-row / per-instrument synths.** Each melodic row, the arp, and the drone
  own their own AMY synth slots. Because AMY routes note-on by `(synth, pitch)`,
  a shared synth would collapse two same-pitch notes into one voice; separate
  slots keep them independent.
- **Tempo-locked from the audio clock.** The arp schedule and the drone's stutter
  LFO + filter sweep derive their timing from AMY's sequencer tick counter (the
  same clock the grid rides). 
  Amy music clock is the master clock for all current and future audio-path logic for staying synced/beat-locked
  across tempo changes.
- **Deferred envelope authority.** A patch's own envelope plays by default; a
  custom envelope only overrides it once committed in the graph editor, so
  changing presets doesn't permanently shadow it.

The component layout:

| Component | Role |
| --- | --- |
| `main/` | app entry, task creation, AMY + USB init, input routing |
| `components/synth_core/` | sequencer core, OLED UI, quantizer, arp, drone, envelope editor |
| `components/display/` | display HAL + screen renderers + reusable graph-popup widget |
| `components/usb_audio/` | USB audio ring buffer / UAC glue |
| `components/rotary_encoder/`, `components/my_buttons/` | input drivers |
| `components/amy/` | vendored AMY engine (see [AMY-EDITS.md](AMY-EDITS.md) for local patches) |

## Documentation

- [SEQUENCER-ARCHITECTURE.md](SEQUENCER-ARCHITECTURE.md) — sequencer core, layers, tags, timing
- [ARP-ARCHITECTURE.md](components/synth_core/ARP-ARCHITECTURE.md) — the standalone arpeggiator
- [DRONE.md](components/synth_core/custompatches/DRONE.md) — the stutter drone (voice model, chords, tempo sync)
- [AMY-EDITS.md](AMY-EDITS.md) — local patches to the vendored AMY engine
- Per-component READMEs under `components/`

## Building

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

Flashing over USB from a Windows host uses USBIPD passthrough.

## Project context

This is a self-directed learning project for getting deeper into embedded audio
and real-time firmware. The interesting problems it has run into so far:

- coordinating timing-sensitive work (audio rendering, the sequencer tick, and
  UI refresh) across two cores without glitches
- making a constrained USB/audio pipeline behave (buffering, drop-vs-block
  trade-offs, sample-rate correctness)
- learning AMY's voice/patch/envelope model well enough to extend it — the arp
  and drone are built entirely on its public event API
- fitting a usable UI on a 128×64 display with one encoder and a few buttons
- keeping a clean line between project code and vendored dependencies, patching
  upstream only for confirmed bugs and tracking those edits in `AMY-EDITS.md`

Some parts are more finished than others; the scope is intentionally broad for
its stage.
