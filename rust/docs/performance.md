# Analysis performance

## Reproducing the measurements

Run the commands from `rust/`.
Generate the long inputs once with `uv run ../fixtures/benchmark/generate.py`.
The generator keeps existing files, and its output directory is ignored by git.
The file benchmarks skip missing inputs, while the in-memory benchmarks always run.

The five Criterion targets in `crates/core/benches/` measure different costs:

- `analysis` measures `BlockMeter`, SIMD true peak, ebur128 reference components, and `analyze_file`.
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
The historical ebur128 profile below inlines several kernels into `Filter::process`.
The current `BlockMeter` benchmark includes custom K weighting and gating,
while component benchmarks isolate true peak and the development-only reference.

## Historical reference measurements

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

## Custom meter baseline

These are historical measurements from before the custom loudness meter and in-render output measurement.
They are retained as the comparison baseline, not results for the current implementation.

The October 30, 2026 baseline uses an Apple M3 Max with 16 logical CPUs and 64 GiB of memory,
macOS 27.2 on arm64, and Rust 1.99.1.
Rust uses the release profile with LTO, one code generation unit, and `target-cpu=native`.
The C++ CLI uses the `macos-release` CMake preset with `-O3 -DNDEBUG`.
The loudness measurement still uses ebur128 0.1.11 at this baseline.

Run `./scripts/cli_benchmark.sh` to measure both release CLIs with alternating hyperfine runs.
The default selection combines the generated long inputs with the normalization corpus.
Use `--corpus ../fixtures/normalization` for the smaller CI-sized comparison.
The script writes raw timing samples, combined JSON and Markdown, source and binary hashes,
and machine and build metadata into a unique directory under `target/cli-benchmark/`.
Copies and configuration are prepared outside timing, and filesystem caches are warm.
Normalization measures the full CLI workflow, including initial analysis, rendering, output measurement, and validation.
At this baseline, output measurement requires a full decode of the written file.
Every run must pass the existing correctness oracles, independently of its timing.
The CI reporting step is non-failing and uses no performance threshold.

The `custom-meter-before` Criterion baseline was saved before the loudness implementation changed.
For 10 seconds of stereo music at 44.1 kHz, and the corresponding 60 second WAV for file benchmarks:

- `meter/block_meter/44100_2ch`: 27.982 ms, 95 percent interval 27.928 to 28.047 ms.
- `analyze_file/music_stereo_44100_16.wav`: 175.52 ms, interval 174.64 to 176.60 ms.
- `render/gain_only`: 21.801 microseconds, interval 21.726 to 21.855 microseconds.
- `render/gain_and_aiff/16`: 2.2577 ms, interval 2.2414 to 2.2750 ms.
- `render/gain_and_aiff/24`: 3.3613 ms, interval 3.3256 to 3.3941 ms.
- `normalize_file/peak/music_stereo_44100_16.wav`: 221.83 ms, interval 220.71 to 222.91 ms.
- `normalize_file/lufs/music_stereo_44100_16.wav`: 220.69 ms, interval 219.94 to 221.78 ms.

Save and compare these cases with:

```shell
cargo bench -p audiobatch-core --bench analysis -- 'meter/block_meter/44100_2ch|analyze_file/music_stereo_44100_16.wav' --save-baseline custom-meter-before --noplot
cargo bench -p audiobatch-core --bench normalize -- 'render/|normalize_file/(peak|lufs)/music_stereo_44100_16.wav' --save-baseline custom-meter-before --noplot
```

The release CLI baseline uses 29 eligible files from `fixtures/normalization`,
with three measured runs per command, one warmup, and no concurrent compilation or tests.
The three known failure fixtures are excluded from timing, not from parity verification.
Means and sample standard deviations, in seconds, are:

- Analysis, one worker: C++ 1.610621 ± 0.012303, Rust 0.852869 ± 0.007723, C++/Rust 1.888.
- Peak normalization, one worker: C++ 3.325175 ± 0.023885, Rust 2.040939 ± 0.026699, C++/Rust 1.629.
- Loudness normalization, one worker: C++ 3.252798 ± 0.018845, Rust 2.012160 ± 0.001571, C++/Rust 1.617.
- Analysis, 16 workers: C++ 0.302230 ± 0.001993, Rust 0.137665 ± 0.002688, C++/Rust 2.195.
- Peak normalization, 16 workers: C++ 0.592095 ± 0.005922, Rust 0.435082 ± 0.009631, C++/Rust 1.361.
- Loudness normalization, 16 workers: C++ 0.593568 ± 0.004899, Rust 0.448726 ± 0.005761, C++/Rust 1.323.

All analysis and normalized-output correctness checks pass for every measured pair.
The raw report is `target/cli-benchmark/20261030T123706Z-EwC7zE/summary.md`, with adjacent JSON and binary hashes.
The Rust binary identifies revision `e5eca33`, and C++ identifies `704ae4b`.
An exploratory combined-corpus run also measured analysis at 21.969 s for C++ and 3.333 s for Rust with one worker,
but its generated AAC normalization fails the existing lossy decoder tolerance, so it is not a complete baseline.
The tolerance is not relaxed to make that benchmark pass.

## Optimized custom meter results

The optimized run uses the same machine, compiler, native CPU flags, release LTO,
and unchanged `custom-meter-before` Criterion reference as the baseline above.
Measurements run sequentially, without concurrent builds, tests, or profiling captures.
The benchmarked implementation includes the custom meter, waveform extrema reuse,
in-render output measurement, fused gain and quantization, and the at-target fast path.

For the same 10 second in-memory signal and 60 second WAV:

- `meter/block_meter/44100_2ch`: 5.9701 ms, 95 percent interval 5.9556 to 5.9906 ms.
  The baseline is 27.982 ms, about 4.69 times faster and 78.7 percent less time.
- `analyze_file/music_stereo_44100_16.wav`: 41.396 ms, interval 41.275 to 41.510 ms.
  The baseline is 175.52 ms, about 4.24 times faster and 76.4 percent less time.
- `render/gain_only`: 21.840 microseconds, interval 21.706 to 22.057 microseconds.
  Criterion detects no significant change, as expected for the unchanged scalar helper.
- `render/gain_and_aiff/16`: 2.1915 ms, interval 2.1822 to 2.2056 ms.
  The fused writer benchmark takes about 2.6 percent less time than the baseline.
- `render/gain_and_aiff/24`: 3.2381 ms, interval 3.2310 to 3.2463 ms.
  The fused writer benchmark takes about 3.0 percent less time than the baseline.
- `normalize_file/peak/music_stereo_44100_16.wav`: 84.876 ms, interval 84.551 to 85.178 ms.
  The baseline is 221.83 ms, about 2.61 times faster and 61.6 percent less time.
- `normalize_file/lufs/music_stereo_44100_16.wav`: 84.826 ms, interval 84.364 to 85.417 ms.
  The baseline is 220.69 ms, about 2.60 times faster and 61.5 percent less time.

The render microbenchmark now calls the production fused writer with a no-op sample observer,
rather than a separately scaled copy and plane-reference allocation.
It excludes `BlockMeter`, so only the end-to-end normalization benchmark measures output metering.
The end-to-end Criterion normalization cases start from a current source record,
whereas CLI normalization also includes initial source analysis.
Their speedups must not be treated as interchangeable measures of the same operation.

The release CLI comparison uses the same 29 eligible normalization corpus files,
three measured runs per command, one warmup, and worker counts of 1 and 16 as the baseline.
Means and sample standard deviations, in seconds, are:

- Analysis, one worker: C++ 1.618250 ± 0.014608, Rust 0.297451 ± 0.007325, C++/Rust 5.440.
  Rust takes about 65.1 percent less time than its 0.852869 s baseline.
- Peak normalization, one worker: C++ 3.277709 ± 0.054053, Rust 0.884520 ± 0.011951, C++/Rust 3.706.
  Rust takes about 56.7 percent less time than its 2.040939 s baseline.
- Loudness normalization, one worker: C++ 3.270517 ± 0.040099, Rust 0.928473 ± 0.023419, C++/Rust 3.522.
  Rust takes about 53.9 percent less time than its 2.012160 s baseline.
