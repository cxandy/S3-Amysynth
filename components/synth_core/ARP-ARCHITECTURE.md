# Standalone Arpeggiator Architecture

> Build target: ESP32-S3-WROOM-1 (N16R8), ESP-IDF 6.0.
> Companion to `SEQUENCER-ARCHITECTURE.md`. Read that first — the arp reuses the
> sequencer's AMY event plumbing, mutex, and OLED task.

---

## Overview

The arpeggiator is a **second top-level screen** that runs **in parallel with**
the step sequencer, not inside it. It owns its own note inputs, its own
scale/root quantizer, its own dedicated AMY synth slot, and its own block of AMY
sequencer tags. It locks to the same global tempo as the sequencer by scheduling
**repeating AMY SEQUENCE events**, exactly like the sequencer's steps.
## Architecture

```mermaid
flowchart TD
    GPIO1["GPIO1 Menu btn"] --> CB["main_button_event_cb"]
    ENC["Encoder task"] --> RT["Input router by ui_mode and overlay"]
    CB --> RT
    RT -->|menu open| MENU["menu overlay handlers"]
    RT -->|envelope editor open| GRAPH["envelope popup"]
    RT -->|ui_mode ARP| ARPUI["arp ui handlers"]
    RT -->|ui_mode SEQUENCER| SEQUI["sequencer handlers"]
    MENU -->|set scale root enabled| QUANT["sequencer_core quantizer"]
    MENU -->|set ui_mode and arp on| STATE["seq_state ui_mode and arp"]
    ARPUI --> ARP["arp_core 8 chromatic slots"]
    ARP -->|sort then snap at playback| EMIT["repeating AMY SEQUENCE events"]
    EMIT --> AMY["AMY synth slot for arp"]
    SEQUI --> SEQCORE["sequencer_core emit_step"]
    SEQCORE --> AMY
    STATE --> RENDER["synth_ui_task 20Hz"]
    RENDER -->|menu| DRAWM["draw menu"]
    RENDER -->|arp| DRAWA["display_arp_draw_frame"]
    RENDER -->|seq| DRAWS["display_seq_draw_frame"]
```
```
encoder / buttons ─▶ synth_ui.c ─▶ arp_core.c ─▶ sequencer_core.c ─▶ AMY engine
   (main.c)          (arp screen +      (arp model +   (shared event
                      dirty/service)     scheduling)     buffer + mutex)
                           │
                     display_arp.c ─u8g2─▶ SSD1306 OLED
```

| Layer | File | Responsibility |
|---|---|---|
| Arp model | `components/synth_core/arp_core.c` | Owns arp state (enabled, dir, octaves, rate, gate, scale, root, patch, 8 note slots). Computes the note sequence and (re)emits it. |
| AMY bridge | `components/synth_core/sequencer_core.c` | `sequencer_core_arp_*` helpers: configure the arp synth, emit/clear arp tags through the shared event buffer + mutex. Owns the arp tag window. |
| UI / input | `components/synth_core/synth_ui.c` | Arp screen cursor/edit state, builds the flat `arp_view_t`, dispatches encoder/button to `arp_*` setters, coalesces refreshes (`arp_core_service`). |
| Display | `components/display/display_arp.c` | Pure render of `arp_view_t` → U8g2. No arp logic. |
| View type | `components/display/display_arp.h` | `arp_view_t` flat struct + cursor index defines. |

**Design rule:** the arp is independent of the sequencer's layers. It never
reads or writes `s_layers[]`. The only thing it shares is the global tempo and
the single AMY event buffer/mutex in `sequencer_core.c`.

---

## Data Model

### `arp_state_t`  (`arp_core.c`, file-static `s_arp`)

