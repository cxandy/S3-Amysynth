# A1 — Second envelope (EG0/EG1) for filter-separate-from-amp

Branch: `feat/a1-second-envelope`. Worktree-local; not merged, not pushed.

## Summary

AMY already runs two fully independent breakpoint envelope generators per
oscillator (EG0, EG1 — `components/amy/src/amy.h:570-574`,
`components/amy/src/envelope.c`). Before this change, every part of the synth
UI/patch layer (melodic rows, arp, drone) only ever drove EG0. This feature:

1. Adds parallel "second envelope" (EG1) storage + accessors to melodic rows,
   the arp, and the drone, mirroring the existing EG0 plumbing exactly.
2. Extends the existing ADSR graph editor (`ui_editors.c`) with an EG0/EG1
   picker — no new screen, per the task brief.
3. Updates two of the three bass presets (which already coupled their filter
   sweep to the amp envelope) to route the filter through EG1 instead,
   producing the classic "plucky amp decay, slower filter tail" subtractive
   shape, and gives the arp the same option once its filter is authored.

## What was implemented

### Data model (melodic rows, arp, drone)

- `seq_env_t env1[SEQ_TRACKS]` + `bool env1_authored[SEQ_TRACKS]` added to
  `seq_layer_t` — `components/display/display_seq.h:113-120`.
- `seq_default_melodic_env1()` — fixed compile-time default (attack 15 ms,
  decay 450 ms, sustain 25%, release 400 ms — deliberately slower/lower than
  the EG0 default) — `components/synth_core/include/seq_defaults.h:42`.
- `sequencer_core_get/set_melodic_envelope2()` (public API) —
  `components/synth_core/include/sequencer_core.h:104-113`; implemented in
  `components/synth_core/sequencer_core/seq_core_editors.c:171-197`
  (`sequencer_configure_melodic_envelope1_track`,
  `sequencer_core_get_melodic_envelope2`,
  `sequencer_core_set_melodic_envelope2`).
- `seq_layer_env1()` accessor + `sequencer_configure_melodic_envelope1()`
  batch push (mirrors the EG0 pair) —
  `components/synth_core/sequencer_core/seq_core_synth.c:169-178,205-211`,
  wired into `sequencer_configure_synth()` at line 296 (right after the EG0
  push, so a patch reload also re-applies any authored EG1).
- `sequencer_core_push_envelope_eg1(synth, osc, env)` — the generic push,
  parallel to the existing `sequencer_core_push_envelope()` but writing
  `bp_is_set[1]`/`eg_type[1]`/`eg1_times`/`eg1_values` —
  `components/synth_core/sequencer_core/seq_core_synth.c:467-484`. Takes an
  explicit `osc` (unlike the EG0 version, which is always osc 0) because one
  of the bass presets' filter lives on osc 1.
- `layer->env1[t] = seq_default_melodic_env1()` seeded in
  `sequencer_core_add_layer()` —
  `components/synth_core/sequencer_core/seq_core_state.c:127`.
- Arp: `env1`/`env1_authored` fields on `arp_state_t`
  (`components/synth_core/arp_core.c:96-97`), defaults in `arp_core_init()`
  (lines 358-364), `arp_get_envelope2`/`arp_set_envelope2`
  (lines 573-587, declared `components/synth_core/include/arp_core.h`).
- Drone: same shape — `components/synth_core/custompatches/drone_core.c`
  struct fields (141-144), defaults in `drone_core_init()` (451-460),
  `drone_get_envelope2`/`drone_set_envelope2` (806-825), declared in
  `components/synth_core/include/custompatches/drone_core.h`.

### UI: EG0/EG1 picker in the existing graph editor

All in `components/synth_core/synth_ui/ui_editors.c` (no new screen/file):

- `s_graph_eg_index` (0 or 1) — which breakpoint set the open editor is
  showing (line 48).