- Analysis, 16 workers: C++ 0.310800 ± 0.003239, Rust 0.056582 ± 0.001800, C++/Rust 5.493.
  Rust takes about 58.9 percent less time than its 0.137665 s baseline.
- Peak normalization, 16 workers: C++ 0.594622 ± 0.011371, Rust 0.285083 ± 0.002519, C++/Rust 2.086.
  Rust takes about 34.5 percent less time than its 0.435082 s baseline.
- Loudness normalization, 16 workers: C++ 0.595915 ± 0.019191, Rust 0.293995 ± 0.004707, C++/Rust 2.027.
  Rust takes about 34.5 percent less time than its 0.448726 s baseline.

Every measured pair passes the level, normalization-target, sample, and metadata oracles.
The report and raw exports are in `target/cli-benchmark/20261030T131331Z-XLO4v0/`.
Its metadata identifies Rust revision `1407953` and the unchanged C++ binary from the baseline.
The remaining dirty paths contain documentation and comment changes only.
These are warm-cache local macOS measurements, not Windows or cold-storage throughput claims.

### Optimized profiling and validation

A fresh samply capture runs the generated 60 second stereo WAV 16 times.
The recording is `target/profile-custom-meter.json.gz`, with its presymbolicated sidecar.
It contains 729 analysis-worker samples, of which 624, or 85.6 percent,
have `BlockMeter::measure_planes` as the leaf and 58, or 8.0 percent, have the PCM decoder as the leaf.
There are no ebur128 or `calc_gating_block` frames.
The optimized measurement kernels are inlined into `measure_planes`,
so this capture does not assign separate sampled percentages to K weighting, extrema, waveform, and true peak.
Those remain distinct required owners of their measurements,
and waveform rescans still occur when a bucket boundary splits a block.
It is not a claim of one combined sample loop for every measurement.

The Instruments Allocations capture could not attach to the CLI process on this machine.
Its failed trace does not establish allocation behavior.
An isolated release harness in `target/allocation-check/` instead wraps the system allocator with `stats_alloc`.
It counts zero allocations, reallocations, and allocated bytes across 600 complete 100 ms meter pushes after construction.
It also counts zero across 599 fused writer, quantized-buffer, and meter pushes at both 16 and 24 bits,
after the reusable writer scratch is primed and the output sink is reserved.
The harness calls production APIs and is not a new production dependency or unsafe code in core.
This verifies steady-state blocks with a known length, not decoding, startup, finish, metadata, unknown-length growth,
or every possible waveform merge.

The full macOS workspace passes 860 tests with the full normalization corpus required and none skipped.
Clippy fix, strict all-target Clippy, rustfmt, shell formatting, prose formatting, and Python checks pass.
The C++ debug preset builds both applications and the test plugin, and all 28 CTest suites pass.
Changed-source lint and clang-format pass.
The parity and benchmark tooling tests also pass.

Release C++ parity passes at 0.05 tolerance for all 29 top-level fixtures and all 32 normalization corpus inputs.
The four normalization targets pass their independent level, target, sample, and metadata checks:
peak 0 and peak -1 each compare 29 outputs, LUFS -14 compares 28, and LUFS -23 compares 29.
Expected silent, excessive-gain, short-loudness, and output-conflict failures remain checked, not silently excluded.
Lossless peak outputs remain sample-exact and loudness outputs stay within one PCM step.
The in-render record tests independently require exact equality with a fresh decode,
including history, waveform, overload bytes, file identity, and final partial windows after three seconds.

## Numerical and regression contracts

Integrated loudness, maximum short-term loudness,
and each short-term history value must stay within 0.001 LU of the ebur128 reference.
Integrated and maximum short-term values must also display identically at two decimal places.
This permits a different energy summation order, not arbitrary loudness or display changes.
True peak retains its unchanged bit-exact contract, with the documented x86 arithmetic-underflow exception below.
Sample extrema, waveform resolution, overload semantics, and PCM quantization keep their existing contracts.

Goldens record our own implementation's numeric fields, histories, blobs, gains, and normalized sample hashes.
They are regression snapshots, not a requirement to retain the reference's floating point bits forever.
A validated numerical change may regenerate the affected goldens and CLI snapshots.
Review those changes alongside differential checks, displayed-value agreement, chunk-size invariance,
exact written-PCM measurement checks, normalization targets, and C++ parity.
A saved CLI snapshot remains useful for identifying changes, but exact old JSON and loudness blobs are not the contract.
Run the validation with:

```shell
cargo nextest run
./scripts/snapshot.sh compare target/snapshot-before
./scripts/parity.sh --release ../fixtures/normalization
./scripts/parity.sh --release --normalize --peak 0 ../fixtures/normalization
./scripts/parity.sh --release --normalize --lufs -14 ../fixtures/normalization
```

## Custom loudness and in-render measurement

Production loudness uses `loudness/k_weighting.rs` and `loudness/gating.rs`.
Ebur128 is a development-only dependency for differential tests and benchmark references.
K weighting derives sample-rate-specific shelving and high-pass coefficients,
combines them into a fourth order filter, and uses fixed `f64` state for up to 64 channels.
The coefficient formulas and recurrence are derived from ebur128, with its license notice retained in the source.
The default ebur128 channel map includes its four- and five-channel layouts,
ignores LFE and unused channels for loudness, and doubles mono energy for dual-mono measurement.

Gating keeps 30 channel-weighted 100 ms energies in a fixed ring.
Complete short-term updates directly sum that ring, rather than subtracting expired energy from a running sum,
so long streams cannot accumulate running-sum drift.
Integrated gating uses 400 ms windows every 100 ms.
Only energies admitted by the absolute gate are appended to the integrated-history `Vec`,
with capacity reserved from the expected stream length, and the relative gate is applied at finish.
Leading short-term windows use implicit silence and the full three-second denominator.
A final partial update does not admit an incomplete integrated gating block.

After three seconds, a final partial update must expire only the corresponding prefix of the outgoing block.
A ring of whole-block energies alone cannot recover that boundary.
The implementation therefore also retains one weighted squared energy per frame in a shared three-second ring,
not three seconds of filtered audio for every channel.
The final partial update scans the outgoing suffix and combines it with retained block energies and new energy.
Complete updates only sum the 30 block energies.
This is an explicit bounded memory compromise for exact partial-window boundaries,
not a claim of block-energy-only storage.

`BlockMeter` can fail only at construction for channel counts outside 1 through 64
or sample rates outside 16 through 2822400 Hz.
The streaming and finish paths do not return meter errors.
Extrema use fixed arrays, and the waveform reuses them when an entire block fits in the remaining bucket.
Blocks crossing a waveform bucket boundary still rescan samples to preserve each bucket's range.
The existing minimum bucket size remains 64 frames, not a 100 ms analysis block.

Normalization measures output during rendering, from the exact decoded `f32` values of the written 16- or 24-bit PCM.
The writer fuses one `f32` gain multiplication with its existing quantizer,
without a scaled-audio copy or a per-chunk plane-reference `Vec`.
Reusable quantized planes feed the same block meter for output peaks, loudness, waveform, and overload history.
Header and frame-count validation still run before commit, but a full output re-decode is unnecessary.
An in-place source already at target returns its current record after cancellation and source-identity validation.
Analysis cache version 5 invalidates older measurements while preserving same-identity custom gains and Serato data.

## SIMD true peak

`loudness/true_peak.rs` places four independent interpolation phases in `f32x4` lanes below 96 kHz,
and duplicates the two phases into those lanes from 96 kHz to below 192 kHz.
The fixed histories expose the newest samples contiguously without allocating during processing.
Every phase keeps the reference coefficient generation, tap order, and separate multiply and add.
Ordered comparisons preserve the reference NaN policy across SIMD backends.
The interpolator includes decoded sample extrema and does not drain an interpolation tail at end of stream.
At 192 kHz and above `BlockMeter` reuses the signed extrema it already measured, without another sample scan.

The production loudness meter performs neither sample peak scanning nor true-peak interpolation.
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
Golden tests capture the current `BlockMeter`, and differential tests check its loudness within the contract above.

## Historical SIMD verification

This verification predates the custom loudness meter and in-render normalization measurements.
The local macOS run passed all 637 workspace tests with none skipped, including the then-current golden files,
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

## Historical SIMD measurements

