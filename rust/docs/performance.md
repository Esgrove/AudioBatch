# Analysis performance

## Current results

Rust uses a custom K-weighting and gating meter, SIMD true peak, and in-render normalization measurements.
Ebur128 is a development-only reference for differential tests and component benchmarks.
On the measured corpus, Rust analysis is about 5.4 times faster than C++,
and normalization is about 3.5 to 3.7 times faster with one worker.
Compared with the Rust baseline, the complete meter is 4.69 times faster and file normalization is 2.60 times faster.

### Measurement conditions

Measurements were collected on October 30, 2026, on an Apple M3 Max with 16 logical CPUs and 64 GiB of memory,
running macOS 27.2 on arm64.
Rust 1.99.1 uses release LTO, one code generation unit, and `target-cpu=native`.
C++ uses Apple clang 21 and the `macos-release` CMake preset with `-O3 -DNDEBUG`.
Runs are sequential, without concurrent builds, tests, or profiling.
These are warm-cache local macOS results, not Windows or cold-storage measurements.

### C++ versus Rust CLI

The corpus contains 29 eligible files from `fixtures/normalization`.
Each command has one warmup and three measured runs, alternating C++ and Rust.
Fresh copies and configuration are prepared outside timing.
Analysis uses `--refresh`, and normalization includes initial source analysis, rendering, output measurement,
metadata, validation, cache writes, and permanent disposal of temporary originals with `--delete`.
Configuration and cache isolation use HOME and XDG overrides on a best-effort basis.

Means and sample standard deviations are in seconds.
Speedup is C++ time divided by Rust time.

With one worker:

- Analysis: C++ **1.618250 ± 0.014608 s**, Rust **0.297451 ± 0.007325 s**, **5.440 times faster**.
  The Rust baseline is 0.852869 s, a 65.1 percent reduction in elapsed time.
- Peak normalization to 0 dBFS: C++ **3.277709 ± 0.054053 s**, Rust **0.884520 ± 0.011951 s**,
  **3.706 times faster**.
  The Rust baseline is 2.040939 s, a 56.7 percent reduction.
- Loudness normalization to -14 LUFS: C++ **3.270517 ± 0.040099 s**, Rust **0.928473 ± 0.023419 s**,
  **3.522 times faster**.
  The Rust baseline is 2.012160 s, a 53.9 percent reduction.

With 16 workers:

- Analysis: C++ **0.310800 ± 0.003239 s**, Rust **0.056582 ± 0.001800 s**, **5.493 times faster**.
  The Rust baseline is 0.137665 s, a 58.9 percent reduction.
- Peak normalization to 0 dBFS: C++ **0.594622 ± 0.011371 s**, Rust **0.285083 ± 0.002519 s**,
  **2.086 times faster**.
  The Rust baseline is 0.435082 s, a 34.5 percent reduction.
- Loudness normalization to -14 LUFS: C++ **0.595915 ± 0.019191 s**, Rust **0.293995 ± 0.004707 s**,
  **2.027 times faster**.
  The Rust baseline is 0.448726 s, a 34.5 percent reduction.

Every measured pair passes the level, normalization-target, sample, and metadata checks.
The three known failure fixtures are excluded from timing but remain covered by parity verification.

### Rust Criterion benchmarks

The in-memory input is 10 seconds of stereo music at 44.1 kHz.
File benchmarks use the corresponding 60 second WAV.
Baseline comparisons use the saved `custom-meter-before` measurements.

- `meter/block_meter/44100_2ch`: **5.9701 ms**, 95 percent interval 5.9556 to 5.9906 ms.
  Baseline 27.982 ms, **4.69 times faster**.
- `analyze_file/music_stereo_44100_16.wav`: **41.396 ms**, interval 41.275 to 41.510 ms.
  Baseline 175.52 ms, **4.24 times faster**.
- `normalize_file/peak/music_stereo_44100_16.wav`: **84.876 ms**, interval 84.551 to 85.178 ms.
  Baseline 221.83 ms, **2.61 times faster**.
- `normalize_file/lufs/music_stereo_44100_16.wav`: **84.826 ms**, interval 84.364 to 85.417 ms.
  Baseline 220.69 ms, **2.60 times faster**.
- `render/gain_and_aiff/16`: **2.1915 ms**, interval 2.1822 to 2.2056 ms.
  Baseline 2.2577 ms, 2.6 percent less time.
- `render/gain_and_aiff/24`: **3.2381 ms**, interval 3.2310 to 3.2463 ms.
  Baseline 3.3613 ms, 3.7 percent less time.
- `render/gain_only`: **21.840 microseconds**, interval 21.706 to 22.057 microseconds.
  Baseline 21.801 microseconds, no statistically significant change.

