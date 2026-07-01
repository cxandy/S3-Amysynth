# A4 — Wavetable Oscillator (`wave=WAVETABLE`)

Branch: `feat/a4-wavetable`

## Summary

AMY's built-in 64-cycle wavetable oscillator (`wave=WAVETABLE`, `oscillators.c`
`render_wavetable()`) was already fully implemented in the vendored AMY source
but sat behind an unwired `#ifdef AMY_WAVETABLE` — no build path ever defined
that macro, so the feature was dead code. This change:

1. Wires the flag through Kconfig + CMake, measures its footprint, and leaves
   it on by default.
2. Exposes wavetable selection through the existing patch-cycle UI (melodic
   layers, arp, drone) as five new virtual patches — no new screen.
3. Adds a `SCAN` LFO modulation target (AMY `duty`) to the existing
   native-LFO infrastructure (arp WAVE mode, melodic wave-patch tracks), the
   same mechanism filter cutoff/amp/pitch already use.

## 1. Build-flag spike (flash/PSRAM footprint)

- `components/amy/Kconfig` — new `AMY_WAVETABLE` bool, **default y**, with the
  measured cost written into the help text.
- `components/amy/CMakeLists.txt` — `if(CONFIG_AMY_WAVETABLE) target_compile_definitions(${COMPONENT_LIB} PUBLIC AMY_WAVETABLE)`, mirroring the existing `AMY_PROFILE_*` pattern in the same file.

**Measured, on-target-equivalent (esp32s3, 16 MB flash), before vs. after
enabling the flag, everything else held constant:**

| | before | after | delta |
|---|---|---|---|
| Flash `.rodata` | 353,954 B | 517,906 B | **+163,952 B** (5 × 16384 samples × 2 B) |
| Flash `.text` | 281,366 B | 282,214 B | +848 B (new `case`/dispatch, negligible) |
| DIRAM | 90,569 B | 90,569 B | **0** |
| IRAM | 16,384 B (100%) | 16,384 B (100%) | **0** |
| Total image | 726,845 B | 891,645 B | +164,800 B |

Confirmed by code inspection, not just measurement: `pcm_get_sample_ram_for_preset()`
(`components/amy/src/pcm.c:77`) returns `(int16_t*)pcm + offset` — a pointer
straight into the flash `const int16_t pcm[]` array (`PROGMEM` is a no-op
macro on this platform, `amy.h:352`) — never `malloc`'d or copied into
internal RAM/PSRAM. `render_wavetable()` reuses the existing `render_lut()`
hot path (no new IRAM function). Net: **flash-only cost, ~160 KB out of 16 MB,
zero RAM/PSRAM/IRAM impact.** Given the smallest app partition is still >80%
free after the addition, `default y` is justified — verified in AMY-EDITS.md.

## 2. Wavetable-scan-as-mod-target (native-LFO infrastructure)

`components/display/display_seq.h:68-72` — added `LFO_TARGET_SCAN` to the
shared `lfo_target_t` enum (5th target, before `LFO_TARGET_COUNT`; the UI
cycling and the `lfo_view_signature()` bit-packing are both already generic
over `LFO_TARGET_COUNT`, so no other change was needed to make it selectable
and displayed — `components/display/display_lfo.c:36` adds the `"SCN"` label).

Wired into the two existing native-LFO switch statements (mirrors
`LFO_TARGET_FILTER`/`AMP`/`PITCH`, `d = depth/100`):

- `components/synth_core/arp_core.c:231` — arp WAVE mode, osc 0:
  `e->duty_coefs[COEF_MOD] = d * 0.5f;`
- `components/synth_core/sequencer_core/seq_core_editors.c:54` — melodic
  wave-patch tracks (`CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO`), same line.