```c
typedef struct {
    bool       enabled;
    arp_dir_t  dir;                   // ARP_UP | ARP_DOWN
    uint8_t    octaves;               // 1..ARP_OCT_MAX (4)
    arp_rate_t rate;                  // 1/4, 1/8, 1/16, 1/32
    uint8_t    gate_pct;              // 10..100
    int16_t    slots[ARP_MAX_SLOTS];  // raw chromatic MIDI, -1 = empty sentinel
    uint8_t    scale_index;           // arp's OWN quantizer scale
    uint8_t    root_note;             // arp's OWN quantizer root
    uint16_t   patch;                 // arp's OWN AMY patch
} arp_state_t;
```

**Key decision — slots store RAW chromatic notes, not snapped notes.** The arp
snaps to its own scale/root only at *playback* (and for display via
`arp_get_slot_snapped`). The stored value never absorbs its own quantized
output, so repeated edits don't compound drift. `slots[]` is null-terminated:
the first `-1` ends the active set.

### `arp_view_t`  (`display_arp.h`)

Flat, render-only snapshot built fresh each frame by `arp_build_view()`. The
renderer is pure — it never calls back into `arp_core`.

```c
typedef struct {
    bool        enabled;
    const char *mode_str;                  // "UP" / "DOWN"
    uint8_t     octaves;
    const char *rate_str;                  // "1/16" etc
    uint8_t     gate_pct;
    char        slot_name[ARP_VIEW_SLOTS][4];   // snapped note name or ""
    bool        slot_active[ARP_VIEW_SLOTS];
    uint8_t     cursor;                    // 0..ARP_CUR_COUNT-1
    bool        editing;                   // value being adjusted
    uint16_t    patch;                     // always shown (number)
    bool        patch_select;              // true while patch hold+turn active
    const char *patch_name;                // banner name, or NULL (table excluded)
} arp_view_t;
```

### Compile-time limits

| Define | Value | File | Meaning |
|---|---|---|---|
| `ARP_MAX_SLOTS` | 8 | `arp_core.h` | Note input slots |
| `ARP_OCT_MAX` | 4 | `arp_core.h` | Max octave span |
| `ARP_MAX_STEPS` | `ARP_MAX_SLOTS × ARP_OCT_MAX` = 32 | `arp_core.c` | Max distinct scheduled arp notes |
| `ARP_VIEW_SLOTS` | 8 | `display_arp.h` | Slots drawn on the OLED |
| `ARP_RATE_COUNT` | 4 | `arp_core.h` | Rate subdivisions |

---

## AMY Scheduling

The arp builds a repeating pattern out of AMY SEQUENCE events, one note-on +
note-off pair per arp step, all sharing one **period** (the full arp cycle
length). AMY replays them every period forever until cleared.

### Tag window

The arp's tags sit **immediately above** the sequencer's tag space so the two
never collide. Defined in `sequencer_core.c`:

```
sequencer step on/off : 0 .. MAX_LAYERS*SEQ_TRACKS*SEQ_MAX_STEPS*2 - 1   (0..1023)
sequencer previews    : 1024 .. 1024 + MAX_LAYERS*SEQ_TRACKS*2 - 1       (..1055)
─────────────────────────────────────────────────────────────────────────────────
SEQ_ARP_TAG_BASE  = 1056
SEQ_ARP_TAG_COUNT = ARP_MAX_SLOTS * ARP_OCT_MAX * 2 = 64
SEQ_ARP_TAG_MAX   = 1056 + 64 - 1 = 1119
```

Each arp step `i` uses tag `SEQ_ARP_TAG_BASE + i*2` (note-on) and `+1`
(note-off). With `ARP_MAX_STEPS = 32`, that's tags 1056..1119.

> **AMY off-by-one — critical.** AMY's `sequencer_add_event()` guards with
> `tag > max_sequences` (NOT `>=`), so a write at index `max_sequences` is
> allowed and overruns `sequences[]`. To stay clear, `main.c` sets
> `amy_cfg.max_sequencer_tags = 1200` (well above 1119). `sequencer_core_arp_emit_note()`
> also defensively drops any tag `> SEQ_ARP_TAG_MAX`. Keep `max_sequencer_tags`,
> `SEQ_ARP_TAG_BASE/COUNT`, and the sequencer tag formula in sync.