These results describe the true-peak SIMD change with the ebur128 loudness meter still in production.
They are not timings for the current custom loudness and normalization pipeline.
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

## Block pipeline and source overload history

The block meter measures complete aligned decoder blocks directly from borrowed planes.
Partial blocks and missing or short channels reuse sample buffers, retaining every supplied prefix before silence padding.
Plane references use fixed stack storage instead of allocating a vector for every block.
Block-local extrema feed the global extrema and strict sample-over flags.
At 192 kHz and above they also supply true-peak-over flags without another sample scan.
At lower rates, `TruePeak::previous_peaks` exposes the last nonempty push's maxima without allocation or filter resets.

Source overload history is independent of waveform quantization, gain, preview inserts, and display formatting.
It retains separate sample and true peak bits per actual channel, OR-merging adjacent buckets at a limit of 8192.
The final partial bucket preserves the decoded length, and an all-clear history is valid present data.
Cache analysis version 5 requires a validated independent overload blob for successful rows.
Missing, malformed, or older data triggers a refill without removing custom gains or independent Serato data.
Normalized outputs receive their histories from exact quantized in-render measurements,
and plugin outputs receive theirs from output reanalysis.
Warning projection is separate GUI work.
Sample flags cover their bucket directly.
Interpolated flags need conservative coverage of retained filter history, rather than claiming an exact peak position.

The analysis benchmark harness borrows mono and stereo planes in fixed storage,
so isolated kernels no longer include a temporary plane-vector allocation for every push.
`true_peak_underflow` generates ordinary, tiny normal, and genuine decoded-f32 subnormal inputs in memory.
The histories start empty for each iteration, so a previous normal peak cannot hide the underflow workload.
`overload_history` measures the production collector separately, including repeated stride doubling.
The normalization benchmark calls the production `apply_gain` helper, which retains the original scalar f32 product.
Neither the block-pipeline changes nor the custom loudness meter introduce extrema, gain, conversion, or K-weighting SIMD.

### Historical controlled follow-up measurements

These measurements predate the custom loudness meter and in-render normalization measurements.
A fresh `core-followups-before` baseline measures the meter before the block changes,
separately from the earlier exploratory `before` figures.
The baseline stereo timings are 30.765 ms at 44.1 kHz, 33.614 ms at 48 kHz,
61.243 ms at 88.2 kHz, 72.836 ms at 96 kHz, and 120.45 ms at 192 kHz.
Mono at 44.1 kHz takes 14.749 ms.
The input is 10 seconds of generated music, with 4096-frame pushes.
Each case uses 10 samples, a one-second warmup, and a four-second group measurement target.
These runs use the same M3 Max, Rust 1.99.1, native CPU flags, and release LTO as above.

The final meter including source overload history, saved as `core-followups-history`,
takes 27.582, 29.883, 54.793, 66.248, and 112.08 ms for those stereo rates.
Mono takes 13.738 ms.
The baseline includes temporary plane-vector allocations in the benchmark harness,
while the revised harness uses fixed references, so this is not a pure production-only kernel comparison.
The optimized pipeline without history, saved as `core-followups-pipeline`,
takes 27.832, 29.923, 55.970, 66.766, and 112.21 ms for those stereo rates, and 13.873 ms for mono.
It is measured from the optimization-only revision in an isolated worktree with the same revised benchmark harness.
The pipeline-only run passes all 19 meter tests before timing.
These independent runs establish that the history overhead is small compared with run-to-run variation,
not that adding history makes the meter faster.
A subsequent Criterion comparison against the pipeline-only baseline finds no change at 44.1, 88.2, and 192 kHz or mono.
At 48 and 96 kHz the reported time changes are +1.33 and -1.31 percent, both within the configured noise threshold.
The final comparison times are 28.023, 30.410, 55.008, 66.006, and 110.63 ms for stereo, and 13.754 ms for mono.
These measurements do not resolve sub-percent collection overhead in the complete meter.
The old baseline to final timings decrease by roughly 7 to 11 percent across these inputs,
including the noted harness-allocation change.

