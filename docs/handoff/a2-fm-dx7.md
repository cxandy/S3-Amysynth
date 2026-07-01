# A2 — FM/DX7 algorithm oscillators (wave=ALGO)

Branch: `feat/a2-fm-dx7`. Status: **complete vertical slice** (engine + preset
bank + live editor UI), build-verified. See "Deferred" for explicitly
out-of-scope depth.

## Summary

Adds AMY's 6-operator DX7-style FM algorithm engine (`ALGO` wave,
`algorithm`/`algo_source[]`, `algorithms.c`'s 33 routing tables — previously
completely unused in this firmware) as a new melodic-voice patch family,
following the exact same "virtual patch ID" pattern the codebase already uses
for raw waveforms (257-263) and multi-osc bass presets (264-266):

- **5 new patch IDs (267-271)**: four fixed starter presets (Bass, E.Piano,
  Bell, Lead) plus one live-editable "Custom" voice.
- **A new FM screen** (`Menu > Screen: FM`) for algorithm selection (0-32,
  the full AMY algorithm range) and per-operator ratio/level editing (6
  operators), reusing the existing scrollable menu-list renderer.
- Reachable through the **existing patch-cycle control** (hold patch button +
  turn encoder on a melodic layer) exactly like every other patch family.

## How an FM voice is wired (read this before touching fm_voice.c)

Every AMY algorithm (`components/amy/src/algorithms.c:37`) wires all 6
operator slots regardless of algorithm index — there is no "N-operator
algorithm", only "6-operator algorithms where some ops may be silenced by the
patch author". This means a single, algorithm-agnostic voice layout is
correct for any of the 33 algorithms:

- relative osc 0 = the AMY `ALGO` control osc (carries `algorithm`,
  `feedback`, `algo_source[0..5]`, and the voice's own envelope/velocity).
- relative oscs 1-6 = `SINE` operators. `algo_source[i]` always points at
  osc `i+1`; AMY offsets everything by the voice's `base_osc` internally
  (`components/amy/src/amy.c:686-698` for the plain event path,
  `components/amy/src/patches.c:1039-1085` — `patches_event_has_voices` — for
  the per-instrument/per-voice path actually used here since every event
  carries `e->synth`).
- an operator's own `amp_coefs[COEF_CONST]` (its "level") is genuinely
  independent per operator (`render_fm_sine`,
  `components/amy/src/oscillators.c:463`: `amp = msynth[osc]->amp * mod_amp`)
  — setting it to 0 silences that operator without needing to know its role
  or omit it from `algo_source[]`. This is the mechanism every preset in
  `fm_presets.c` uses to get a "2-operator" or "3-operator" feel out of a
  6-operator engine: unused operators are always wired, just silent.
- note-on/off addressed at a synth with no `e->osc` set fans out to **every**
  osc in the voice (`patches.c:1039-1085`), but the per-osc `MIDI_NOTE`/
  `VELOCITY` delta handlers in `amy.c` explicitly skip any osc whose
  `status == SYNTH_IS_ALGO_SOURCE` (`amy.c:1233-1237`, `amy.c:1416-1420`) —
  operators get that status the moment `algo_source[]` names them
  (`amy.c:1371-1378`), so only osc 0 (the `ALGO` control osc) actually
  triggers via the normal path; the operators are triggered internally by
  `algo_note_on()`/`note_on_mod()` (`algorithms.c:98-122`). This is why the
  engine code never has to special-case "which osc is the note target" — it
  falls out of AMY's existing dispatch for free.

This was verified by hand-tracing `algorithms.c`'s `render_algo` bus/zero/ADD
logic for algorithm 0 (both its 4-deep serial chain, ops array indices 0-3,
and its simple 2-op pair, indices 4-5) rather than by listening on hardware
(no device access in this environment) — see "What's NOT verified" below.

## Key files

| File | What |
|---|---|
| `components/synth_core/include/sequencer_core.h:39-53` | `SEQ_PATCH_FM_BASE`/`FM_BASS`/`FM_EPIANO`/`FM_BELL`/`FM_LEAD`/`FM_CUSTOM`/`FM_MAX` (267-271) + `sequencer_core_fm_voice_changed()` decl |
| `components/synth_core/include/custompatches/fm_voice.h` | `fm_voice_t` (algorithm, 6x ratio, 6x level, feedback), `s_fm_voice` (the live-editable global voice) |
| `components/synth_core/custompatches/fm_voice.c` | `fm_voice_configure_track()` (full patch load, oscs_per_voice=7), `fm_voice_push_live()` (cheap per-field update for the editor) |
| `components/synth_core/include/custompatches/fm_presets.h`, `.../custompatches/fm_presets.c` | 4 fixed starter presets, all on algorithm 0, dispatched by patch id |
| `components/synth_core/sequencer_core/seq_core_synth.c:248-283` | `is_fm_patch` dispatch branch in `sequencer_configure_synth()`; `:345-355` `sequencer_core_fm_voice_changed()` |
| `components/synth_core/synth_ui/ui_screen_fm.c` | New screen: view-building, encoder/button handlers, ratio-table stepping |
| `components/display/display_menu.c` / `.h` | `display_menu_draw_frame_titled()` — small refactor so the FM screen reuses the existing scrollable list renderer instead of a bespoke one |
| `components/display/display_seq.h:30` | `UI_MODE_FM` added to `ui_mode_t` |
| `components/synth_core/synth_ui/ui_screen_menu.c` | `MI_SCREEN_FM` menu entry -> `UI_MODE_FM` |
| `components/synth_core/synth_ui/synth_ui_task.c` | `V_FM` dispatch case; `fm_voice_default(&s_fm_voice)` at boot |
| `components/synth_core/synth_ui/ui_patch_cycle.c` | `SEQ_PATCH_FULL_MAX` 266->271; FM entries added to the curated cycle list |
| `components/display/patch_names.c` | Names for patches 267-271 |
| `main/main.c` | FM-screen encoder/button routing, mirrors the TrackOpts hooks exactly |

## UI flow (how to reach it)

1. On a melodic layer, hold the patch-select button (`MY_BUTTON_1`) and turn
   the encoder — steps through the curated patch list, which now includes
   `FM Bass` / `FM E.Piano` / `FM Bell` / `FM Lead` / `FM Custom (edit)`
   (267-271). Selecting any of the four fixed presets is immediately audible
   with no further steps (this alone proves the engine end-to-end).
2. To edit the custom voice: open the menu (menu button) -> `Screen: FM`.
   Encoder scrolls: `Algorithm`, `Feedback`, then `OP0 Ratio`/`OP0 Level` ...
   `OP5 Ratio`/`OP5 Level` (14 rows). Encoder-click toggles edit on the
   focused row; while editing, the encoder changes the value and every change
   is pushed live (`sequencer_core_fm_voice_changed()`) to any melodic row
   currently on `FM Custom`.
3. Switch a layer to patch 271 (`FM Custom`) via the patch-hold gesture
   first, then open the FM screen to hear edits in real time.

## Design decisions (and what I rejected)

- **Global "custom" voice, not per-layer/per-track state.** `s_fm_voice` is
  one global struct (mirrors `amy_fx.h`'s `s_fx` global-FX-cache convention),
  not a new field in `seq_layer_t`/`seq_state`. Rejected: adding per-layer FM
  state to `seq_layer_t` — that struct is actively being extended by
  `feat/b1-mute-solo` and `feat/b3-step-probability` in parallel; touching it
  here would guarantee a merge conflict for no real benefit (bass/wave
  presets already work exactly this way — shared state keyed only by the
  patch number, no dedicated per-layer struct fields).
- **All 4 fixed presets use algorithm 0.** Rejected: giving each preset a
  different algorithm for extra "authenticity" — that would have required
  hand-verifying the bus/zero/ADD routing of 3-4 more algorithms by reading
  `algorithms.c`'s flag table, with no way to confirm correctness by ear in
  this environment. Algorithm 0's two chains (a 4-deep serial modulator stack
  op-index 0-3, and a simple 2-op pair op-index 4-5) already give two
  genuinely different structures to build all 4 voicings from by varying
  ratio/level only — the safer choice for a slice I can't audition. The
  `FM_CUSTOM` voice does expose the full 0-32 algorithm range (see below).
- **`FM_CUSTOM` exposes all 33 algorithms, not just algorithm 0.** This is
  safe *by construction*, not because I traced all 33 by hand: every
  algorithm always wires exactly 6 operators (verified from the table at
  `algorithms.c:37-72` — no algorithm leaves an op slot at flag `0x00`), and
  this engine always populates all 6 relative oscs regardless of algorithm.
  There is no per-algorithm special case for the engine to get wrong.
  Whether a given algorithm/ratio/level combination is *musically good* for
  algorithms other than 0 is unverified (see below).
- **One shared envelope for all 6 operators + the control osc's own
  envelope**, not per-operator envelopes. Real DX7 patches give every
  operator an independent EG. Deferred — see below. The existing per-track
  ADSR editor (`seq_core_editors.c`) already reaches the control osc (osc 0)
  for free (envelope-push events with no `e->osc` set land on osc 0, and osc
  0's own amp scales every operator that's a final carrier), so "the voice
  has an editable envelope" already works without new code; only per-operator
  envelope shaping is missing.
- **FM screen reuses `display_menu.c`'s renderer** via a new
  `display_menu_draw_frame_titled()` (title parameter, `display_menu_draw_frame`
  becomes a thin wrapper). Rejected: a bespoke `display_fm.c` — the FM
  screen's shape (scrollable label/value rows, one entered for editing) is
  identical to the menu overlay's, and `display_menu.h`'s own header comment
  already documents it as intentionally decoupled/reusable. This avoided an
  entire new renderer file and a duplicate `menu_item_view_t`-shaped type.
- **Ratio editing steps through a curated table** (0.5, 1, 1.5, 2, 2.5, 3,
  3.5, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16 — `ui_screen_fm.c`), not a raw
  float. Keeps the encoder usable (an unbounded float step would either be
  too coarse or take forever to dial in); these are standard FM ratio
  choices. Level and feedback step linearly in 5% increments.
- **Operators are labelled `OP0`..`OP5` by array index**, not remapped to
  real DX7 `OP1`..`OP6` numbering (which is array-index-reversed per
  `algorithms.c`'s authoring convention — see `fm_voice.h`'s header comment).
  Kept the code's internal indexing as the UI's source of truth rather than
  add a translation layer purely for label cosmetics.

## What's NOT verified (be aware before trusting the sound)

- **No hardware/audio verification was possible in this environment** (no
  device access). All four starter presets and the engine's operator-summing
  behavior are derived from reading `algorithms.c`'s bus-routing flags by
  hand, not from listening. They are safe (won't crash, won't produce NaN or
  silence-by-accident) but the exact ratios/levels are a best-effort FM
  sound-design starting point, not a tuned final voicing. **Before shipping,
  someone should load each of the 4 presets and the default `FM_CUSTOM` voice
  on hardware and adjust `fm_presets.c`'s ratio/level constants by ear.**
- Algorithms other than 0 (reachable via `FM_CUSTOM`'s algorithm row) have
  not been hand-traced individually; they rely entirely on the "every
  algorithm always wires 6 ops, level=0 safely silences an unwanted one"
  argument above, which is structural/general rather than per-algorithm
  verified.
- `feedback` is plumbed through end-to-end but not exercised by any of the
  4 fixed presets (algorithm 0's only `FB_IN` slot, op-index 0, is kept
  silenced in all four) — untested audibly even in isolation.

## Deferred / explicitly out of scope

- **Per-operator envelopes.** All 6 operators currently share one fixed
  short default envelope baked in at patch-load time
  (`fm_voice.c:FM_OP_ATTACK_MS` etc.); only the control osc's envelope is
  user-editable (via the existing per-track ADSR graph editor, for free).
  Real DX7 sound design leans heavily on per-operator EG shape (e.g. a fast
  operator decay for a percussive attack transient under a sustained
  carrier). Extending this needs either 6x more `seq_env_t`-shaped storage
  per voice or a compact encoding; deferred as a distinct follow-up.
- **Full 32-algorithm *diagram/labelling* UI.** The engine and the encoder
  control both already span the full 0-32 range; what's missing is any
  visual/textual indication of *which operators are carriers vs modulators*
  for the currently-selected algorithm (real DX7 hardware shows this as a
  printed panel diagram). Without it, exploring algorithms other than 0
  requires knowing/reading `algorithms.c`'s flag table. A follow-up could
  render a tiny ASCII/graphic algorithm diagram, or a lookup table mapping
  each algorithm's role per operator into the row labels.
- **Per-operator detune / fixed-frequency mode / operator on-off toggle
  independent of level.** Real DX7 operators have a few more knobs (coarse/
  fine ratio split, fixed-Hz mode, individual on/off separate from output
  level). Not exposed; `op_level=0` doubles as "off" for this slice.
- **Arp/drone FM support.** Scoped to melodic sequencer layers only (per the
  task's primary framing). `arp_core`/`drone_core` still clamp their own
  patch selection well below the FM range internally
  (`seq_core_synth.c:sequencer_core_arp_configure` clamps to
  `SEQ_PATCH_WAVE_MAX`=263; `drone_core.c:DRONE_PATCH_MAX` clamps to
  `SEQ_PATCH_TRIANGLE`=261, tighter still) — extending the shared
  `SEQ_PATCH_FULL_MAX` constant used by `ui_patch_cycle.c` does not change
  this, exactly as it already didn't for the pre-existing bass-preset range;
  wiring FM into arp/drone would need its own oscs_per_voice-aware plumbing
  in those modules.

## Build/verification evidence

Environment note: MCP `esp-idf-eim` tools are bound to the main worktree, so
per the task's documented exception this was verified with raw `idf.py` from
inside `.claude/worktrees/a2-fm-dx7`, sourcing `export.sh` and invoking
`idf.py` via `python "$IDF_PATH/tools/idf.py"` (the activation script does not
put `idf.py` itself on `PATH`, only its toolchain dependencies).

1. `idf.py set-target esp32s3` — generated `sdkconfig` from the tracked
   `sdkconfig.defaults` successfully, **but** the subsequent CMake configure
   failed with `GCC link time optimization(LTO) is not supported`
   (`managed_components/espressif__cmake_utilities/gcc.cmake:53`). Root-caused
   by manually re-running CMake's internal `check_ipo_supported()` try-compile
   probe (`build/CMakeFiles/_CMakeLTOTest-CXX/bin`, via `ninja`): the compiler
   invocation was passed a malformed response-file reference (`@"/cxxflags"`,
   an absolute path missing its directory prefix) and failed before any of
   this feature's code was involved. This reproduced deterministically on a
   **fresh** configure and is unrelated to any change in this branch — it is
   a CMake/toolchain interaction specific to `CheckIPOSupported`'s try-compile
   sub-project not inheriting build-dir context correctly in this sandbox.
   The main repo's long-lived `build/` directory never re-triggers this
   probe (no reason to reconfigure since `CMakeLists.txt` there hasn't
   changed), which is why this hadn't surfaced before.
2. To unblock verification of the actual C code, `CONFIG_CU_GCC_LTO_ENABLE`
   was temporarily flipped to `n` in this worktree's **generated, untracked**
   `sdkconfig` only (not `sdkconfig.defaults`, not committed, not present in
   `git status` as a tracked change) purely to route around the CMake probe.
   With that one local flip:
   - `idf.py build` (full clean build from an empty `build/` dir): **succeeded**,
     `Project build complete`, `S3-Amysynth.bin` generated
     (0xb1f60 bytes, 30% partition free).
   - A second targeted rebuild after `touch`-ing every file this branch
     added/modified confirmed all of them compile cleanly with **zero
     warnings or errors** (`fm_voice.c`, `fm_presets.c`, `ui_screen_fm.c`,
     `seq_core_synth.c`, `ui_screen_menu.c`, `ui_patch_cycle.c`,
     `synth_ui_task.c`, `display_menu.c`, `patch_names.c`, `main.c`).
   The `CONFIG_CU_GCC_LTO_ENABLE` flip was reverted back to `y` afterward so
   the worktree's local `sdkconfig` matches `sdkconfig.defaults` again (it is
   untracked either way and will not be committed).
3. **Not done in this session**: an LTO-enabled build. Given the LTO-off
   build compiled and linked this branch's code with zero diagnostics, and
   the LTO failure is demonstrably a pre-existing environment/CMake issue
   independent of these changes, the residual risk is low but not zero —
   **the human reviewer should run this branch through the normal MCP
   `build_project` flow in the main worktree (which has a working persisted
   build cache and real LTO) before merging**, per the "Potential merge
   pitfalls" note below.

## Potential merge pitfalls

- **`components/synth_core/sequencer_core/seq_core_synth.c`** — heavily
  touched here (`is_fm_patch` branch, clamp-bound changes, new
  `sequencer_core_fm_voice_changed()`). `feat/b1-mute-solo` and
  `feat/b3-step-probability` both plausibly touch this file too (mute/solo
  gating and per-step evaluation both live in `sequencer_configure_synth()`'s
  neighborhood). Expect a textual merge conflict; the FM changes are additive
  (`is_fm_patch` is a new independent boolean/branch, not a rewrite of
  existing branches) so conflict resolution should be mechanical, not
  semantic — re-verify the `if/else if` chain in
  `sequencer_configure_synth()` still has exactly one dispatch per patch
  after merging.
- **`components/display/display_seq.h`'s `ui_mode_t` enum** — `UI_MODE_FM`
  appended at the end (value 5). If another branch also appends a new
  `UI_MODE_*` at the same position, values will silently collide/renumber on
  merge. Low risk since none of the parallel branches in the table describe
  adding a new top-level screen mode, but worth a `grep -n "UI_MODE_" -r` pass
  after merging everything.
- **`main/main.c`** — `feat/c1-hint-bar` explicitly touches "main.c overlay
  dispatch, persistent one-line status string". This branch's main.c edits
  are two small additive `if (synth_ui_fm_is_active())` blocks in the
  existing per-screen isolation chain (mirroring the TrackOpts hooks exactly)
  — should merge cleanly as long as c1-hint-bar's changes are in a different
  region (status-bar rendering rather than the button/encoder routing
  chains this branch touches). Worth a manual smoke-check that the FM screen
  still isolates correctly (encoder/button don't leak into sequencer editing)
  after merging both.
- **`components/synth_core/synth_ui/ui_screen_menu.c`** — new `MI_SCREEN_FM`
  entry appended to the `menu_item_id_t` enum and the menu item table. Any
  other branch adding a menu entry (none currently listed in the parallel
  table, but `feat/a3-portamento-arp` or similar could plausibly want a menu
  toggle) would need the same append-at-end treatment; check for enum-value
  drift after merging.
- **`components/display/display_menu.c`/`.h`** — the `display_menu_draw_frame_titled()`
  refactor changes `display_menu_draw_frame`'s body into a thin wrapper. Low
  risk (behavior-preserving for existing callers) but flagging since any
  other branch that also touches `display_menu.c` will see this diff.
- **AMY oscillator budget** — an FM voice costs 7 oscs per melodic row
  (vs. 1-2 for existing patches). Selecting `FM_*` as the *global* melodic
  patch (`sequencer_core_set_melodic_patch`, applies to every melodic layer)
  with all 4 layers x 4 tracks active would cost up to 16 x 7 = 112 oscs,
  plus drums/arp/drone. Still comfortably under the documented `max_oscs=250`
  budget (`docs/agent/00-CONTEXT-CARD.md`), but if another parallel branch
  also grows osc consumption (e.g. `feat/a4-wavetable`, `feat/a5-pcm-sampling`),
  re-check the combined worst-case budget once both land.