This reuses AMY's existing generic combo-coef machinery for `duty`
(`duty_coefs[NUM_COMBO_COEFS]`, already fully wired end-to-end in
`amy.c:1593`/`combine_controls()` — nothing new needed there), the same way
`filter_freq_coefs`/`amp_coefs` are driven today. `duty` is **not**
wavetable-specific in AMY — it is also the PWM width for `wave=PULSE` — so
`SCAN` is left ungated by `AMY_WAVETABLE` and stays generically useful even
with the flag off; the name and comment call out its two meanings.

**Not done:** a target-selector for the drone. The drone's own stutter LFO
(`osc1`, PULSE) is a bespoke, fixed-purpose amp modulator
(`drone_core.c:280`, `mod_source=1` → osc0 `amp_coefs[COEF_MOD]`), not an
instance of `seq_lfo_t`/`lfo_target_t` — `synth_ui_lfo_open()` explicitly
returns early for `UI_MODE_DRONE` (no free LFO editor exists for it at all,
pre-existing). Adding a generic target selector to the drone is a
separate, larger design change (its amp math is already a bespoke dB/Peak-Duck
model — see `AMY-EDITS.md`/`drone-param-model`); out of scope here. The
drone still gets a fully audible, selectable WAVETABLE carrier (below) — just
without a scan-depth knob riding its stutter LFO.

## 3. UI/preset surface — reusing the existing wave-patch cycle

No new screen. Wavetable banks are exposed as five new **virtual patches**
(same mechanism as the existing `SEQ_PATCH_SINE..KS` raw-wave virtual patches
and `SEQ_PATCH_BASS_1..3` multi-osc presets), reachable through the
already-existing patch-cycle control on melodic layers, arp, and drone.

- `components/synth_core/include/sequencer_core.h:42-56` — new range
  `SEQ_PATCH_WAVETABLE_BASE..MAX` (267-271, one per built-in table: 111.WAV,
  BRAIDS01.WAV, PPG_WA00.WAV, SINE2SAW.WAV, VIRAL.WAV), gated
  `#if CONFIG_AMY_WAVETABLE`. Kept as its **own** range above the bass presets
  (not inserted into `SEQ_PATCH_WAVE_BASE..MAX`) so nothing downstream had to
  renumber. New shared helper `sequencer_core_is_wave_patch()` replaces four
  duplicated `>= WAVE_BASE && <= WAVE_MAX` checks and now covers both ranges
  in one place.
