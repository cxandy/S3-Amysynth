# S3-Amysynth

A handheld synthesizer and step sequencer built on the ESP32-S3 with ESP-IDF.
Audio is generated on-device by the [AMY](https://github.com/shorepine/amy)
synth engine and streamed to a PC/DAW over USB Audio or directly via I2S DAC, with an OLED + encoder UI
for live editing.

## What it is

S3-Amysynth runs a multi-layer step sequencer that drives the AMY engine in
real time:

- **Drum layer** — a 16/32-step grid across 4 tracks, live-editable while playing.
- **Melodic layers** — added/removed at runtime, each with per-row synths so
  stacked notes don't collapse into one voice.
- **Envelope editor** — an on-OLED graph for shaping a row's amplitude envelope
  (attack / sustain level / release), with auto-derived decay.
- **Patch selection** — cycles AMY's built-in Juno/DX7/piano presets at runtime.
- **Scale quantizer** — snaps melodic notes to a selectable musical scale.

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
- **Per-row synths.** Each melodic row owns its own AMY synth slot. Because AMY
  routes note-on by `(synth, pitch)`, a shared synth would collapse two rows on
  the same pitch into one voice; separate synths keep them independent.
- **Deferred envelope authority.** A patch's own envelope plays by default; a
  row's custom envelope only overrides it once the user commits an edit in the
  graph editor, so changing presets doesn't permanently shadow them.

The component layout:

| Component | Role |
| --- | --- |
| `main/` | app entry, task creation, AMY + USB init |
| `components/sequencer_ui/` | sequencer core, OLED UI, quantizer, envelope editor |
| `components/priv_i2c_u8g2/` | display HAL + reusable graph-popup widget |
| `components/usb_audio/` | USB audio ring buffer / UAC glue |
| `components/rotary_encoder/`, `components/my_buttons/` | input drivers |
| `components/amy/` | vendored AMY engine (see `AMY-EDITS.md` for local patches) |

More detail lives in `SEQUENCER-ARCHITECTURE.md` and the per-component READMEs.

## Building

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

Flashing over USB from a Windows host uses USBIPD passthrough.

## Project context

This is a self-directed project I'm using to get deeper into embedded audio and
real-time firmware. It deliberately pulls together several areas I want to
improve at once rather than staying in a single comfortable lane:

- coordinating timing-sensitive work (audio rendering, the sequencer tick, and
  UI refresh) across two cores without glitches
- making a constrained USB/audio pipeline behave (buffering, drop vs. block
  trade-offs, sample-rate correctness)
- integrating a real third-party synth engine instead of a toy oscillator, and
  learning its voice/patch/envelope model well enough to extend it
- building a usable UI on a 128×64 display with one encoder and a few buttons
- keeping a clean line between my own code and vendored dependencies — patching
  upstream only when a bug is confirmed, keeping those edits minimal and tracked
  in `AMY-EDITS.md`

It's a learning project, so some parts are more finished than others and the
scope is intentionally ambitious for its stage.

## Current focus

- improving audio-stream reliability under varying host conditions
- refining the melodic layer / envelope editing workflow
- moving toward standalone I2S output alongside USB

## Dependencies

AMY, U8g2, TinyUSB, and Espressif managed components (button, UAC2). External
libraries are treated as upstream sources; local patches to AMY are confirmed,
minimal, and documented in `AMY-EDITS.md`.
