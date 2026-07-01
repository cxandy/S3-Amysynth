# C1 — On-screen button hint bar

Branch: `feat/c1-hint-bar`. Adds a persistent, one-line OLED status string
that shows what `MY_BUTTON_1`/`MY_BUTTON_2`/`MY_BUTTON_3` currently do, so the
seven-plus-meaning overload on those buttons (patch-cycle, apply-to-all-tracks,
filter enable/disable, prog add/delete, drum-select, track-add/delete, ...) is
no longer silent.

## What was implemented

Three new files plus a five-line wiring change; no existing renderer or the
button-dispatch function in `main.c` was touched.

- `components/synth_core/include/synth_ui_hint.h` — public API:
  `synth_ui_hint_text()` / `synth_ui_hint_visible()`.
- `components/synth_core/synth_ui/synth_ui_hint.c` — the lookup itself.
  `hint_b1()` (`:12-31`), `hint_b2()` (`:33-44`), `hint_b3()` (`:46-52`) each
  re-derive one button's current meaning by calling the *same* public getters
  `main_button_event_cb` (`main/main.c:291-596`) already calls
  (`synth_ui_filter_is_active()`, `synth_ui_graph_is_active()`,
  `synth_ui_lfo_is_active()`, `synth_ui_arp_is_active()`,
  `synth_ui_prog_is_active()`, `synth_ui_trackopts_is_active()`,
  `synth_ui_drone_is_active()`) plus one direct read of `seq_state.ui_mode`
  (only needed because `synth_ui_toggle_editor_apply_scope()`'s "no-op in
  ARP/DRONE" contract is a `ui_mode` check, not exposed as its own getter),
  in the *identical guard order* main.c uses. `synth_ui_hint_text()`
  (`:55-61`) composes `"1:<b1> 2:<b2> 3:<b3>"` into a static 32-byte buffer.
  `synth_ui_hint_visible()` (`:63-71`) suppresses the strip for two screens
  (see Design decisions).
- `components/display/display_hint.h` / `display_hint.c` — pure rendering,
  no `synth_core`/AMY dependency (matches every other `display_*.c` in this
  component). `display_hint_draw()` (`display_hint.c:13-25`) erases a fixed
  7-row strip (`y=57..63`, the bottom of the 128x64 panel) with draw-color 0,
  then draws the supplied text in `u8g2_font_4x6_tr` at `y=63`.
- `components/synth_core/synth_ui/synth_ui_task.c:220-225` — after the
  existing per-view redraw `switch` (unchanged) and before
  `last_sig`/`last_view` bookkeeping, calls
  `display_hint_draw(s_u8g2, synth_ui_hint_text())` and a second
  `u8g2_SendBuffer(s_u8g2)` when `synth_ui_hint_visible()`. This is the only
  edit to an existing file's logic; `#include "display_hint.h"` /
  `#include "synth_ui_hint.h"` added at `:12-13`.
- `components/synth_core/CMakeLists.txt` / `components/display/CMakeLists.txt`
  — register the two new `.c` files.

## Design decisions

1. **Overlay-after-send, not per-renderer edits.** Every `display_*_draw_frame`
   already ends by clearing, drawing, and calling `u8g2_SendBuffer()`
   (`docs/agent/reference/display-screens.md`). Rather than touch any of the
   nine renderer files (seven `display_*.c` screens plus the filter/LFO/graph
   editors in `ui_editors.c`) to carve out reserved space, the hint bar draws
   *after* the screen's own send (the u8g2 RAM buffer is not cleared by
   `SendBuffer`, so a second draw + send only updates the bottom strip) and
   unconditionally erases that strip first. This means the persistent bar
   covers every view uniformly with a single, contained change, at the cost
   of clipping the bottom few pixels of content on the busiest screens (the
   sequencer grid's last track row, the arp screen's second slot row, the
   ADSR/filter editor's axis tick marks). I judged this an acceptable,
   deliberate trade for a `main.c`/`ui_editors.c`-touching alternative,
   given `feat/a1-second-envelope` is actively editing
   `ui_editors.c`'s envelope screens on a parallel branch (see Pitfalls).
2. **Only `MY_BUTTON_1/2/3` are shown, not `MY_BUTTON_0`/encoder.** 128px at
   the smallest practical font (`u8g2_font_4x6_tr`) budgets roughly
   20-24 characters; `MY_BUTTON_0` (play/pause, occasionally cancel) and the
   encoder (highly view-specific: cycle cursor, adjust value, open editor) are
   comparatively self-explanatory from the screen's own content and were cut
   to keep every state's string well under the width budget. The task's own
   examples ("patch-cycle, apply-to-all-tracks, filter enable/disable") are
   all `MY_BUTTON_1` behaviors, so this button is guaranteed always-shown.
3. **Suppressed for `V_PROG`** (`synth_ui_hint_visible()`,
   `synth_ui_hint.c:65-67`) because `display_prog_draw_frame` already renders
   its own button legend (`"+:B2 -:B1"`, `display_prog.c:97-99`) — drawing a
   second, differently-formatted one in the same footprint would be
   confusing, not clarifying.
4. **Suppressed for the drone visualiser (`V_DRONE_VIS`)**
   (`synth_ui_hint.c:68-70`) because its 8-square gate-pattern grid
   (`display_drone.c` PATTERN section, `y=47..61`) anchors each bar's fill to
   the *bottom* of its square — exactly the pixels the hint strip would
   overwrite — so here the clip would hide live data, not just cosmetic
   padding. Ordinary `V_DRONE` (the parameter list) is unaffected.
5. **Text is re-derived from getters, not the render-dispatch `view` enum.**
   Early in this task I considered keying hint text off the same `view`
   enum `synth_ui_task.c`'s switch already computes (filter > lfo > graph >
   menu > arp > drone > prog > trackopts > seq). That precedence order
   matches the *encoder's* dispatch chain almost exactly, but **not**
   `MY_BUTTON_2`'s: `synth_ui_prog_is_active()` and
   `synth_ui_trackopts_is_active()` do **not** exclude an open graph editor
   (`ui_screen_prog.c:56-58`, `ui_screen_trackopts.c:72-74`), unlike
   `synth_ui_arp_is_active()`/`synth_ui_drone_is_active()`, which do
   (`ui_screen_arp.c:14-18`, `ui_screen_drone.c:272-276`). Concretely: if the
   ADSR/envelope editor is opened while `ui_mode == UI_MODE_PROG`,
   `main_button_event_cb`'s PROG isolation guard (`main.c:357-374`) still
   wins over the later graph-active check for `MY_BUTTON_2`, so `B2` stays
   "+entry" rather than becoming "amp mode" — the reverse of what happens
   for ARP/DRONE in the same situation. Reproducing this exactly required
   calling the raw getters in `main.c`'s literal guard order rather than
   keying off `view`.