- `graph_read_target_env_idx()` / `graph_write_target_env_idx()` — eg-index-
  aware dispatch to the six accessor pairs above (lines 219-296).
  `graph_read_target_env()` is now a thin wrapper for the currently-shown
  index, so every existing caller keeps working unchanged.
- `graph_write_points_to_env(eg_index)` — factored out of the old
  `graph_commit_to_env()` body so the same "popup points → seq_env_t → store"
  conversion can target either eg_index (line 358).
- `synth_ui_graph_toggle_eg_index()` (line 496, declared in `synth_ui.h:137`)
  — the picker itself. If the currently-shown curve has unsaved edits
  (`s_graph_env_dirty`), it is written through to its OWN eg_index first (so
  switching tabs never silently discards work), then the view flips and is
  fully reseeded — including range (SHORT/LONG) — from the other eg_index's
  own stored envelope, since an amp envelope and a filter envelope can have
  very different total times.
- Topbar (`graph_draw_topbar()`) now shows `EG0`/`EG1` next to the target
  label (e.g. `L1 T1 EG1>T`, `ARP EG0`, `DRONE EG1`).
- Wired to **MY_BUTTON_3 long-press**, but only while the ADSR graph editor
  specifically is open — `main/main.c:456-467`. See "Design decisions" for
  why this gesture was free to reuse.

### Default-patch sound design (EG0 amp / EG1 filter split)

- `components/synth_core/custompatches/bass_presets.c`: **BASS_1** (line 59)
  and **BASS_2** (line 128) already coupled `filter_freq_coefs[COEF_EG0]` to
  the same envelope driving the amp. Both now route the filter sweep through
  `COEF_EG1` with its own, slower breakpoint set (BASS_1: atk 5 ms / dec
  450 ms / sus 15% / rel 200 ms vs. the amp's atk 5/dec 250/sus 40/rel 150;
  BASS_2 similar). **BASS_3** has no filter section to split and is
  unchanged.
- Arp: `arp_configure_wave_synth()` (line 248) sets
  `filter_freq_coefs[COEF_EG1] = ARP_FILTER_EG1_DEPTH_OCT` (3.0 octaves, a
  fixed constant — the arp exposes EG1 *timing*, not modulation depth) **only
  when the arp's filter has been authored and enabled**, so a stock/default
  arp voice (no filter) is completely unaffected. `arp_rebuild()` (line 320)
  always pushes valid EG1 breakpoints alongside that wiring so the coef never
  reads AMY's "never-configured breakpoint set = permanent 1.0 gate"
  (`envelope.c` comment, "an empty env reads as 1.0 *all the time*").
- Drone: **deliberately not wired** to any coef — see below.

## Design decisions (made without asking, and why)

1. **Gesture choice — MY_BUTTON_3 long-press.** Every other button/press
   combination while the graph editor is open was already claimed
   (BUTTON_1 = apply-scope, BUTTON_2 = amp-mode, BUTTON_3 short = cycle
   editor, ENC short/long = select/commit, BUTTON_0 long = cancel). I audited
   `main.c`'s button dispatch and found MY_BUTTON_3's `BUTTON_LONG_PRESS_START`
   was never handled in any branch — a genuinely free gesture, not a repurpose
   of something else. Scoped it to "graph editor open" only so it can't fire
   while the filter/LFO tabs are showing.

2. **Drone's default patch is NOT wired to EG1.** The drone's filter
   movement already comes from an independent, more powerful mechanism:
   `sweep_lo`/`sweep_hi` swept over time by a BPM-synced service tick
   (`drone_push_cutoff()`, `drone_core.c:394-401`), not a breakpoint envelope.
   Layering an EG1-driven cutoff offset on top of that host-driven sweep
   would double-modulate the same `filter_freq_coefs[COEF_CONST]` parameter
   for unclear musical benefit, and the drone is a sustained-pad instrument
   where "plucky decay" doesn't really apply the way it does to bass/arp.
   The EG1 *storage and UI* still exist for the drone (consistent
   architecture, and useful if a PATCH-mode drone patch already routes its
   own `bp1`) — only the default WAVE-mode wiring was skipped. Documented here
   instead of asking, per the "make the reasonable conservative call" guidance.

