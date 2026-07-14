# pie_dsp

ESP32-S3 PIE (128-bit SIMD) kernels for the AMY render path.

Two entry points, both hand-written Xtensa assembly:

| Symbol | Macro | Does |
|---|---|---|
| `pie_memset_s3(dest, val, nbytes)` | `PIE_BZERO(p, nbytes)` | Block fill via `EE.VST.128` |
| `pie_memcpy_s3(dest, src, nbytes)` | `PIE_BCOPY(src, dst, nbytes)` | Block move via `EE.VLD.128` / `EE.VST.128` |

Note that `PIE_BCOPY` keeps `bcopy`'s `(src, dst)` argument order, which is the reverse of `memcpy`'s.

Off the S3 both macros fall back to libc `memset` / `memcpy` (not `bzero`/`bcopy`: `<strings.h>` is POSIX-only and absent on MSVC), so anything built on top of them still compiles for desktop and other targets. The gate is `PIE_DSP_S3_ENABLED` (`include/pie_dsp_platform.h`, which only pulls in `sdkconfig.h` when it exists), shared by the C header and the preprocessed `.S` sources; the kernels assemble to nothing when it is off, so the sources can be listed unconditionally in `CMakeLists.txt`. Neither macro supports overlapping buffers on any target.

## Why only a memset and a memcpy

That is the finding, not a shortcut.

PIE multiplies and multiply-accumulates on 8- and 16-bit lanes only (`EE.VMULAS.S16` into the 40-bit QACC/ACCX). 32-bit lanes get add/sub/shift/min/max/compare and nothing else; there is no gather and no float SIMD. AMY's fixed-point build uses `SAMPLE` = s8.23 = `int32`, so every hot-path multiply (`MUL8_SS`, `SMULR6`, `top16SMUL`) is a 32x32 product PIE cannot vectorize at all.

What is left over is not vectorizable either: the LUT oscillators walk a wavetable by phase accumulator (gather-indexed), and the biquads, EQ, echo, chorus and reverb all carry IIR feedback state (recurrence-bound). esp-dsp independently reaches the same conclusion - its own ESP32-S3 biquad (`dsps_biquad_f32_aes3.S`) contains zero PIE instructions and is hand-scheduled scalar FPU.

So the eligible set is the multiply-free, dependency-free, contiguous operations: the bulk block clears and copies.

An earlier revision also vectorized AMY's `scan_max()` and `block_norm()`, which are multiply-free reductions and so looked eligible on paper. On-target A/B said otherwise - they bought nothing on any scene and cost up to ~1% on filter-heavy ones. Nearly every call is on a tiny buffer (`scan_max(w, 4)`, `scan_max(w, 6)` for LPF24, `scan_max` / `block_norm` over the 8-entry `filter_delay`), where the vector setup costs more than the scalar loop it replaces, and those buffers live inside `synthinfo` so they are not aligned either. Multiply-free turns out to be necessary but not sufficient: the operation also has to be long enough to amortise the vector setup.

## Alignment

Neither kernel has an alignment precondition - both handle a misaligned base with a scalar head. But an aligned base skips that head, and callers that hand PIE a 16-byte-aligned buffer get the full vector path. On the AMY side that is what `malloc_caps_block()` exists to provide (see `AMY-EDITS.md`).

## Inline-asm alternative (`include/pie_dsp_inline.h`, not wired up)

A second, header-only implementation of the same two operations: `pie_block_bzero()` / `pie_block_bcopy()` as gated inline asm, specialized to 16-byte-aligned whole blocks (everything else falls back to libc at runtime). Nothing in the build uses it - the `.S` kernels above are the shipping, A/B-benched path. It exists as the shape to offer upstream if platform-specific source files or vendored Apache-2.0 assembly are unwanted there: original code (no lines copied, though the aligned main loops of esp-dsp's kernels are the reference it reproduces), one header, no `.S`. Codegen verified (same `EE.VST.128` / `EE.VLD.128` zero-overhead inner loop as the kernels' aligned path); not yet A/B-benched. Trade-off: an unaligned buffer gets plain `memset`/`memcpy` instead of the kernels' scalar-head-then-vector path, so 16-byte allocation becomes required for any speedup.

## Provenance

`src/pie_memset_s3.S` and `src/pie_memcpy_s3.S` are vendored from esp-dsp (`modules/support/mem/esp32s3/dsps_memset_aes3.S` and `dsps_memcpy_aes3.S`, Apache-2.0), unmodified apart from the symbol rename and the build gate. We carry a local copy rather than depend on the whole 4.4 MB esp-dsp component for two ~300-line kernels; the rename also lets esp-dsp sit in the tree as a benchmarking reference without a duplicate-symbol clash at link time.

## Users

AMY reaches these through `components/amy/src/amy_simd.h` (`AMY_BLOCK_BZERO` / `AMY_BLOCK_BCOPY`): the per-bus `fbl` clear, the per-audible-oscillator `per_osc_fb` clear, the chorus mod buffer, and the FM operator scratch in `algorithms.c`. The FM scratch is where most of the win is - dx7 6-op polyphony renders 10.4% cheaper from the PIE routing alone (on-target A/B, median CPU cycles per rendered block, bit-identical output).