### Rate → ticks

`AMY_SEQUENCER_PPQ = 48`, so a 1/16 note = 12 ticks (matches the sequencer's
`SEQ_TICKS_PER_STEP`).

| Rate | Ticks/note |
|---|---|
| `ARP_RATE_1_4` | 48 |
| `ARP_RATE_1_8` | 24 |
| `ARP_RATE_1_16` | 12 |
| `ARP_RATE_1_32` | 6 |

### Sequence computation (`arp_core_refresh`)

1. Collect non-empty slots into a sorted ascending set (`arp_collect_sorted`,
   bubble sort, n ≤ 8).
2. `steps = count × octaves`; `period = steps × rate_ticks`;
   `gate = rate_ticks × gate_pct / 100` (min 1).
3. For each step `i`:
   - `note_idx = i % count`, `octave = i / count`.
   - `pick = sorted[note_idx]` (UP) or `sorted[count-1-note_idx]` (DOWN).
   - `chromatic = pick + octave*12`, then `arp_snap()` → clamp to melodic range.
   - `tick_on = 1 + i*rate_ticks`; emit on/off pair at `tag_base + i*2`.
4. Any previously-used arp tag not rewritten this pass was already cleared by
   `arp_clear_all()` at the top of `refresh`.

`tick 0` is reserved for "clear", so on/off ticks are forced ≥ 1.

---

## AMY Synth Slot

| Consumer | Synth ID | Default patch | Voices |
|---|---|---|---|
| Drum layer | 10 | 1025 | 16 |
| Melodic layers | 11..62 | `CONFIG_SEQ_MELODIC_PATCH` (138) | 1/row |
| **Arp** | **63** | `CONFIG_SEQ_ARP_DEFAULT_PATCH` (138) | 4 |

The arp owns slot **63**, reserved above the melodic ceiling (62) so it never
collides with a melodic layer's per-row block. AMY's default `max_synths = 64`
gives valid indices 0..63, so 63 is the last usable slot. The arp synth uses 4
voices to allow note overlap at fast rates.

---

## Refresh Coalescing (performance)

Every arp setter changes scheduling, and a naive setter calls `arp_core_refresh()`
inline, which clears + re-emits up to 32 events — each through the shared AMY
event mutex on Core 0. A fast encoder spin through octaves/rate/gate would fire
one full re-emit per detent, flooding the mutex.

**Fix:** setters call `arp_mark_dirty()` (store value, set a flag) instead of
re-emitting. `arp_core_service()` performs at most one re-emit per call and is
invoked once per UI frame (20 Hz) from `synth_ui_task`. Fast edits collapse
into a single re-emit per 50 ms.

```c
// setter (arp_core.c)
void arp_set_octaves(uint8_t o) { ...; s_arp.octaves = o; arp_mark_dirty(); }

// once per frame (synth_ui_task)
arp_core_service();   // if dirty: arp_core_refresh(); else cheap no-op
```

`arp_set_patch()` is the exception — it only reconfigures the synth (no
re-schedule), so it does **not** mark dirty. `arp_core_init()` marks dirty once
so a boot-enabled arp emits on its first service tick.

> Companion optimization in `components/amy/src/sequencer.c`: the per-tick scan
> now walks an **active-tag index** (only tags with live events) instead of
> `0..highest_tag`. The arp used to pin `highest_tag` at 1119 permanently,
> making every 500 µs tick scan ~1120 mostly-empty slots on Core 0. See
> `SEQUENCER-ARCHITECTURE.md` / project memory for that change.

---

## Public API

### `arp_core.h`