## Verified finding (not fixed — out of scope)

While tracing `MY_BUTTON_1`'s real behavior for the Track Options screen, the
top guard in `main_button_event_cb` (`main.c:298-324`) unconditionally
`return`s for any `MY_BUTTON_1` event *before* the later Track-Options
isolation block (`main.c:383-405`) is ever reached. That later block's
`case MY_BUTTON_1: synth_ui_request_add_layer();` (`main.c:385-389`) is
therefore **dead code** — pressing `MY_BUTTON_1` on the Track Options screen
always falls through to the default patch-hold latch, never adds a layer.
The hint bar reflects the real (patch-hold) behavior, not the apparently
intended one. This is a pre-existing `main.c` dispatch bug, out of scope for
a UI-only hint-bar change; flagging for the maintainer to decide whether
`MY_BUTTON_1`'s top guard should itself special-case Track Options, or
whether "add layer" should move to a different button.

A second, smaller asymmetry (not a bug, just worth knowing): `MY_BUTTON_2`'s
PROG/Track-Options isolation guards don't exclude the graph editor the way
ARP/DRONE's do (design decision 5, above) — current behavior is reproduced
faithfully, not altered.

## What is deferred / not done

- **No layout changes to reclaim real margin** on the sequencer grid, arp
  slot grid, drone visualiser, or the filter/LFO/graph editors. All of these
  currently have 0-6px of truly free bottom margin (verified by reading
  `graph_popup.c`'s `plot_rect()` pad constants, `display_seq.c`'s
  `grid_top`/`row_h`, and `display_arp.c`'s slot-cell geometry directly). A
  follow-up that shrinks each screen's content area by a fixed ~7px to give
  the hint bar a clean margin everywhere is a reasonable next step, but is a
  layout change to files two other branches are actively touching
  (`ui_editors.c` for `feat/a1-second-envelope`; conceivably `display_seq.c`/
  sequencer UI for `feat/b3-step-probability`) — deliberately left out of
  this vertical slice to avoid unrelated regressions and merge churn.
- **`MY_BUTTON_0` and the encoder are not shown** in the bar (design decision
  2). If a future pass wants them, the encoder's meaning is fully
  view-dependent (unlike B1/B2/B3, which are derivable from flat getters)
  and would need to key off the render-dispatch `view` the way
  `synth_ui_task.c`'s own switch does.
- **The dead-code `MY_BUTTON_1` Track-Options binding** noted above is
  reported, not fixed.

## Build / verification evidence

Environment note: this worktree's dependency-manager-fetched
`managed_components/espressif__cmake_utilities/gcc.cmake` initially failed
`idf.py set-target` with `GCC link time optimization(LTO) is not supported`
(a `check_ipo_supported()` false negative in this environment, unrelated to
this feature). The main repo worktree carries a local, untracked
(`.gitignore`-excluded, not part of the git history) patch to that same
fetched file that skips the faulty detection; I copied that identical patch
into this worktree's copy of the same untracked, dependency-manager file
(not part of this branch's diff) to reach a working baseline before making
any source changes.

Commands actually run in this worktree, in order, this session:

```
source /home/fatta/esp-idf/S3-Amysynth/export.sh
idf.py set-target esp32s3   # generated sdkconfig from sdkconfig.defaults
idf.py build                # full build, exit 0
touch <the 3 changed/added synth_ui_task.c/synth_ui_hint.c/display_hint.c>
idf.py build                # incremental rebuild of just the touched files, exit 0, no warnings
```

Final build output (both runs): `Project build complete.`
`S3-Amysynth.bin binary size 0xb28d0 bytes ... 30% free`, no compiler warnings
or errors attributable to `synth_ui_hint.c` / `display_hint.c` /
`synth_ui_task.c`. Object files confirmed present at
`build/esp-idf/synth_core/CMakeFiles/__idf_synth_core.dir/synth_ui/synth_ui_hint.c.obj`
and `build/esp-idf/display/CMakeFiles/__idf_display.dir/display_hint.c.obj`.

Not done: on-hardware visual verification (no device access in this
environment, and flashing requires explicit human confirmation per project
policy). The 4px/6px font and 7-row strip geometry is derived from reading
`graph_popup.c`, `display_seq.c`, `display_arp.c`, and `display_drone.c`
source directly (see Design decisions and Deferred sections), not from
running on a real panel — worth a visual sanity check on hardware before
merge, particularly the clipping trade-off on the sequencer/arp/ADSR-editor
screens.

## Potential merge pitfalls

- **`feat/a1-second-envelope`** touches `ui_editors.c` (`graph_target_t`,
  envelope screen). This branch does not touch `ui_editors.c` at all, so
  there is no field-level or line-level conflict, but if that branch changes
  when/how `synth_ui_graph_is_active()` / `synth_ui_lfo_is_active()` become
  true (e.g. new envelope-editor entry points, or a new target that isn't
  ARP/DRONE/melodic), the `hint_b1()`/`hint_b2()` ARP/DRONE exclusion
  (`synth_ui_hint.c:20-22`) should be re-checked against the new
  `graph_target_t` values.
- **`feat/b1-mute-solo`** adds a `sequencer_core` layer struct and TrackOpts
  screen changes. If it changes what `MY_BUTTON_1`/`MY_BUTTON_2` do while
  the Track Options screen is active, `hint_b2()`'s `"DelLy"` case
  (`synth_ui_hint.c:36`) and the dead-code finding above should be
  revisited together, since both concern the same screen's button bindings.
- **`feat/b3-step-probability`** adds new per-step UI; if it adds a new
  screen/overlay (a new `ui_mode_t` value or a new modal), it is not
  represented anywhere in `hint_b1()`/`hint_b2()`/`hint_b3()`'s guard chains
  or `synth_ui_hint_visible()`'s skip list — the bar will fall through to
  whatever the closest existing case matches, which may be misleading until
  a case is added for it.
- **No conflict expected** with `feat/i2s-clock-source` (render clock only)
  or `feat/a2-fm-dx7`/`feat/a4-wavetable`/`feat/a5-pcm-sampling`/
  `feat/a3-portamento-arp` (patch/model/UI-screen work that doesn't touch
  `synth_ui_task.c`, `main.c`'s button dispatch, or any of the seven
  `display_*.c` renderers this feature reads geometry from). If any of them
  *do* end up touching `display_seq.c`/`display_arp.c` layout constants
  (e.g. to add a new HUD element), re-verify the hint strip's `y=57` clip
  line against their new content.