- `components/synth_core/sequencer_core/seq_core_synth.c:96-145` —
  `sequencer_configure_melodic_wave_track()` (shared by melodic layers and
  arp's `sequencer_core_arp_configure()`) sets `wave=WAVETABLE` +
  `preset=pcm_wavetable_base+index` for the new range, alongside the existing
  `SINE..KS` table lookup.
- `components/synth_core/custompatches/drone_core.c:234-345` —
  `drone_configure_wave_synth()` gained an `int16_t wt_preset` parameter
  (`-1` = not applicable, a no-op); the drone's PATCH-mode virtual-patch
  branch in `drone_rebuild()` now recognizes 267-271 the same way it already
  recognized 257-261.
- `components/synth_core/synth_ui/ui_patch_cycle.c` — the shared full-range
  ceiling (`SEQ_PATCH_FULL_MAX`) and curated shortlist
  (`s_melodic_patch_cycle[]`) both extended to include 267-271, so both browse
  modes (`CONFIG_SEQ_PATCH_BROWSE_FULL_RANGE` on or off) reach them from
  melodic layers and arp.
- `components/display/patch_names.c:21-26,305-311` — five new display names
  ("Wavetable: 111", "Wavetable: Braids01", ...), gated the same way.

**Design decision — drone needs its own patch-cycle domain.** The drone
excludes NOISE/KS/bass (262-266) from its patch range (its excitation model
doesn't suit them) by snapping any value landing there back down to
TRIANGLE (`drone_set_patch()`). Reusing the shared `s_melodic_domain` for
drone cycling meant that snap-back made 267-271 **unreachable**: stepping
forward from TRIANGLE always re-lands on the same excluded value, which
snaps right back — the domain's "current" position never advances past 261.
Fixed by giving the drone a dedicated list-mode `patch_domain_t`
(`s_drone_patch_cycle[]` in `ui_patch_cycle.c`, gated
`#if CONFIG_AMY_WAVETABLE` so behavior with the flag off is byte-identical to
before) that only ever contains drone-valid patches, keeping forward/backward
cycling monotonic. Note: the *pre-existing* full-range-mode gap for
NOISE/KS/bass (which already existed before this change, independent of
`AMY_WAVETABLE`) is not fixed — only the newly-introduced unreachability of
267-271 is in scope here.

**Design decision — `#if CONFIG_AMY_WAVETABLE` vs. bare `#ifdef AMY_WAVETABLE`.**
Inside `components/amy/src/` (vendored/upstream), the bare macro is kept —
that's the pre-existing upstream convention and it's defined there via the
`PUBLIC` compile-definition in `CMakeLists.txt`. Everywhere else (`synth_core`,
`display`), I use the Kconfig-generated `CONFIG_AMY_WAVETABLE` instead: the
`display` component does not depend on `amy` at all (no `REQUIRES amy` in its
`CMakeLists.txt`), so the amy component's `PUBLIC` compile-definition would
never propagate there — `patch_names.c` needs to see the flag to size its
name table. `CONFIG_*` symbols are globally visible via ESP-IDF's forced
`-include sdkconfig.h`, independent of the component dependency graph, so
using it consistently in all of *my* new code (not amy's vendored files) sidesteps
that propagation gap entirely rather than half-relying on it.

**Not done:** no dedicated raw-`wave` cycle entry for WAVETABLE in
`ui_screen_arp.c` (`ARP_CUR_WAVE`) or drone's `DRONE_SRC_WAVE` (`drone_set_wave`
cycle in `ui_screen_drone.c`). Both of those cycle a single `uint16_t wave`
enum value with no accompanying "which preset" slot — wavetable needs a
`preset` index alongside the wave constant, which doesn't fit that model
without adding new persisted state. Routing wavetable exclusively through the
PATCH-mode virtual-patch mechanism (which already carries a natural index via
the patch number offset) avoids adding a new per-voice preset field to
`arp_state_t`/`drone_state_t` for a single-session feature slice. If a later
task wants "raw WAVE mode + wavetable" (bank chosen independently of the
carrier-wave cycle), that's the extension point to revisit.

## Build/verification evidence

Environment note (read before judging the build log below): this sandboxed
worktree's CMake 4.0.3 + this ESP-IDF release combination fails
`CheckIPOSupported`'s isolated CXX probe unconditionally. The isolated
`_CMakeLTOTest-CXX` try_compile does not inherit `CMAKE_TOOLCHAIN_FILE`, so
`toolchain.cmake`'s response-file flags resolve to `@"/cxxflags"` (empty
`IDF_TOOLCHAIN_BUILD_DIR`) and the CXX compile fails, aborting *every* configure
in *every* sibling worktree before any of my source changes are even read (confirmed:
none of the 12 other parallel worktrees at `.claude/worktrees/*` have a completed
`.elf` either, and the failure reproduces on a from-scratch checkout with zero
edits). This is a pre-existing local-environment/toolchain-version defect, not
something introduced by this feature, and not fixable by editing tracked
project files (`CMakeLists.txt:9-10` calls `cu_gcc_lto_set()`, whose default
behavior is what's broken — the actual defect is in the component-manager-fetched
`managed_components/espressif__cmake_utilities/gcc.cmake`, which is untracked
and regenerated per checkout, so it cannot be fixed persistently from here).

**Workaround used for local verification only:** `CONFIG_CU_GCC_LTO_ENABLE` was
toggled off in this worktree's generated (untracked, never committed)
`sdkconfig` purely to get past the broken CMake probe and exercise the C
compiler on every changed file. This is **not** a claim that the feature was
verified with LTO on — it could not be, in this environment, regardless of
which feature branch is being built. A human running this on the main
worktree (where the MCP build tooling / a working toolchain state already
exists) should re-verify with LTO on before merging; the C-level changes here
are ordinary conditional compilation with no LTO-sensitive constructs.

Commands run, in this worktree:

```
source /home/fatta/esp-idf/S3-Amysynth/export.sh
cd .claude/worktrees/a4-wavetable
idf.py build      # AMY_WAVETABLE=y (default) — PASSED
idf.py build      # AMY_WAVETABLE=n (flag disabled) — PASSED, confirms clean gating
idf.py build      # AMY_WAVETABLE=y again (final state) — PASSED
idf.py size        # footprint numbers above
```

Both configurations (`AMY_WAVETABLE` on and off) built with zero errors and
zero new warnings; the one warning present (`synth_ui_state.c:7: 'TAG' defined
but not used`) is pre-existing and unrelated to this change.

## Deferred / not done

- Drone scan-depth modulation (see design decision above) — drone has no
  generic LFO-target infrastructure to hang it on; adding one is a separate,
  larger feature.
- Raw-`wave`-cycle WAVETABLE entries for arp/drone (see design decision
  above) — would need a new persisted "which bank" field; deferred in favor
  of the zero-new-state PATCH-mode route.
- Tempo-sync note: `LFO_TARGET_SCAN`'s depth is applied once per
  `arp_configure_wave_synth()`/`melodic_configure_native_lfo_track()` call,
  same as the pre-existing FILTER/AMP/PITCH targets — no new limitation
  introduced.
- No hardware audition — this was a build/wiring verification pass only (no
  device access in this environment); a human should confirm on-target that
  selecting a wavetable patch is audible and that `SCAN` audibly sweeps.

## Potential merge pitfalls

- **`feat/a2-fm-dx7`** — likely touches the same virtual-patch numbering
  space (`SEQ_PATCH_*` constants in `sequencer_core.h`) and possibly
  `patch_names.c`/`ui_patch_cycle.c` if it adds its own virtual-patch range
  for FM/DX7 operator patches. If it also appends after `SEQ_PATCH_BASS_MAX`,
  the two branches will collide on `267`; whichever merges second should
  renumber to stack after the other's range (both ranges are deliberately
  designed to append, not interleave).
