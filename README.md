# S3-Amysynth

A handheld synthesizer and step sequencer built on the ESP32-S3 with ESP-IDF.
Audio is generated on-device by the [AMY](https://github.com/shorepine/amy)
synth engine and streamed to a PC/DAW over USB Audio or directly via I2S DAC, with an OLED + encoder UI
for live editing.


## Prototype Video

<video src="https://rt-rtos.github.io/assets/amybox%20(2).mp4" poster="assets/1.jpg" controls muted loop playsinline width="640">
  <a href="https://rt-rtos.github.io/assets/amybox%20(2).mp4"><img src="assets/1.jpg" alt="S3-Amysynth prototype demo" width="640"></a>
</video>

> Video not playing? [Watch the prototype demo](https://rt-rtos.github.io/assets/amybox%20(2).mp4)

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

## Overview

**S3-Amysynth** is an embedded audio project built on the **ESP32-S3** using **ESP-IDF**. It combines firmware development, real-time audio generation, hardware interfacing, and UI design in a custom handheld synthesizer platform.

The project is designed to demonstrate practical embedded engineering skills in a form that is easy to assess at a glance: what the system does, how it is built, and what technical areas it develops.



## What the Project Does

S3-Amysynth is a microcontroller-based synthesizer and drum sequencer with a hardware interface and live audio output.

### Core functionality

- Real-time audio generation using the **AMY synth engine**
- **16-step drum sequencer** with editable playback
- **Dynamic Melodic Layers** Additional Melodic layers can be added and removed
- **OLED user interface** for sequencer state and controls
- **Rotary encoder** and **buttons** for navigation and tempo/input control
- **USB Audio Class 2.0** audio streaming to a PC or DAW during development
- **I2S DAC/Amp** for on device audio planed in later stages
- Support for a custom ESP32-S3 hardware platform with dedicated audio and display peripherals



## Technical Stack

### Hardware

- **ESP32-S3-N16R8**
- **PCM5102 I2S DAC**
- **SSD1306 128×64 OLED**
- Rotary encoder
- Push buttons

### Software

- **ESP-IDF 6.0**
- **FreeRTOS**
- **TinyUSB**
- **AMY synth engine**
- **U8g2** graphics library
- Embedded **C**



## Skills Demonstrated

This project reflects development in the following areas:

### Embedded Firmware Development

- Writing structured firmware for a microcontroller platform
- Initializing and coordinating multiple peripherals
- Working within hardware and memory constraints

### Real-Time Systems

- Managing timing-sensitive behavior for audio and sequencing
- Coordinating tasks across a dual-core embedded platform
- Handling responsiveness requirements for user input and playback

### Audio Systems

- Implementing digital audio behavior on constrained hardware
- Working with sample-rate-sensitive output paths
- Understanding the relationship between synthesis, buffering, and playback timing

### Hardware-Software Integration

- Interfacing with display, buttons, encoder, ADC input, and audio hardware
- Debugging issues that cross between firmware behavior and physical hardware
- Building software around a custom board design rather than a generic dev-board workflow

### Debugging and Engineering Workflow

- Diagnosing timing, configuration, and integration issues
- Applying targeted fixes to system-level problems
- Documenting technical decisions and implementation details



## Knowledge Level Indicated

This project brings together several overlapping areas of interest, including embedded firmware, real-time systems, digital audio, hardware interfacing, and UX design. As a self-directed project outside formal coursework, it reflects strong motivation, a willingness to take on difficult technical challenges, and a passion for the field. Relative to roughly six months of embedded development experience, the scope is purposefully ambitious and challenging. 
This project hopefully indicates progress beyond beginner exercises toward more advanced systems-level work.

### Strongest indicators

- Comfortable working in an **ESP-IDF** environment
- Practical understanding of **microcontroller peripherals**
- Exposure to **real-time audio and task-based firmware design**
- Ability to integrate multiple subsystems into a functioning embedded application
- Growing familiarity with debugging issues involving timing, concurrency, and external interfaces

### What this suggests

The project goes beyond introductory firmware exercises and shows progression toward more capable work in:
- embedded systems
- firmware engineering
- real-time software
- audio-oriented embedded development



## Engineering Challenges Addressed

The project includes work in areas that commonly appear in real embedded development:

- Real-time sequencing and playback timing
- USB audio output integration
- Embedded UI design on a small display
- Input handling with shared control modes
- Audio pipeline configuration and synchronization
- Multi-component firmware organization

These are useful indicators of hands-on engineering ability because they require more than isolated feature implementation.

## Open-Source Dependency Practice

This project depends on several open-source components, including **AMY**, **U8g2**, **TinyUSB**, and ESP-IDF-managed libraries. Development is approached with a clear separation between **project-owned code** and **external dependencies**.

### Approach

- Project behavior is implemented in local application and component code first
- External libraries are treated as stable upstream sources, not as the default place to make changes
- Upstream code is only edited when incorrect behavior is clearly confirmed and a local-layer fix is not sufficient
- Any external patch is kept minimal, documented, and tracked separately from project code
- Internal and external edits are recorded distinctly to make maintenance, review, and rollback easier
- Confirmed upstream issues are intended to be reported back to the original source where appropriate



## Why This Project Matters

S3-Amysynth is a good example of systems-level development because it combines:

- low-level firmware work
- hardware interaction
- audio processing concepts
- user interface logic
- debugging across multiple subsystems

It communicates both technical curiosity and the ability to carry a project across design, implementation, testing, and refinement.



## Current Focus

The project is currently focused on:

- improving embedded audio reliability
- refining sequencer behavior and controls
- strengthening hardware-software integration
- building a clearer and more maintainable firmware structure



## Summary

S3-Amysynth is a custom **ESP32-S3** embedded audio system built with **ESP-IDF 6.0**, **FreeRTOS**, and the **AMY** synthesis engine. It combines real-time sound generation, a live-editable **16-step drum sequencer**, an **SSD1306 OLED** interface, and hardware input handling through a **rotary encoder** and push buttons on a custom handheld platform.

The current firmware renders audio on the microcontroller and streams it over **USB Audio Class 2.0** using **TinyUSB** at **48 kHz stereo 16-bit**, while the hardware design also includes an **I2S PCM5102 DAC** path for later standalone output. The system brings together multiple embedded concerns in one project: task coordination across a dual-core MCU, timing-sensitive sequencing, audio buffering, peripheral integration, and UI feedback on a constrained device.

Overall, the project is a practical example of systems-level embedded development that spans:
- firmware architecture
- real-time audio processing
- hardware-software integration
- user input and display management
- debugging across project-owned code and upstream dependencies
