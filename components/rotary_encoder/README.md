# Rotary Encoder Component

This helper configures an ESP32-S3 pulse counter (PCNT) unit and exposes a
reusable API for handling quadrature rotary encoders.

## Features
- Dual-channel PCNT setup (edge + level actions) for X4 decoding.
- Optional glitch filter and user-configurable high/low count limits.
- Thin wrapper that manages unit/channel creation and cleanup.

## Primary API
| Function | Description |
| --- | --- |
| `rotary_encoder_default_config(pin_a, pin_b)` | Build a `rotary_encoder_config_t` with sane defaults for the provided phase pins. |
| `rotary_encoder_new_with_config(config, &handle)` | Create/start an encoder using the supplied config. Limits and the glitch filter can be tuned here. |
| `rotary_encoder_new(pin_a, pin_b, &handle)` | Convenience helper that simply calls `rotary_encoder_default_config()` and creates the encoder. |
| `rotary_encoder_get_count(handle)` | Reads the signed counter value. |
| `rotary_encoder_reset(handle)` | Clears the PCNT unit count. |
| `rotary_encoder_delete(handle)` | Stops the PCNT unit and frees the handle. |

The API is poll-based: read `rotary_encoder_get_count()` periodically and act
on the delta. (The firmware polls at 50 Hz from `encoder_task` and accumulates
two raw X4 ticks per UI action, matching one physical detent.)

Placing the directory under `components/` adds it to the CMake graph
automatically. Callers only need to include `rotary_encoder.h` and link
against ESP-IDF's standard drivers component.
