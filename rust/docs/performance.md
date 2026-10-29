# Analysis performance

## Reproducing the measurements

Run the commands from `rust/`.
Generate the long inputs once with `uv run ../fixtures/benchmark/generate.py`.
The generator keeps existing files, and its output directory is ignored by git.
The file benchmarks skip missing inputs, while the in-memory benchmarks always run.

The five Criterion targets in `crates/core/benches/` measure different costs:

- `analysis` measures `BlockMeter`, the ebur128 components, and `analyze_file`.
- `decode` opens and drains each file without measuring loudness or building a waveform.
- `normalize` measures gain and AIFF writing in memory, then normalization of temporary file copies.
- `waveform` measures overview construction, serialization, and display column queries.
- `misc` measures cached analysis reads and record sorting.

Save a reference before editing the measurement code, then compare without replacing it:

```shell
cargo bench -p audiobatch-core --bench analysis -- --save-baseline before
cargo bench -p audiobatch-core --bench decode -- --save-baseline before
cargo bench -p audiobatch-core --bench normalize -- --save-baseline before
cargo bench -p audiobatch-core --bench waveform -- --save-baseline before
cargo bench -p audiobatch-core --bench misc -- --save-baseline before
cargo bench -p audiobatch-core --bench analysis -- --baseline before
```

Criterion stores the measurements and HTML reports in `target/criterion/`.
Throughput is audio frames per second, not channel samples per second.
The in-memory analysis and render inputs are 10 seconds long, while generated audio files are 60 seconds long.
Do not run other benchmarks, profiling captures, builds, or tests concurrently when collecting comparison results.
File comparisons include filesystem caches and metadata overhead, so they are not an additive decomposition of analysis.

Build a CLI with release optimizations and debug information for samply:

```shell
cargo build --profile profiling --bin audiobatch-rs
samply record --save-only --unstable-presymbolicate --iteration-count 8 -o target/profile-before.json.gz -- ./target/profiling/audiobatch-rs --refresh --jobs 1 --output json ../fixtures/benchmark/output/music_stereo_44100_16.wav
samply load target/profile-before.json.gz
```

`--refresh` bypasses the analysis cache, and `--jobs 1` keeps one analysis worker active.
The capture still includes startup, logging, and SQLite work.
Inspect the analysis worker rather than treating the main thread's waits as measurement CPU time.
Inlining combines several kernels into `Filter::process`, so the component benchmarks separate true peak from K-weighting.

## Reference measurements

The initial local measurements were collected on October 29, 2026, on an Apple M3 Max,
with Rust 1.99.1, LLVM 23.1.1, native CPU flags, and release LTO.
The measured Rust implementation is the one in commit `e98f0f9`, before the loudness kernel changes.
The workspace revision used for the sampling capture was `b3a0589`, whose later changes affect only C++.
These are local macOS measurements, not Windows results.
Some initial file measurements overlapped compilation and validation,
so their absolute timings are exploratory rather than controlled optimization comparisons.

The saved `before` in-memory baseline measures 10 seconds of stereo music at 44.1 kHz:

- `BlockMeter`: 34.380 ms.
- ebur128 with all measurements and short-term queries: 31.210 ms.
- True peak alone, including the implied sample peak scan: 6.579 ms.
- K-weighting and integrated gating, without short-term queries: 5.910 ms.
- K-weighting and integrated gating with short-term queries: 28.951 ms.
  This measurement has a wide 95 percent confidence interval of 25.723 to 33.586 ms.

The eight-iteration samply capture contains 1,832 analysis-worker samples:

- 60.4 percent have `Filter::calc_gating_block` as their leaf function.
  This combines the repeated short-term window scans with integrated gating scans.
- 27.4 percent have `Filter::process` as their leaf function.
  K-weighting, sample peak scanning, and the interpolator are inlined into this function.
- 8.3 percent have `BlockMeter::measure_block` as their leaf function.
- 1.9 percent have the PCM decoder's `decode_ref` as their leaf function.
- The remaining samples cover I/O, allocations, copying, tracing, and cache work.

