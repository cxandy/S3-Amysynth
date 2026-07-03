# Integration report — all 9 feature branches merged

`integration/all-features`, built from `main` (317d400), merges all 9
feature branches from the parallel autonomous workflow in this order:

1. `feat/b1-mute-solo`
2. `feat/b3-step-probability`
3. `feat/a1-second-envelope`
4. `feat/a3-portamento-arp`
5. `feat/a4-wavetable`
6. `feat/a2-fm-dx7`
7. `feat/a5-pcm-sampling`
8. `feat/c1-hint-bar`
9. `feat/i2s-clock-source`

**This branch is a reviewable staging area, not a merge to `main`.** Nothing
was pushed. `main` and all 9 original feature branches are untouched.

**Build status: verified.** `idf.py build` succeeds (esp32s3, `S3-Amysynth.bin`
generated, 14% flash free). LTO was disabled for verification only, in the
local untracked `sdkconfig` — see "Environment issue" below; `sdkconfig.defaults`
(tracked) is untouched and still specifies LTO on. **A human should re-run
with LTO enabled on a working toolchain before trusting this build fully.**

Order was chosen to cluster related concerns (sequencer data-model changes
first, then arp/engine changes, then patch/UI-shell changes, then the
highest-risk clocking change last) so each merge's conflicts were resolved
against an already-settled base rather than compounding.

## Real conflicts found and how they were resolved

Every one of these was found by actually attempting the merge and reading
the conflicting code — not inferred from the branches' own handoff docs
(several of which flagged the *files* at risk but not the exact semantics).

### 1. `sequencer_emit_step()` guard — b1 × b3 (`seq_core_engine.c`)
Both branches added a condition to the same early-return guard: b1 added
"track muted/soloed-out", b3 added "step is decorated (has
probability/ratchet/conditional data)". Resolved as a straight OR — either
condition cancels the plain periodic tag pair.

### 2. Mute/solo silently bypassed for decorated steps — b1 × b3, found while resolving #1
Not a textual conflict — a real behavior gap neither branch could see in
isolation. `seq_core_trig.c`'s decorated-step ratchet scheduler
(`sequencer_core_service_tick()`) fired notes directly with no
`sequencer_track_audible()` check, so a muted/soloed-out track with a
probability/ratchet step would still sound. Fixed by gating the ratchet
schedule call on audibility, while leaving probability/condition evaluation
and `s_track_last_played` history unaffected by mute — so a track resumes
its exact rhythmic position when unmuted instead of freezing.

This required exposing `sequencer_track_audible()` (previously `static` in
`seq_core_engine.c`) via `seq_core_internal.h` — a second, separate bug
(implicit-declaration compile error) that **only the real `idf.py build`
caught**, not the merge itself. Fixed in a follow-up commit
(942b0c0).

### 3. Patch-number collision — a4 × a2 (`sequencer_core.h`, `ui_patch_cycle.c`, `patch_names.c`)
The most serious issue found. a4 (wavetable) and a2 (FM/DX7) each
independently claimed virtual patch IDs **267–271** for 5 new patches, with
no way to know about each other. Resolved by renumbering FM/ALGO to
**272–276**, unconditionally (not gated by `CONFIG_AMY_WAVETABLE`), so the
two ranges can never collide regardless of build config — `SEQ_PATCH_ROUTABLE_MAX`
/ `SEQ_PATCH_FULL_MAX` now derive from the FM range since it's always the
true ceiling.

While fixing this, caught a second latent bug this same renumbering would
otherwise have introduced: `patch_names.c`'s name table is a **positional**
array (not designated-index), so with `CONFIG_AMY_WAVETABLE` off, the FM
names would have silently shifted down to fill the gap at patch 267 instead
of 272 — permanently desyncing on-screen names from the actual patch numbers
in that build config. Fixed by adding explicit `"(reserved)"` placeholder
entries for 267–271 when wavetable is compiled out, so FM always lands at
its fixed 272–276 index either way.

### 4. Mechanical unions (no semantic risk)
CMakeLists.txt source-file lists (×3 merges), `synth_ui_internal.h`
declarations, `synth_ui_task.c`'s view enum / switch / priority-chain
insertions, `seq_core_synth.c`'s `is_wave_patch` check, `arp_core.c`'s
independent post-reconfigure reasserts (EG1 push × portamento push) — all
independent additions at different program points, unioned and verified by
reading each resolved file end to end, not just trusting a clean auto-merge.