```c
/* Lifecycle */
void arp_core_init(void);
void arp_core_refresh(void);   // force immediate full re-emit
void arp_core_service(void);   // coalesced re-emit if dirty (call per frame)

/* Setters — each marks the arp dirty (except set_patch) */
void arp_set_enabled(bool);
void arp_set_direction(arp_dir_t);
void arp_set_octaves(uint8_t);     // 1..ARP_OCT_MAX
void arp_set_rate(arp_rate_t);
void arp_set_gate_pct(uint8_t);    // 10..100
void arp_set_scale(uint8_t);
void arp_set_root_note(uint8_t);
void arp_set_patch(uint16_t);      // 0..256; reconfigures synth only
void arp_set_slot(uint8_t idx, int16_t chromatic_note);  // -1 clears

/* Getters (for UI display) */
bool       arp_get_enabled(void);
arp_dir_t  arp_get_direction(void);
uint8_t    arp_get_octaves(void);
arp_rate_t arp_get_rate(void);
const char *arp_rate_name(arp_rate_t);
uint8_t    arp_get_gate_pct(void);
uint8_t    arp_get_scale(void);
uint8_t    arp_get_root_note(void);
uint16_t   arp_get_patch(void);
int16_t    arp_get_slot(uint8_t idx);          // raw chromatic, -1 = empty
int16_t    arp_get_slot_snapped(uint8_t idx);  // pitch actually played
uint8_t    arp_active_slot_count(void);
```

### `sequencer_core.h` (AMY bridge)

```c
uint8_t  sequencer_core_arp_synth(void);   // 63
uint8_t  sequencer_core_arp_voices(void);  // 4
uint32_t sequencer_core_arp_tag_base(void);// 1056
uint8_t  sequencer_core_clamp_melodic_note(int32_t);

void sequencer_core_arp_configure(uint16_t patch_number, uint8_t num_voices);
void sequencer_core_arp_emit_note(uint32_t tag_base, uint8_t midi_note,
                                  float velocity, uint32_t tick_on,
                                  uint32_t gate_ticks, uint32_t period);
void sequencer_core_arp_clear_note(uint32_t tag_base);  // clears base & base+1
```

### `synth_ui.h` (arp screen)

```c
bool synth_ui_arp_is_active(void);     // ui_mode==ARP && !menu && !graph
void synth_ui_arp_handle_encoder(long delta);
void synth_ui_arp_handle_button(void); // toggle field edit
void synth_ui_arp_cycle_patch(int delta);
```

---

## Screen, Input, and Isolation

### Screen selection

`ui_mode` (`UI_MODE_SEQUENCER` | `UI_MODE_ARP`) is switched from the **menu
overlay** ("Screen: Seq" / "Screen: Arp" action items). The render task
precedence in `synth_ui_task` is:

```
graph editor  >  menu overlay  >  arp screen  >  sequencer
```

`synth_ui_arp_is_active()` is true only when `ui_mode == UI_MODE_ARP` AND
neither the menu nor the graph editor is up.

### Arp screen input

| Control | Action |
|---|---|
| Encoder turn (not editing) | Move cursor across fields then 8 slots (`ARP_CUR_*`) |
| Encoder turn (editing) | Adjust the focused field / slot value |
| `MY_BUTTON_ENC` press | Toggle edit on the focused field (`arp_handle_button`) |
| `MY_BUTTON_1` hold + turn | Cycle the arp's OWN patch (`arp_cycle_patch`) |
| `MY_BUTTON_3` single-click | Toggle menu overlay |
| `MY_BUTTON_0` long-press | Global play/pause (shared with sequencer) |

Cursor index space (`display_arp.h`): `0=ENABLE, 1=MODE, 2=OCT, 3=RATE,
4=GATE, 5..12 = slots 0..7`.

### Seq/Arp isolation (`main.c`)

While the arp screen is active, all **sequencer-editing** gestures are blocked
so hidden sequencer state can't be mutated behind the arp view:

- `MY_BUTTON_2` drum-select is suppressed and its latch force-cleared (prevents
  a stuck "held" state if the screen switched mid-hold).