The profile confirms that repeated energy scans, not true-peak interpolation alone, dominate this input.
The component timings separate the costs approximately, but must not be presented as mutually exclusive sampled shares.

## Exact output constraints

The golden files freeze every numeric analysis field, the short-term history and cache blobs,
the waveform, normalization gains, and normalized sample hashes.
They must not be regenerated to accept a performance change.
The CLI snapshot in `target/snapshot-before/` also freezes JSON and normalized files.
Check it with:

```shell
cargo nextest run
./scripts/snapshot.sh compare target/snapshot-before
./scripts/parity.sh --release ../fixtures/normalization
./scripts/parity.sh --release --normalize --peak 0 ../fixtures/normalization
./scripts/parity.sh --release --normalize --lufs -14 ../fixtures/normalization
```

A rolling sum of 100 ms energies changes the sample summation order.
Although its loudness error can be very small, that does not establish exact history, maximum loudness,
or cache blob equality.
The current ebur128 interface also does not expose its filtered samples or block energies,
so an external rolling sum would need another K-weighting pass or a replacement loudness filter.
That optimization is deferred while the exact golden contract remains in force.
True-peak interpolation can instead place independent phases in SIMD lanes without reordering each phase's tap sum.

## SIMD true peak

`loudness/true_peak.rs` places four independent interpolation phases in `f32x4` lanes below 96 kHz,
and duplicates the two phases into those lanes from 96 kHz to below 192 kHz.
The fixed histories expose the newest samples contiguously without allocating during processing.
Every phase keeps the reference coefficient generation, tap order, and separate multiply and add.
Ordered comparisons preserve the reference NaN policy across SIMD backends.
The interpolator includes decoded sample extrema and does not drain an interpolation tail at end of stream.
At 192 kHz and above `BlockMeter` reuses the signed extrema it already measured, without another sample scan.

The integrated and short-term reference meter does not run its implied sample peak scan or interpolation.
Every architecture uses the same SIMD true peak path, including x86, with no opt-in or reference fallback.
Ebur128 enables arithmetic flush to zero temporarily on x86 with SSE2,
while the safe SIMD kernel leaves the caller's floating point environment unchanged.
Those differ for subnormal arithmetic results even when all source samples are normal.
A focused x86 test records a subnormal overshoot that demonstrates the difference.
The SIMD implementation is enabled for direct Windows testing,
but bit equality in this underflow regime is not established by normal-amplitude fixtures.
No new unsafe code or floating point environment changes are introduced in core.

The supported hardware baseline is Apple Silicon M1 or newer with NEON and x86 with AVX2.
AVX-512 capable AMD CPUs also select the AVX2 backend, with no SSE compatibility paths.
The four phase lanes use fixed `f32x4` vectors, so backend selection does not imply 256-bit lane packing.
Wider channel packing is separate optimization work and must preserve the same per-phase tap order.
The kernel tests compare each streaming snapshot against ebur128 with the selected NEON or AVX2 backend
and a forced scalar fallback.
They cover both sides of the 96 and 192 kHz boundaries, specialized and generic channel counts,
irregular and empty chunks, wrapped histories, NaNs, infinities, signed zeros, and short streams.
Underflowing interpolation is checked exactly on targets where the reference does not enable x86 flush to zero.
The unchanged golden and differential tests also compare the integrated `BlockMeter` outputs.

## Local verification

The local macOS run passes all 637 workspace tests with none skipped, including the frozen golden files,
the ebur128 differential harness, the forced scalar and NEON backends, and the sample-only extrema regression.
The saved CLI snapshot matches exactly for analysis, peak normalization, and loudness normalization JSON.
Its 56 peak-normalized and 55 loudness-normalized outputs have identical samples and tags.
C++ parity passes for 32 analysis records, 29 peak-normalized outputs, and 28 loudness-normalized outputs,
with only the existing accepted decoder differences for lossy formats.
No golden files or saved pre-optimization snapshots were regenerated.