The isolated stereo collector takes 0.486 microseconds for 100 blocks including construction and finish,
42.910 microseconds for 8193 blocks, and 119.16 microseconds for 32771 blocks.
The latter cases include bounded bucket merging rather than an unbounded append-only history.
These timings exclude peak detection, waveform building, serialization, and SQLite work.
The production scalar gain benchmark takes 21.958 microseconds for 441000 mono samples,
with the reusable output allocation outside the iteration.
Neither measurement is a claim about an explicit SIMD gain or history kernel.
For fresh 4800-frame interpolation, SIMD takes 11.323 microseconds at ordinary amplitude,
11.272 at tiny normal amplitude, and 11.247 for subnormal input.
The corresponding ebur128 timings are 27.080, 27.231, and 27.112 microseconds.
These local NEON timings do not resolve the x86 flush-to-zero numerical exception.

### Historical core follow-up verification

This verification describes the block-pipeline and overload-history changes before the custom loudness meter.
The macOS workspace passed all 684 tests with none skipped.
The optimized core run passes all 299 tests with `AUDIOBATCH_REQUIRE_NORMALIZATION_FIXTURES=1`,
requiring the entire normalization corpus rather than accepting missing LFS members.
The unchanged CLI snapshot matches analysis, peak, and loudness JSON,
and all 56 peak and 55 loudness output pairs have identical samples and tags.
C++ analysis parity passes for 32 corpus files,
and normalization parity passes for 29 peak and 28 loudness outputs with only accepted lossy-decoder differences.
Existing measurement goldens and the saved pre-optimization snapshot are unchanged.
Full workspace Clippy, including `--all-targets -- -D warnings`, and the Rust, shell, and prose checks pass.
The dependency `block` still emits its existing future-compatibility notice.

## Remaining validation

The cross-target checks recorded below predate the custom loudness meter and in-render normalization measurements.
They must be repeated for the current implementation and do not establish its Windows execution results.
Windows execution and timings for the x86 SIMD kernels are deferred to the user's PC after the macOS commits.
They do not block the local macOS work, and cross-target checks do not count as execution evidence.
The Windows pass must run debug and optimized core tests with the full normalization corpus.
It also needs CLI snapshots, parity where available, and controlled benchmarks with backend diagnostics.
The production core library passes `cargo clippy --lib --target x86_64-pc-windows-msvc -- -D warnings`
with the SIMD path enabled and portable `RUSTFLAGS`.
This local type-check uses `LIBSQLITE3_SYS_USE_PKG_CONFIG=1 PKG_CONFIG_ALLOW_CROSS=1` to avoid compiling bundled SQLite.
It does not link or execute a Windows binary,
and all-target checking is blocked by the missing Windows C headers needed by Criterion's `alloca` dependency.
Cross-compilation alone does not establish output equality or speed on that machine.
The x86 analysis path runs the SIMD kernel directly, so Windows validation needs no feature flag or code change.
Its arithmetic underflow difference remains explicit and must not be hidden by regenerating golden outputs.
The custom loudness meter and in-render normalization measurements require their own controlled benchmark comparisons.
The historical runs above do not establish their current correctness or speed.

## Separate implementation follow-ups

- Keep the 0.001 LU and two-decimal display contracts covered by differential checks,
  including `f64` maximum loudness, repeated window wraps, and final partial windows after three seconds.
  Regenerating our own regression snapshots does not replace those checks.
- Keep explicit extrema, gain, and AIFF conversion SIMD as independently measured work.
  Preserve scalar exceptional values, signed zero, f64 promotion, clipping, and ties-to-even output rounding.
  The shared scalar gain helper is benchmark coverage, not an optimization claim.
- Keep artwork loading independent of analysis and overload cache records.
  Request identity, coalescing, negative-result caching, parse options, and refresh lifecycle need their own review.
  Shared metadata-copy reads must continue to preserve embedded pictures.
- Keep Clear All operation exclusion and worker-drain safety in GUI run orchestration.
  Clearing visible rows must not permit conflicting output operations or resurrect rows from late events.
- Keep gain-clear authority, cue-start races, and selection or sorting state fixes as separate GUI changes.
- Add warning rendering afterward using source-identity-matched history.
  Preview gain and plugins must not rewrite source overload bits,
  and interpolation coverage must not be presented as an exact decoded sample position.
