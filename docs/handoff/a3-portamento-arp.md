# A3 — Portamento / Glide (Arp)

Branch: `feat/a3-portamento-arp`
Scope: arp only (per the task's "To Implement" note, the melodic sequencer is
a candidate for a follow-up pass, not included here).

## What was implemented

Exposes AMY's built-in portamento (`portamento_ms` / `PORTAMENTO` delta,
`components/amy/src/amy.h:558`) as a single per-arp scalar control, "GLIDE",
editable from the arp screen with the same cursor+encoder convention already
used for GATE/OCT/RATE. No AMY render-path or scheduling changes — the glide
is computed entirely inside AMY's existing `portamento_alpha` low-pass on
`logfreq` (`components/amy/src/amy.c:1574-1577`).

### Core state + AMY push

- `components/synth_core/include/arp_core.h:140-152` — new
  `arp_set_portamento_ms(uint16_t)` / `arp_get_portamento_ms(void)` plus
  `ARP_PORTAMENTO_MAX_MS` (2000 ms ceiling).
- `components/synth_core/arp_core.c`:
  - `arp_state_t.portamento_ms` field (new, defaults to 0/off via the
    existing `memset` in `arp_core_init`).
  - `arp_push_portamento()` (static) — pushes `e->portamento_ms` to the arp
    synth via `amy_helpers_event_begin/send`, `e->osc` and `e->velocity` left
    unset so AMY fans it out to every voice's base osc
    (`patches_event_has_voices`, `components/amy/src/patches.c:1039-1082`).
  - `arp_set_portamento_ms()` — clamps via `SEQ_CLAMP_U16`, stores, and calls
    `arp_push_portamento()` directly. This is **not** a scheduling change, so
    unlike the octave/rate/gate setters it does not call `arp_mark_dirty()`
    — no coalesced re-emit needed.
  - `arp_rebuild()` now ends with an unconditional `arp_push_portamento()`
    call. Both the WAVE-mode pool re-init and the PATCH-mode
    `amy_send_patch()` path reset every osc's `portamento_alpha` to 0 inside
    AMY (`reset_osc_params`, `components/amy/src/amy.c:801`), so any
    source/patch/wave change would otherwise silently drop a configured
    glide time.

### UI

The compact 128x64 arp screen has no spare pixel row (2 macro rows + a
2x4 note-slot grid already fill the frame — see `display_arp.c`), so GLIDE
was **not** added as a new row. Instead it reuses the existing "cursor
determines which field occupies the row-2 right-hand slot" convention already
used for SOURCE/WAVE/patch-number:

- `components/display/display_arp.h` — new `ARP_CUR_PORTA` cursor stop
  (index 7, between WAVE and the slot grid; `ARP_CUR_SLOT0` shifted from 7 to
  8), new `arp_view_t.portamento_ms` field, updated cursor-index doc comment.
- `components/display/display_arp.c:58-87` — row-2 right-hand block gains a
  third case (`sel_porta`): when the cursor is on GLIDE it draws
  `"GLIDE:<ms>"` in the same slot SOURCE/WAVE/patch already share; otherwise
  behaviour is unchanged.
- `components/synth_core/synth_ui/ui_screen_arp.c`:
  - `arp_build_view()` — populates `out->portamento_ms`.
  - `arp_view_signature()` — hashes it in so the OLED only redraws on change.
  - `arp_edit_value()` — new `ARP_CUR_PORTA` case, `dir * 25 ms` per detent,
    clamped `0..ARP_PORTAMENTO_MAX_MS`, mirroring the existing GATE case's
    style (`SEQ_CLAMP_INT` + direct setter call).
  - `synth_ui_arp_handle_encoder()` — the existing "skip WAVE cursor in PATCH
    mode" special case now lands on `ARP_CUR_PORTA` (was `ARP_CUR_SLOT0`)
    when moving forward, since GLIDE is always visible regardless of source.

Encoder/button semantics for GLIDE are identical to every other main-screen
arp field: move cursor to it, short-press `MY_BUTTON_ENC` to toggle edit,
turn to adjust, short-press again to commit/lock. No new button bindings.

### Docs