`cargo clippy --fix --allow-dirty --workspace --exclude audiobatch-gui --all-targets`,
`cargo clippy --workspace --exclude audiobatch-gui --all-targets -- -D warnings`, Rust formatting,
and the changed prose checks pass.
The required full workspace Clippy fix command completes with one existing `too_many_lines`
warning in the concurrently edited GUI constructor, outside the measurement changes.
The full workspace tests and formatting pass with the final NEON and AVX2 dispatch.

## Local SIMD measurements

The final NEON and AVX2 dispatch was measured on the same M3 Max after validation finished,
without concurrent builds, tests, or profiling captures.
These figures describe local macOS performance only.
The `before` baseline remains unchanged, and the earlier `simd-true-peak` baseline checks dispatch overhead.

For 10 seconds of stereo input, the final true peak benchmark measures:

- 44.1 kHz: 1.984 ms SIMD and 6.628 ms ebur128, about 3.34 times faster.
- 48 kHz: 2.172 ms SIMD and 7.271 ms ebur128, about 3.35 times faster.
- 88.2 kHz: 3.972 ms SIMD and 13.418 ms ebur128, about 3.38 times faster.
- 96 kHz: 9.870 ms SIMD and 17.590 ms ebur128, about 1.78 times faster.
- 192 kHz: 2.111 ms for the standalone sample scanner and 4.053 ms ebur128.
  The standalone scanner regresses about 77 percent against the earlier SIMD dispatch baseline.
  Production `BlockMeter` does not call that scanner at these rates, it reuses its signed extrema instead.
- Mono 44.1 kHz: 0.988 ms SIMD and 2.403 ms ebur128, about 2.43 times faster.

The interpolating stereo kernels have no meaningful regression against the earlier SIMD dispatch baseline.
The final `BlockMeter` measurements and Criterion time changes against `before` are:

- Stereo 44.1 kHz: 31.007 ms, about 11.0 percent less time.
- Stereo 48 kHz: 32.371 ms, about 38.5 percent less time.
- Stereo 88.2 kHz: 61.529 ms, about 14.5 percent less time.
- Stereo 96 kHz: 72.712 ms, about 9.3 percent less time.
- Stereo 192 kHz: 125.980 ms, no statistically significant change.
- Mono 44.1 kHz: 15.088 ms, about 19.5 percent less time.

All eleven generated file analysis benchmarks improve against the saved baseline.
At 44.1 kHz, music WAV, FLAC, MP3, AAC, and Ogg take about 11 to 17 percent less time,
and the mono WAV takes about 12 percent less time.
The higher rate WAV comparisons report about 18 percent less time at 48 kHz,
15 percent at 88.2 kHz, 11 percent at 96 kHz, and 6 percent at 192 kHz.
White noise FLAC at 44.1 kHz takes about 16 percent less time.
The initial baseline includes the measurement caveats above,
so these file comparisons are directional evidence rather than isolated kernel speedups.
The unusually large 48 kHz in-memory comparison also should not be generalized to every workload.

## Remaining validation

Windows measurements and execution of the new kernels on x86 SIMD levels require a Windows machine or CI.
The production core library passes `cargo clippy --lib --target x86_64-pc-windows-msvc -- -D warnings`
with the SIMD path enabled and portable `RUSTFLAGS`.
This local type-check uses `LIBSQLITE3_SYS_USE_PKG_CONFIG=1 PKG_CONFIG_ALLOW_CROSS=1` to avoid compiling bundled SQLite.
It does not link or execute a Windows binary,
and all-target checking is blocked by the missing Windows C headers needed by Criterion's `alloca` dependency.
Cross-compilation alone does not establish output equality or speed on that machine.
The x86 analysis path runs the SIMD kernel directly, so Windows validation needs no feature flag or code change.
Its arithmetic underflow difference remains explicit and must not be hidden by regenerating golden outputs.
The larger K-weighting and gating replacement, rolling short-term sums,
and normalization SIMD work must each pass the same safety net
and get their own controlled benchmark comparison.
