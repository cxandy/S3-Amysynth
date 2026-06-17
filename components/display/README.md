# display — display layer

The project's **display layer**: the ESP-IDF↔U8g2 hardware glue plus every
on-screen renderer. It started as a drop-in I2C/U8g2 glue component and grew
into the home for all the OLED rendering as the UI expanded.

> Naming note: the renderer files use the `display_*` prefix. The reusable HAL
> file keeps the `priv_i2c_u8g2.{c,h}` filename and its `i2c_u8g2_*` symbols, so
> existing `CONFIG_I2C_U8G2_*` menuconfig options are unchanged.

## What it provides

**Display HAL** (the original, still-reusable part)

- ESP-IDF I2C master bus/device setup, U8g2 byte + gpio/delay callbacks
- Display init + power control; hands the app a `u8g2_t*` to draw with
- Configurable via menuconfig (`Kconfig`); default panel is SSD1306 128×64

**Renderers** (project-specific; the UI logic lives in `synth_core`, this layer
only draws the flat view structs it is handed)

- Sequencer grid, the arp screen, the drone parameter list, and the menu overlay
- A reusable ADSR/curve **graph-popup** widget used by the envelope editor
- Patch-name tables for the on-screen patch browser

## Files

| File | Role |
| --- | --- |
| `priv_i2c_u8g2.{c,h}` | I2C + U8g2 HAL: bus/device setup, init, power, `u8g2_t*` accessor (`i2c_u8g2_*` symbols) |
| `display_seq.{c,h}` | sequencer grid renderer + the shared UI state struct (`display_seq_state_t`, `seq_*` types) |
| `display_arp.{c,h}` | arp screen renderer + its flat view struct |
| `display_drone.{c,h}` | drone parameter-list renderer + its flat view struct |
| `display_menu.{c,h}` | menu overlay renderer (scrollable label:value list) |
| `graph_popup.{c,h}` | reusable graph/curve editor widget (ADSR) |
| `patch_names.{c,h}` | AMY patch number → name tables for the browser |
| `Kconfig` | display menuconfig options |

## Design

The renderers are intentionally **dumb**: each takes a flat "view" struct
(plain values + cursor/edit flags) and draws it. The state those views describe,
and all the input handling, live in `synth_core` (sequencer core, arp, drone,
menu). This keeps the display layer decoupled from synth/sequencer logic — the
same reason the menu and arp/drone screens each have a small `*_view_t` instead
of reaching into engine state directly.

The shared sequencer UI state struct (`display_seq_state_t` in
`display_seq.h`) is the one exception: it is large enough that it doubles as
the canonical UI state, included by both this layer and `synth_core`.

## HAL quick usage

```c
#include "priv_i2c_u8g2.h"

i2c_u8g2_handle_t display;
i2c_u8g2_config_t cfg = i2c_u8g2_config_default();
ESP_ERROR_CHECK(i2c_u8g2_init(&display, &cfg));

u8g2_t *u8g2 = i2c_u8g2_get_u8g2(&display);
u8g2_ClearBuffer(u8g2);
u8g2_SetFont(u8g2, u8g2_font_ncenB08_tr);
u8g2_DrawStr(u8g2, 0, 12, "Hello");
u8g2_SendBuffer(u8g2);
```

## Notes

- Default panel setup is `u8g2_Setup_ssd1306_i2c_128x64_noname_f`; change
  `cfg.setup_fn` for a different controller.
- The OLED is full-buffer and `SendBuffer` is blocking I2C (~20 ms), so the UI
  task on core 0 renders on change only (signature-compared), not every frame.
- For multi-task access, guard U8g2 calls with your own mutex — the project does
  all drawing from the single `synth_ui` task.