`components/synth_core/ARP-ARCHITECTURE.md` — corrected the (already stale)
cursor-index-space line and added the new getter/setter to the API listing,
with a note that the file is illustrative rather than exhaustive (it already
didn't cover SOURCE/WAVE/filter/LFO/amp-scale before this change — not
re-synced in full here, out of scope for this task).

## Design decisions (made autonomously, no human check-in)

1. **Screen placement: shared row-2 slot, not a graph-popup mode.** The only
   existing precedent for "one more per-arp scalar with no free screen row"
   is `amp_scale`, which is edited via the graph popup's amp-mode overlay
   (`ui_editors.c`, `MY_BUTTON_2` while the ADSR editor is open). I rejected
   that route for GLIDE: `s_graph_amp_mode` in `ui_editors.c` is shared
   across melodic/drone/arp targets (`graph_target_t`), and portamento is
   explicitly arp-only for this pass — bolting an arp-only third mode onto a
   melodic/drone/arp-shared toggle would either misbehave for the other two
   targets or need target-conditional branching in an already-large,
   991-line file that `feat/a1-second-envelope` is concurrently restructuring
   (see Merge Pitfalls). Reusing the main screen's existing cursor/dial
   pattern (matching GATE/OCT exactly) is self-contained to arp-only files,
   costs one `if/else` branch in an already-cursor-swapped display slot, and
   needs no new button gesture.
2. **Range: 0..2000 ms, 25 ms/detent.** No existing precedent for editing a
   wide `uint16_t` ms value via bare cursor+dial (the ADSR ms fields are
   graph-dragged, not detent-stepped). 2000 ms comfortably covers musical
   glide times; 25 ms/detent gives ~80 turns edge-to-edge, in the same
   ballpark as GATE's 18-turn 10..100 range scaled for a ~20x wider domain.
   Tunable later; not backed by a hardware measurement.
3. **Push-on-set, not mark-dirty.** Portamento isn't a scheduling parameter
   (unlike octaves/rate/gate/slots, which change which AMY SEQUENCE events
   exist) — it only affects how AMY glides between notes it already plays.
   Pushing it immediately (not coalesced through `arp_core_service()`) keeps
   it simpler and avoids introducing a second "reason to be dirty" into the
   existing single dirty-flag/re-emit contract.
4. **Always re-push after `arp_rebuild()`, not gated by an `_authored` flag.**
   Unlike env/filter/LFO (which only reassert when the user has explicitly
   authored a custom one, so an un-authored patch's own envelope isn't
   clobbered), portamento has no "patch-owned" equivalent to defer to — 0 is
   always a safe, correct default to reassert, so there is no
   `portamento_authored` flag and no conditional.

## What is deferred / not done