### 5. `ui_screen_menu.c` — a2 × a5, auto-merged, verified safe
Both branches add entries to the same plain C `enum { ... }` of menu item
IDs. This auto-merged with no git conflict. Verified safe (unlike #3)
because enum members here are only ever referenced symbolically, never as
hardcoded numeric literals elsewhere — sequential auto-renumbering by
declaration order is correct by construction.

### 6. Hint bar completeness gap — c1 × b3/a1, found while merging c1
Not a conflict — `feat/c1-hint-bar` branched before `feat/b3-step-probability`'s
Step Trig popup existed, so `synth_ui_hint.c`'s `MY_BUTTON_2` hint fell
through to the plain-sequencer default ("Pitch") while the Step Trig popup
actually owns the encoder and suppresses that gesture (confirmed against
`main.c:462-473`). Added a `synth_ui_stepedit_is_active()` check → "Close".
Also corrected a doc comment in the same file claiming "no screen-specific
overrides exist" for `MY_BUTTON_3`, no longer true after `feat/a1-second-envelope`'s
EG0/EG1 long-press toggle (display label left as-is — the hint already
collapses other buttons' long-press nuance the same way). Audited every
`synth_ui_fm_is_active()` call site in `main.c`: FM has no
`MY_BUTTON_1`/`2`/`3`-specific behavior, only `MY_BUTTON_ENC` and raw encoder
rotation (both already outside this hint's scope), so no FM entry was needed.

### 7. Drone EG1 — pre-existing doc inaccuracy, not a merge defect
`feat/a1-second-envelope`'s own handoff doc claims "drone was deliberately
left unwired to EG1." The actual code (present before this integration,
unaffected by any merge here) already wires `sequencer_core_push_envelope_eg1()`
into `drone_core.c`, gated by `env1_authored` — the same safe, opt-in pattern
used everywhere else. Not a bug, just a stale claim in that branch's own doc;
noted here so a reviewer doesn't go looking for missing plumbing that already
exists.

## Environment issue affecting every branch (not a merge defect)

All 9 branches independently hit, and this integration reproduced directly:
a pre-existing CMake 4.0.3 / `cmake_utilities` bug where a fresh build
configure fails LTO detection (`GCC link time optimization(LTO) is not
supported`, `managed_components/espressif__cmake_utilities/gcc.cmake:53`).
Confirmed genuine (not fabricated) by reproducing it independently. Worked
around for all verification builds by disabling `CONFIG_CU_GCC_LTO_ENABLE`
in the local, untracked, generated `sdkconfig` only — `sdkconfig.defaults`
(tracked, LTO on) was never touched. **A real LTO-enabled build of this
integration branch has not been done and should happen before merging to
`main`.**

## What a human should check before merging any of this to `main`

1. **Re-run a full LTO-enabled build** (e.g. via the MCP `build_project` flow
   in the main worktree, which has a working cached LTO configuration) —
   every verification in this pass, on every branch and in this integration,
   was LTO-off.
2. **`feat/i2s-clock-source`** — highest-risk item (clocking change per
   project policy). Kconfig-gated OFF by default (confirmed:
   `CONFIG_RENDER_CLOCK_I2S_ENABLE` defaults `n` and is unset in this build).
   Needs the scope/logic-analyzer verification its own handoff doc already
   flags before ever flipping the default.
3. **On-hardware audio/visual pass** — none of this was flashed or heard;
   several individual handoff docs flag ears-on-hardware items (FM preset
   tuning, wavetable audibility, BASS_2's EG1 targeting the wrong oscillator,
   the hint bar's bottom-strip pixel overlay on dense screens).
4. **`main.c:298-405` dead code** (found by the c1 agent, confirmed still
   present) — Track Options' `MY_BUTTON_1` → add-layer binding is
   unreachable because the unconditional top-level B1 guard always returns
   first. Not fixed here (out of scope), flagged for the maintainer.
5. Re-read each branch's own `docs/handoff/<name>.md` for feature-specific
   deferred work — this report only covers *cross-branch* integration
   issues, not each feature's individual completeness.

## Branch/worktree map (all still present, untouched, unpushed)

| Branch | Worktree |
|---|---|
| `feat/b1-mute-solo` | `.claude/worktrees/b1-mute-solo` |
| `feat/c1-hint-bar` | `.claude/worktrees/c1-hint-bar` |
| `feat/a1-second-envelope` | `.claude/worktrees/a1-second-envelope` |
| `feat/b3-step-probability` | `.claude/worktrees/b3-step-probability` |
| `feat/a2-fm-dx7` | `.claude/worktrees/a2-fm-dx7` |
| `feat/a4-wavetable` | `.claude/worktrees/a4-wavetable` |
| `feat/a5-pcm-sampling` | `.claude/worktrees/a5-pcm-sampling` |
| `feat/a3-portamento-arp` | `.claude/worktrees/a3-portamento-arp` |
| `feat/i2s-clock-source` | `.claude/worktrees/i2s-clock-source` |
| `integration/all-features` (this branch) | `.claude/worktrees/integration-all-features` |