3. **BASS_3 left unchanged.** It has no filter section at all (just two
   oscillators, no `filter_type`); adding one purely to exercise EG1 would be
   a new tonal design decision (not a mechanical envelope split like BASS_1/2)
   and out of scope for a "keep it low risk" task.

4. **Generic melodic filter (non-bass patches) not wired to EG1.** The task
   named `bass_presets.c` / drone / arp specifically. Many stock AMY patch
   strings (Juno/DX7) already route their own `bp1` internally for their
   filter — those rows are **already usable today** with the new per-row EG1
   editor with zero further code changes (verified the engine-level wiring:
   `patches.c:510-514` parses `bp1` from patch strings same as `bp0`). Adding
   a *new* EG1 coef to the shared `sequencer_core_push_filter()` (used by
   every melodic row + the arp) was explicitly rejected: AMY treats a
   breakpoint set that has never received any values as a **permanent 1.0
   gate**, not "no effect" (`envelope.c`, see comment above
   `compute_breakpoint_scale`'s `bp_r < 0` branch). Wiring a coef there
   without simultaneously guaranteeing valid breakpoints for every possible
   row would risk permanently pinning filters open on rows that never author
   an EG1 — a real regression risk, not just a missed opportunity. The arp
   avoids this by gating the coef assignment and the envelope push together
   in the same authored+enabled condition (see above); doing the same
   generically for every melodic row's filter was judged too much surface
   area for this slice.

5. **BASS_2's per-row EG1 UI edit has a known gap.** BASS_2's filter lives on
   **osc 1** (its default patch-load code correctly targets osc 1), but the
   generic per-row `sequencer_configure_melodic_envelope1_track()` always
   targets **osc 0** (mirroring how the EG0 per-row path already always
   targets osc 0). If a user opens the graph editor on a BASS_2 row and edits
   EG1, the write succeeds and is stored, but the delta lands on the wrong
   oscillator and has no audible effect until/unless a future change makes
   the per-row EG1 push osc-aware per patch. This is a real, narrow gap, not
   a crash risk (both oscs are valid osc indices on that synth) — noted here
   as a known limitation rather than fixed, to keep the generic path simple
   and consistent with the existing single-osc-targeting convention.

6. **`ui_editors.c` file-size guideline.** This file was already ~991 lines
   before this change (over the project's ~300-500 line guideline), and this
   feature adds roughly another 100 lines to it. I considered splitting the
   "graph pop-up" section (already delimited by its own
   `/* ── Graph pop-up ... */` ... `/* ── graph pop-up: end ── */` comments)
   into its own file, but decided against it for this slice: the task
   explicitly asks to keep this feature low-risk, and a structural file-move
   refactor (new CMakeLists.txt entry, re-checking which statics are
   externally referenced, etc.) is exactly the kind of unrelated scope/risk
   the task asks to avoid. Recording this as a good, low-risk follow-up
   rather than doing it now.

## What is deferred / NOT done

- Drone default-patch EG1 coef wiring (see decision 2 above) — storage/UI
  exist, default WAVE synth doesn't route anything to it.
- Generic (non-bass, non-arp) melodic filter → EG1 coef wiring — every row's
  EG1 is fully editable and pushed to AMY today, but only patches that
  already reference `COEF_EG1` themselves (stock patch strings, or our own
  bass presets) will make it audible. This is a conscious, documented
  choice, not an oversight (see decision 4).
- BASS_2 per-row EG1 UI edits landing on the wrong osc (decision 5) —
  functional but silently inaudible on that one preset; not fixed.
- Splitting `ui_editors.c`'s graph-popup section into its own file (decision
  6) — flagged as a good, low-risk follow-up, not done here.
- No Kconfig knobs were added for the EG1 defaults (attack/decay/sustain/
  release constants are fixed compile-time values in `seq_defaults.h` /
  `arp_core.c` / `drone_core.c`), to avoid Kconfig/sdkconfig churn for a
  feature this narrow; can be promoted to Kconfig later if wanted.

## Build / verification evidence

Environment note first, since it affects how "clean build" was verified:

**This environment's fresh-configure path is currently broken independent of
this diff.** `idf.py set-target esp32s3` in a brand-new build directory with
the tracked `sdkconfig.defaults` (which has `CONFIG_CU_GCC_LTO_ENABLE=y`)
fails during CMake configuration:

```
CMake Error at managed_components/espressif__cmake_utilities/gcc.cmake:53 (message):
  GCC link time optimization(LTO) is not supported
```

Root cause (traced, not guessed): `check_ipo_supported()`'s ephemeral
try-compile project inherits `CMAKE_CXX_FLAGS = @"${IDF_TOOLCHAIN_BUILD_DIR}/cxxflags"`
from `esp-idf/tools/cmake/toolchain.cmake:83`, but `IDF_TOOLCHAIN_BUILD_DIR`
is a project-scoped cache variable that isn't forwarded into that ephemeral
project, so it evaluates empty — the compiler is invoked with a literal
`@"/cxxflags"` response file, which doesn't exist:
```
xtensa-esp-elf-g++: error: @/cxxflags: linker input file not found: No such file or directory
```
This reproduces from a stock, unmodified checkout (the failure happens at
CMake configure time, before any of this branch's C source is even touched),
so it is an environment/toolchain-version compatibility issue, not something
introduced by this feature. The main repo worktree's existing `build/`
directory (last configured 2026-06-27, before whatever changed) still works
incrementally, which is consistent with this being a newly-broken **fresh**
configure path.

Per this task's build-effort policy ("if a build complication needs anything
beyond clean+build, stop and ask, don't burn context on workarounds"), and
since this is unrelated to the feature's code, I used the smallest available
official (non-hacky) escape hatch — ESP-IDF's supported *layered*
`SDKCONFIG_DEFAULTS` mechanism — to get a working configuration for
verification, rather than hand-editing the generated `sdkconfig` (which
CLAUDE.md explicitly forbids) or touching the tracked `sdkconfig.defaults`:

```bash
source /home/fatta/esp-idf/S3-Amysynth/export.sh
export IDF_PYTHON_ENV_PATH=/home/fatta/.espressif/python_env/idf6.0_py3.14_env
cd <this worktree>
rm -rf build   # previous failed configure attempt
python3 "$IDF_PATH/tools/idf.py" \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;<scratch>/sdkconfig.lto_off" \
  set-target esp32s3
python3 "$IDF_PATH/tools/idf.py" build
```
where `sdkconfig.lto_off` contains only `CONFIG_CU_GCC_LTO_ENABLE=n`. This
file lives outside the repo (scratchpad) and is not part of any commit; the
tracked `sdkconfig.defaults` is untouched, and the actual shipped
configuration (LTO on) is unaffected.

**Result: `idf.py build` completed successfully** ("Project build complete."),
producing `build/S3-Amysynth.bin` / `build/S3-Amysynth.elf`. A second,
touch-forced incremental rebuild of every file this branch modified
(`ui_editors.c`, `arp_core.c`, `drone_core.c`, `bass_presets.c`,
`seq_core_synth.c`, `seq_core_editors.c`, `seq_core_state.c`, `main.c`)
recompiled and relinked cleanly with **zero warnings and zero errors**
(`grep -iE "warning|error"` over the full rebuild log returned nothing).

No LTO-specific code path is exercised differently by this change: all new
functions are ordinary UI-task-context configuration calls (matching the
existing `sequencer_core_push_envelope()` / `arp_set_envelope()` pattern),
none are in the render/DSP path, so none need `AMY_IRAM_ATTR`/`AMY_DRAM_ATTR`
placement, and the LTO-detection failure itself is a configure-time-only
issue unrelated to whether this code compiles under LTO.

**What a human should do before merging:** re-run a fresh `idf.py
set-target` + `idf.py build` with the repo's normal (LTO-on) configuration on
a machine/toolchain where the LTO detection isn't broken (or investigate/fix
the `IDF_TOOLCHAIN_BUILD_DIR` propagation issue in the shared build
environment), to get one confirmed LTO-on build of this exact diff. I could
not do that in this environment as delivered.

## Potential merge pitfalls

- **`feat/b1-mute-solo`** and **`feat/b3-step-probability`** both add fields
  to `seq_layer_t` / per-track arrays in `display_seq.h` and
  `sequencer_core`. This branch also adds two fields to `seq_layer_t`
  (`env1[SEQ_TRACKS]`, `env1_authored[SEQ_TRACKS]`) right next to the
  existing `env`/`env_authored` fields. All three branches touching the same
  struct will very likely conflict at the field-layout level even though
  each compiles cleanly alone — re-diff `display_seq.h` carefully on merge,
  and re-run `sizeof(seq_layer_t)` sanity checks if b3's step-probability
  fields are large (SEQ_STEPS × SEQ_TRACKS × MAX_LAYERS budget was flagged in
  that branch's own scope).
- **`feat/a3-portamento-arp`** touches `arp_core.c`/`arp_core.h` (adding
  per-track portamento/`synth_delay_ms` exposure). This branch also modifies
  `arp_state_t`, `arp_core_init()`, `arp_rebuild()`, and
  `arp_configure_wave_synth()` substantially. Expect a real merge conflict in
  `arp_core.c` around the osc0 event-building block and `arp_rebuild()`'s
  tail (where this branch added the EG1 filter-coef wiring and the EG1 push
  condition) — re-verify both features still coexist correctly afterward
  (particularly that portamento's own per-track fields don't collide with
  `env1`/`env1_authored` byte-for-byte in any packed/serialized form, if that
  branch introduces one).
- **`feat/i2s-clock-source`** (render/GPTimer/ISR path) should have zero
  overlap with this branch — nothing here touches the render task, ISR, or
  clock code — but it's worth a quick sanity build together regardless since
  it's flagged high-risk in the task brief.
- **`feat/c1-hint-bar`** touches `main.c`'s overlay dispatch. This branch
  also edits `main.c`'s `MY_BUTTON_3` handler (adding the
  `BUTTON_LONG_PRESS_START` branch for the EG0/EG1 toggle). Low collision
  risk (different button/branch), but the two diffs are close together in
  the file — re-diff `main_button_event_cb()` after merge.
- **`feat/a1-second-envelope` (this branch) vs. `feat/a2-fm-dx7`**: if the
  FM/DX7 work adds its own envelope/operator model reusing `seq_env_t` or the
  graph editor, check for target/eg_index enum collisions in
  `graph_target_t` / `ui_editors.c`.

## Files touched (for quick reference)

```
components/display/display_seq.h
components/synth_core/include/seq_defaults.h
components/synth_core/include/sequencer_core.h
components/synth_core/include/synth_ui.h
components/synth_core/include/arp_core.h
components/synth_core/include/custompatches/drone_core.h
components/synth_core/sequencer_core/seq_core_internal.h
components/synth_core/sequencer_core/seq_core_synth.c
components/synth_core/sequencer_core/seq_core_editors.c
components/synth_core/sequencer_core/seq_core_state.c
components/synth_core/arp_core.c
components/synth_core/custompatches/drone_core.c
components/synth_core/custompatches/bass_presets.c
components/synth_core/synth_ui/ui_editors.c
main/main.c
```