- **Melodic sequencer glide.** Explicitly out of scope for this pass per the
  task description; `sequencer_core`/`seq_core_*` files are untouched. A
  follow-up would need a per-track field in `seq_layer_t` (mind
  `feat/b3-step-probability`'s per-step field budget — see below) and a push
  helper mirroring `arp_push_portamento()`, likely promoted into
  `sequencer_core.c` if shared with the arp.
- **`synth_delay_ms` (voice-steal grace).** Named alongside portamento in the
  source doc but not implemented — it's an instrument/synth-setup field
  (`amy_send_patch`/`patches_load_patch` time), a different mechanism from a
  per-note runtime control, and a separate scoped change.
- **No persistence.** Like every other arp field, `portamento_ms` lives in
  the file-static `arp_state_t` and resets to 0 on reboot; this matches all
  existing arp state (no NVS anywhere in `arp_core.c` yet).
- **No drone exposure.** Drone (`drone_core.c`) was not touched; it has its
  own dB-model amp/mod plumbing worth a separate look before adding glide
  there (drone's `amp_combine_controls` MOD/BEND divergence, per
  `docs/agent/amy-internals.md` §AMP, could interact with portamento
  differently than the arp's plain WAVE/PATCH paths — not verified here).

## Build/verification evidence

Environment: `source /home/fatta/esp-idf/S3-Amysynth/export.sh`, then raw
`idf.py` from inside this worktree (per the task's documented MCP exception).

1. `idf.py set-target esp32s3` (fresh worktree, first run) hit a **pre-existing
   environment issue unrelated to this change**: CMake 4.0.3's
   `check_ipo_supported()` (invoked by `managed_components/espressif__cmake_utilities/gcc.cmake`
   because `CONFIG_CU_GCC_LTO_ENABLE=y`) runs a nested `try_compile(... PROJECT ...)`
   that re-executes IDF's response-file toolchain
   (`tools/cmake/toolchain.cmake`) in a context where
   `IDF_TOOLCHAIN_BUILD_DIR` resolves empty, producing a literal `@"/cxxflags"`
   compiler argument and a hard `linker input file not found: /cxxflags`
   failure — confirmed by manually re-running `ninja` inside the generated
   `build/CMakeFiles/_CMakeLTOTest-CXX/bin` test project. This reproduced
   deterministically on a clean `build/` in this worktree, occurs before any
   of this change's files are even compiled, and is independent of the
   config both `sdkconfig.defaults` here and the main repo's already-built
   `sdkconfig` set `CONFIG_CU_GCC_LTO_ENABLE=y` identically. It is a
   CMake-version/IDF-toolchain interaction, not something introduced by this
   diff, and per the project's hard invariant ("amy is in the LTO set — keep
   it") this is not something to "fix" by disabling LTO in the tracked
   config.
2. To still get a real, evidence-backed build verification without touching
   any tracked file, I temporarily flipped `CONFIG_CU_GCC_LTO_ENABLE` to `n`
   in the **local, untracked, generated** `sdkconfig` (never staged/committed;
   restored to `y` again afterwards to match `sdkconfig.defaults`) and ran:
   ```
   idf.py build
   ```
   Result: **clean full build, `Project build complete.`**, producing
   `build/S3-Amysynth.bin` (0xb1880 bytes, 31% of the app partition free).
3. Touched only this change's three modified source files
   (`arp_core.c`, `ui_screen_arp.c`, `display_arp.c`) and rebuilt — all three
   compiled with **zero warnings/errors**, followed by a successful relink.
4. **Not exercised in this session:** an LTO-enabled build (blocked by the
   environment issue above, not by this diff) and on-hardware behavior (no
   device access; flashing requires explicit human confirmation per project
   policy, which was not requested). **The human reviewer should re-run the
   standard MCP `build_project` flow in the main repo worktree** (which
   already has a warm, LTO-successful `build/` and `sdkconfig`) against this
   branch's diff before merging, to get a real LTO-enabled build signal this
   session couldn't produce.

## Potential merge pitfalls

- **`feat/a1-second-envelope` — `components/synth_core/synth_ui/ui_editors.c`.**
  Not touched by this change at all (deliberately — see Design Decision 1),
  but if a future iteration wants a graph-popup-based glide control instead
  of the main-screen field added here, it will land in the exact
  `graph_target_t` / `s_graph_amp_mode` region a1 is expanding. No line-level
  conflict expected from *this* branch, but flagging the shared file for
  awareness.
- **`feat/b1-mute-solo` / `feat/b3-step-probability` — sequencer_core
  per-track/per-step structs.** Not touched here at all; this pass stayed
  entirely inside `arp_core.c`/`arp_core.h`/`display_arp.*`/`ui_screen_arp.c`.
  No overlap expected, but if a future melodic-glide follow-up (deferred
  above) lands a portamento field into `seq_layer_t`, it should be sequenced
  after b1/b3 land to avoid a field-layout collision in that struct.
- **`feat/c1-hint-bar` — `main.c` overlay dispatch.** Not touched; this
  feature added no new button gestures or overlay text, so no expected
  interaction.
- **Cursor-index renumbering.** `ARP_CUR_SLOT0` moved from 7 to 8 and
  `ARP_CUR_COUNT` grew by one. Any other in-flight branch that hard-codes
  `ARP_CUR_*` numeric literals instead of the enum-like defines (grep
  confirms none currently do outside the files this change touches) would
  need re-checking; anything using the `ARP_CUR_*` symbols themselves is
  unaffected by the shift.