- **`feat/a1-second-envelope`** — touches `ui_editors.c`'s `graph_target_t`
  and the ADSR/envelope screen. I did not touch `graph_target_t` (a separate
  enum from `lfo_target_t`), but both land in the same `ui_editors.c` file;
  expect a straightforward textual merge, not a semantic conflict.
- **`feat/b3-step-probability`** — adds fields to `seq_layer_t` /
  per-step arrays in `display_seq.h`. My only change to that header is
  appending `LFO_TARGET_SCAN` to `lfo_target_t` (an enum, not a struct field),
  so it should merge cleanly, but both branches touch `display_seq.h` — worth
  a diff-review pass together.
- **`feat/b1-mute-solo`** — adds a `sequencer_core` layer struct field and a
  TrackOpts screen; also touches `sequencer_core.h`/`sequencer_core.c`. My
  changes there are additive (`SEQ_PATCH_WAVETABLE_*` defines,
  `sequencer_core_is_wave_patch()`), low collision risk, but re-check that no
  new field lands at the same struct offset assumptions.
- **`feat/a4-wavetable` vs. `feat/melodic-native-lfo`** (already-merged,
  visible in this tree as `CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO`) — no conflict
  expected since that work already landed on `main`; this branch builds on
  top of it directly (the native-LFO switch statements I extended are that
  feature's code).
- Any branch that reuses `patch_number`/`patch` as a bitfield, fixed-width
  serialized value, or NVS-persisted preset (I did not find one, but did not
  exhaustively audit) should confirm 271 still fits — current storage is
  plain `uint16_t` everywhere touched.