The writer microbenchmark calls the production fused writer with a no-op sample observer and excludes `BlockMeter`.
The end-to-end normalization cases include output metering but start with a current source analysis record.
CLI normalization also includes initial source analysis, so its speedup measures a different work boundary.
Throughput is audio frames per second, not channel samples per second.

### Saved reports

The complete CLI exports contain individual samples, corpus and binary hashes, versions, and build settings:

- Current: `target/cli-benchmark/20261030T131331Z-XLO4v0/`, Rust binary revision `1407953`.
- Baseline: `target/cli-benchmark/20261030T123706Z-EwC7zE/`, Rust binary revision `e5eca33`.
- Both runs use the same C++ binary, revision `704ae4b`, and the same corpus.
- Criterion baseline: `target/criterion/`, named `custom-meter-before`.

These generated reports are local artifacts and are not committed.

## Processing and memory

`BlockMeter` groups planar decoder chunks into 100 ms measurement blocks.
Complete aligned blocks borrow the decoder's channel buffers.
Plane references, extrema, and K-weighting state use fixed storage for up to 64 channels.
Histories reserve capacity from the expected frame count when available.

K weighting combines the shelving and high-pass stages into one fourth order `f64` filter.
Filtering accumulates weighted squared energy directly.
Short-term loudness sums a fixed ring of 30 block energies,
avoiding repeated filtered-audio scans and running-sum drift.
Integrated loudness uses four-block windows, the -70 LUFS absolute gate, and the relative gate 10 LU below the mean.
Startup short-term readings include leading silence, and a final partial block adds no incomplete integrated window.

Exact final partial windows also need the outgoing block's unexpired suffix.
A shared three-second ring retains one weighted squared energy per frame, independent of channel count.
At 48 kHz it uses about 1.15 MB, in addition to block totals, filter state, and histories.
This retains no per-channel filtered audio.
Complete queries sum block totals, and only the final partial query scans the boundary suffix.

Waveform construction reuses block extrema when the block fits in a bucket and the extrema retain its exact range.
Bucket crossings and zero-bound extrema require rescanning samples.
The minimum waveform bucket is 64 frames, so a 100 ms meter block can cross several buckets.
K weighting, extrema, and true peak remain separate measurement loops.

True peak uses `fearless_simd`, with NEON on Apple Silicon and AVX2 on x86.
Four interpolation phases occupy `f32x4` lanes below 96 kHz, and two phases are duplicated below 192 kHz.
At 192 kHz and above, the meter reuses sample extrema instead of scanning for true peak again.
Ordered comparisons preserve NaN handling, and interpolation retains tap order and separate multiplication and addition.

Normalization applies one `f32` gain multiplication inside the AIFF quantizer.
Reusable planar buffers feed the exact decoded values of the written 16- or 24-bit PCM to `BlockMeter`.
There is no scaled-audio copy, per-chunk plane-reference allocation, or full output re-decode.
Header and frame-count validation remain in place before commit.
An in-place source already at target returns its current record after cancellation and source-identity checks.

[Architecture](../ARCHITECTURE.md) describes the channel map, waveform, overload history, cache, and commit lifecycle.
The adapted coefficient routines retain their upstream notice in
[`crates/core/licenses/ebur128.txt`](../crates/core/licenses/ebur128.txt).

### Profile and allocation checks

A 16-iteration samply capture of the 60 second stereo WAV contains 729 analysis-worker samples.
`BlockMeter::measure_planes` is the leaf for 85.6 percent, and PCM decoding for 8.0 percent.
Measurement kernels are inlined into `measure_planes`, so the profile does not separate their individual costs.
There are no ebur128 or `calc_gating_block` frames.
The capture is `target/profile-custom-meter.json.gz`, with a presymbolicated sidecar.

The release harness in `target/allocation-check/` uses `stats_alloc` around production APIs.
It counts zero allocations and reallocations across 600 complete meter blocks after construction,
and 599 fused writer and meter blocks at both 16 and 24 bits after scratch buffers are primed.
This covers known-length steady-state processing, not decoding, construction, finish, metadata,
unknown-length history growth, or every waveform merge.
The harness is local and introduces no production dependency or unsafe code in core.

## Accuracy and validation

- Integrated loudness, maximum short-term loudness, and each history value stay within 0.001 LU of ebur128.
  Integrated and maximum short-term values must also match at two displayed decimal places.
- True peak is bit-exact against the reference except for the explicit x86 arithmetic-underflow case below.
  Chunk-size invariance, sample extrema, waveform bytes, and overload history retain exact regression checks.
- In-render normalization records equal an independent fresh decode, including file identity, history,
  waveform, overload bytes, and final partial windows after three seconds.