- `MY_BUTTON_0` single-click (layer cycle) is suppressed; long-press play/pause
  stays live.
- Step-toggle / track-note-nudge never reach the sequencer because the encoder
  path routes to `arp_handle_encoder` when `arp_is_active()`.

The arp's own controls and the `MY_BUTTON_1` arp-patch gesture remain live.

---

## OLED Display Layout (`display_arp.c`)

```
[0,0]──────────────────────────────────[127,0]
ARP:ON    UP            OCT:2            y=8   (macro row 1)
RATE:1/16     GATE:75%          P138     y=20  (macro row 2, patch right-aligned)
────────────────────────────────────────  y=26
[C3]  E3    G3    B3                      y=40  (slot row 0)
 --   --    --    --                      y=54  (slot row 1)
```

- **Macro row 1:** ARP enable, direction, octave span.
- **Macro row 2:** rate, gate %, and the **patch number** ("P138") right-aligned
  to x≈126. GATE nudged to x=56 to clear it. The patch number is framed while
  the patch hold gesture is active.
- **Patch name banner:** while patch-select is held, the patch's human name is
  drawn in a centered, cleared+framed box over the slot grid (y=34) — mirroring
  the sequencer view. Only when `CONFIG_SEQ_PATCH_SHOW_NAMES` compiles the name
  table in; otherwise `patch_name` is NULL and the block costs nothing.
- **Slots:** two rows of 4 cells; snapped note name or "--" for empty. Selected
  cell framed; double-framed while editing.

The frame is only redrawn when a 32-bit FNV-1a signature of `arp_view_t`
(`arp_view_signature`) changes — including `patch` and `patch_select` so patch
edits and hold-banner toggles trigger a redraw.

---

## Boot Defaults (`Kconfig`, `arp_core_init`)

| Field | Kconfig | Default |
|---|---|---|
| enabled | `SEQ_ARP_DEFAULT_ENABLED` | n (off) |
| scale | `SEQ_ARP_DEFAULT_SCALE` | 1 (Major) |
| root | `SEQ_ARP_DEFAULT_ROOT_NOTE` | 60 (C4) |
| gate % | `SEQ_ARP_DEFAULT_GATE_PCT` | 75 |
| octaves | `SEQ_ARP_DEFAULT_OCTAVES` | 1 |
| patch | `SEQ_ARP_DEFAULT_PATCH` | 138 (DX7 E.Piano 1) |

`arp_core_init()` is called from `synth_ui_init()`, after
`sequencer_core_init()`. All slots start empty (`-1`); the arp produces no sound
until enabled and at least one slot is filled.

---

## Future Development Considerations

- **More slots / octaves.** Raising `ARP_MAX_SLOTS` or `ARP_OCT_MAX` grows
  `ARP_MAX_STEPS` and therefore `SEQ_ARP_TAG_COUNT`. You must also raise
  `amy_cfg.max_sequencer_tags` so `SEQ_ARP_TAG_MAX + 2 ≤ max_sequencer_tags`
  (AMY off-by-one). Re-check the tag window comment in `sequencer_core.c`.
- **More arp directions** (up/down, random, as-played). Add to `arp_dir_t` and
  extend the pick logic in `arp_core_refresh`; no scheduling/tag changes needed.
- **Per-slot velocity / accents.** `sequencer_core_arp_emit_note` already takes a
  velocity; add a `slot_vel[]` and plumb it through `arp_core_refresh`.
- **Tempo-change refresh.** Period is in ticks, so a BPM change retimes the arp
  automatically (AMY scales ticks). No re-emit needed unless `rate`/`octaves`/
  `slots` change. If a future feature needs a re-time, call `arp_core_refresh()`.
- **Persistence (NVS).** `arp_state_t` is a flat, pointer-free struct — directly
  serialisable with `nvs_set_blob`. Save under one key; re-emit via
  `arp_core_refresh()` after restore.