- Lossless C++ parity is sample-exact in peak mode and allows at most one PCM step in loudness mode.
  Lossy formats retain their separate decoder tolerance.
- Goldens are regression snapshots of the current meter.
  Regeneration requires differential, displayed-value, normalization, and parity checks.
- Cache analysis version 5 refreshes older measurements while retaining same-identity custom gains and Serato data.

Local macOS validation passes all 860 workspace tests with the normalization corpus required and none skipped.
Release core tests, Clippy, rustfmt, shell and prose formatting, Python checks, and packaging regression tests pass.
The C++ debug build and all 28 CTest suites pass, as do clang-tidy and clang-format checks.

Release analysis parity passes at 0.05 tolerance for 29 top-level fixtures and all 32 normalization corpus inputs.
Normalization parity passes all four targets:
peak 0 and peak -1 each compare 29 outputs, LUFS -14 compares 28, and LUFS -23 compares 29.
Expected silence, excessive-gain, short-loudness, and output-conflict failures are checked separately.

### Validation limits

- Windows execution and controlled timings for the current meter are not established by the local macOS results.
  Run debug and release tests with the full normalization corpus, parity, and benchmarks on the target PC.
- Ebur128 temporarily enables arithmetic flush to zero on x86 with SSE2.
  The safe SIMD kernel leaves the caller's floating point environment unchanged,
  so arithmetic-underflow results can differ even for normal source samples.
  A focused x86 regression covers this difference.
- The generated long AAC normalization input fails the existing lossy-decoder sample tolerance.
  The complete CLI comparison therefore uses the committed normalization corpus, not the combined generated corpus.
  The tolerance remains unchanged.
- Three timing samples per command support this local comparison, not a universal performance guarantee.
  CI reports timings without a performance threshold, and its benchmark step does not fail parity.

## Reproducing the measurements

Run commands from `rust/`.
Generate the long Criterion inputs once:

```shell
uv run ../fixtures/benchmark/generate.py
```

Compare release CLIs on the measured corpus:

```shell
./scripts/cli_benchmark.sh --corpus ../fixtures/normalization --jobs 16 --runs 3 --warmup 1
```

The default `--corpus auto` also includes generated benchmark audio when present.
The script requires hyperfine and uv, builds the Rust release CLI, and discovers a release C++ CLI.
Set `AUDIOBATCH_CPP_CLI` to select a specific C++ binary.
Exports go into a unique directory under `target/cli-benchmark/`.
Copies and correctness checks run outside timing, and no original fixture is normalized or deleted.

Criterion targets cover `analysis`, `decode`, `normalize`, `waveform`, and `misc`.
File benchmarks skip absent generated inputs, while in-memory cases always run.
Save a new reference before changing code, then compare without replacing it:

```shell
cargo bench -p audiobatch-core --bench analysis -- --save-baseline before
cargo bench -p audiobatch-core --bench normalize -- --save-baseline before
cargo bench -p audiobatch-core --bench analysis -- --baseline before
cargo bench -p audiobatch-core --bench normalize -- --baseline before
```

To compare the measured cases with the existing optimization baseline:

```shell
cargo bench -p audiobatch-core --bench analysis -- 'meter/block_meter/44100_2ch|analyze_file/music_stereo_44100_16.wav' --baseline custom-meter-before --noplot
cargo bench -p audiobatch-core --bench normalize -- 'render/|normalize_file/(peak|lufs)/music_stereo_44100_16.wav' --baseline custom-meter-before --noplot
```

Record a sampling profile with release optimization and debug symbols:

```shell
cargo build --profile profiling --bin audiobatch-rs
samply record --save-only --unstable-presymbolicate --iteration-count 16 -o target/profile.json.gz -- ./target/profiling/audiobatch-rs --refresh --jobs 1 --output json ../fixtures/benchmark/output/music_stereo_44100_16.wav
samply load target/profile.json.gz
```

Inspect analysis-worker samples rather than main-thread waits.
Run benchmarks and profiles without concurrent builds or tests.

Run the correctness checks independently of timing:

```shell
AUDIOBATCH_REQUIRE_NORMALIZATION_FIXTURES=1 cargo nextest run
AUDIOBATCH_REQUIRE_NORMALIZATION_FIXTURES=1 cargo nextest run --release -p audiobatch-core
./scripts/parity.sh --release ../fixtures
./scripts/parity.sh --release ../fixtures/normalization
./scripts/parity.sh --release --normalize --peak 0 ../fixtures/normalization
./scripts/parity.sh --release --normalize --peak -1 ../fixtures/normalization
./scripts/parity.sh --release --normalize --lufs -14 ../fixtures/normalization
./scripts/parity.sh --release --normalize --lufs -23 ../fixtures/normalization
```
