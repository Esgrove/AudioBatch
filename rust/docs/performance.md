# Analysis performance

## macOS results

Rust uses a custom K-weighting and gating meter, SIMD true peak, and in-render normalization measurements.
Ebur128 is a development-only reference for differential tests and component benchmarks.
On the measured macOS corpus, Rust analysis is about 5.4 times faster than C++,
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

## Windows results

### Measurement conditions

Measurements were collected on October 31, 2026, on an AMD Ryzen 9 7950X with 16 cores, 32 logical CPUs,
and 64 GiB of installed memory, running Windows 11 Pro, build 26300, on x86-64.
Rust 1.99.1 uses release LTO, one code generation unit, and `RUSTFLAGS="-C target-cpu=native"`.
The C++ Release CLI uses Visual Studio 18 2026 and MSVC 19.51, toolset 14.51, with `/O2 /Ob2 /DNDEBUG`.
The measured Rust binary reports revision `3f0a6e5`, and the C++ binary reports `7bca7de`.
Hyperfine 2.0.0 executes native Git Bash explicitly, not WSL.
CPU and installed memory were read through Windows CIM.
The export's optional `lscpu` metadata field is unavailable on Windows.
Builds and correctness suites finish before timing, and benchmark targets run sequentially.
These are warm-cache local measurements, not cold-storage results or isolated operating-system comparisons.

The Windows CLI corpus has 26 eligible files, compared with 29 on macOS.
Both exclude the three known normalization failures.
Windows also excludes two M4A inputs and one AIFC input,
because the C++ CLI's Windows format registration does not discover those extensions.
The Rust decoder still supports them, and the full Rust normalization corpus remains covered by golden tests.
Each measured pair checks levels, normalization targets, output samples, and metadata outside timing.
The benchmark does not relax the parity tolerances or accept failed CLI invocations.

### C++ versus Rust CLI

Each command has one warmup and ten measured runs, alternating C++ and Rust.
Fresh copies and configuration are prepared outside timing.
The work boundaries match the macOS CLI comparison, including initial analysis during normalization.
Means and sample standard deviations are in seconds.
Speedup is the ratio of C++ mean time to Rust mean time.

| Workers | Operation | C++, mean ± standard deviation, s | Rust, mean ± standard deviation, s | Speedup |
| ---: | :--- | ---: | ---: | ---: |
| 1 | Analysis | 1.195422 ± 0.011775 | 0.302966 ± 0.004488 | 3.946 |
| 1 | Peak normalization, 0 dBFS | 2.503883 ± 0.062913 | 0.905042 ± 0.138723 | 2.767 |
| 1 | Loudness normalization, -14 LUFS | 2.509610 ± 0.031247 | 0.856444 ± 0.063901 | 2.930 |
| 32 | Analysis | 0.346678 ± 0.006299 | 0.117265 ± 0.039087 | 2.956 |
| 32 | Peak normalization, 0 dBFS | 0.621137 ± 0.006144 | 0.206332 ± 0.027790 | 3.010 |
| 32 | Loudness normalization, -14 LUFS | 0.696802 ± 0.265634 | 0.335879 ± 0.434500 | 2.075 |

All measured pairs pass the level, sample, and metadata checks.
The 32-worker loudness case has substantial outliers, including 1.451 seconds for C++ and 1.571 seconds for Rust.
Its medians are 0.606432 seconds for C++ and 0.194740 seconds for Rust.
The table retains every sample rather than discarding the slower runs.
The timing spread, different hardware, and smaller Windows corpus limit cross-platform speedup comparisons.

### Rust Criterion benchmarks

The inputs and work boundaries match the macOS Criterion cases,
10 seconds of generated in-memory music and the generated 60 second stereo 44.1 kHz WAV for file operations.
Times are Criterion point estimates in milliseconds, with 95 percent confidence intervals.
There is no saved Windows pre-optimization baseline, so these are current timings, not before-and-after speedups.

| Case | Estimate, ms | 95 percent interval, ms |
| :--- | ---: | ---: |
| Complete meter, 44.1 kHz stereo | 6.1924 | 6.1819 to 6.2124 |
| Analyze 60 second WAV | 41.0224 | 40.8403 to 41.2888 |
| Normalize WAV, peak | 86.5206 | 84.6627 to 88.5178 |
| Normalize WAV, loudness | 95.1670 | 84.6678 to 107.8254 |
| Fused gain and AIFF writer, 16 bit | 2.6163 | 2.5754 to 2.6726 |
| Fused gain and AIFF writer, 24 bit | 2.7714 | 2.7527 to 2.7903 |
| Gain only | 0.04795 | 0.04757 to 0.04841 |

The loudness normalization case contains two high-severe outliers, so its interval is wider than the analysis interval.
The writer cases exclude output metering, while file normalization includes it and starts with a current source record.
Throughput counts audio frames, not channel samples.

The same run also measures the complete meter and the development-only ebur128 reference at every interpolation boundary:

| Input | Complete meter, ms | Ebur128 full, ms | Reference time divided by meter time |
| :--- | ---: | ---: | ---: |
| 44.1 kHz stereo | 6.1924 | 26.1917 | 4.23 |
| 48 kHz stereo | 6.7305 | 28.1049 | 4.18 |
| 88.2 kHz stereo | 12.1193 | 51.1557 | 4.22 |
| 96 kHz stereo | 19.4939 | 54.5689 | 2.80 |
| 192 kHz stereo | 14.8851 | 88.6395 | 5.95 |
| 44.1 kHz mono | 3.1724 | 12.5640 | 3.96 |

Criterion reports are saved in `target/criterion/` under the baseline name `windows-20261031`.
The ten-run CLI report is `target/cli-benchmark/20261031T193808Z-iagcyo/`,
with raw samples, binary and corpus hashes, build settings, and correctness output.
The earlier three-run CLI comparison is `target/cli-benchmark/20261031T193550Z-pCWkLF/`.
It passes correctness checks, but its normalization timings contain substantial outliers.
Generated timing reports are local artifacts and are not committed.

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

Local Windows validation passes all 870 workspace tests with the normalization corpus required.
The three opt-in numerical diagnostic tests remain ignored in the routine run.
The Windows Rust job requires the full normalization goldens in debug and release.
Optimized checks cover the complete core suite and the selected CLI normalization tests.
Clippy, rustfmt, workflow validation, benchmark-tooling regressions, parity-oracle tests,
and the changed Python, shell, and prose formatting checks pass.

Release analysis parity passes at 0.05 tolerance for 29 top-level fixtures and all 32 normalization corpus inputs.
Normalization parity passes all four targets:
peak 0 and peak -1 each compare 29 outputs, LUFS -14 compares 28, and LUFS -23 compares 29.
Expected silence, excessive-gain, short-loudness, and output-conflict failures are checked separately.

### Validation limits

- Windows and macOS measurements use different CPUs and platform decoders.
  Compare implementations within each machine, not the operating systems as an isolated performance variable.
- Ebur128 temporarily enables arithmetic flush to zero on x86 with SSE2.
  The safe SIMD kernel leaves the caller's floating point environment unchanged,
  so arithmetic-underflow results can differ even for normal source samples.
  A focused x86 regression covers this difference.
- The generated long AAC normalization input fails the existing lossy-decoder sample tolerance.
  The complete CLI comparison therefore uses the committed normalization corpus, not the combined generated corpus.
  The tolerance remains unchanged.
- Three timing samples per command support this local comparison, not a universal performance guarantee.
  CI reports timings without a performance threshold, and its benchmark step does not fail parity.

### Windows and macOS numerical differences

The Windows x86-64 and macOS arm64 CI runs use the same Rust 1.99.1 compiler release and portable `RUSTFLAGS=""`.
All 88 generated input identities and the 34 committed fixture checksums match.
The Cargo lockfiles also match after line-ending normalization.
The differences begin in native floating point math, not signal generation or fixture corruption.
K-weighting coefficients at 88.2 and 192 kHz differ in their last bits,
and eight committed lossy fixtures decode to different PCM hashes despite matching source bytes.
Within each platform the captured debug and release numerical stages agree exactly.

The largest absolute differences across the 34 Windows snapshot overrides are:

| Measurement | Maximum difference |
| :--- | ---: |
| Integrated loudness | 0.000000107 LU |
| Maximum short-term loudness | 0.000000104 LU |
| Short-term history above -70 LUFS | 0.00000191 LU |
| Sample peak amplitude | 0.0000000596 |
| Sample peak level | 0.00000164 dB |
| True peak amplitude | 0.0000000596 |
| DC-filter tail history | 0.01215 LU |

Sample peaks differ by at most two representable `f32` steps.
The maximum amplitude difference equals half one 24-bit PCM step, or 1/512 of a 16-bit step.
The largest history difference occurs at the final point of the 192 kHz DC test,
around -143.28 LUFS, where the filter has removed the constant input and only a near-silent residual remains.
Loudness values agree beyond the two displayed decimal places.
The normalized sample bytes checked by the golden normalization tests remain identical.
These observations describe this corpus, not a guarantee that every lossy input decodes identically.

Exact regression checks use 34 Windows x86-64 overrides in `crates/core/tests/golden/windows-x86_64/`,
with shared snapshots as the fallback for all other outputs.
No numerical tolerance was added, and production measurement and decoding are unchanged.
On Windows, `UPDATE_GOLDEN=1` writes only differing overrides and never rewrites the shared snapshots.
Input identity checks remain platform-independent.

### CI coverage

The Rust workflow combines Clippy and tests in one job per platform,
retaining workspace debug tests, optimized core tests, the CLI smoke checks, and macOS packaging and snapshot checks.
Nextest compiles its test targets and runs the input identity tests as part of the normal suites.
The separate macOS coverage job remains.
Temporary numerical captures and their uploads are not part of routine CI.

The Windows Rust build job downloads the full normalization corpus through the LFS cache.
Its full debug workspace and optimized core suites cover filesystem, normalization, and golden checks,
with CLI normalization tests included in the optimized run too.
`AUDIOBATCH_REQUIRE_NORMALIZATION_FIXTURES=1` rejects missing audio and unresolved LFS pointers.
Native parity-oracle and benchmark-tooling tests run separately from the timings.
Normalization-fixture changes trigger the Rust workflow.
The separate parity workflow runs the macOS C++ versus Rust comparison, without a duplicate Windows test job.

Numerical reports remain available on demand:

```shell
AUDIOBATCH_DIAGNOSTICS_DIR=target/numerical-diagnostics/debug cargo test -p audiobatch-core numerical_diagnostics_ --lib -- --ignored
AUDIOBATCH_DIAGNOSTICS_DIR=target/numerical-diagnostics/release cargo test --release -p audiobatch-core numerical_diagnostics_ --lib -- --ignored
```

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
On Windows run it from Git Bash.
The harness resolves the native Bash executable explicitly so hyperfine cannot select WSL.
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

To compare the measured cases with the existing macOS optimization baseline:

```shell
cargo bench -p audiobatch-core --bench analysis -- 'meter/block_meter/44100_2ch|analyze_file/music_stereo_44100_16.wav' --baseline custom-meter-before --noplot
cargo bench -p audiobatch-core --bench normalize -- 'render/|normalize_file/(peak|lufs)/music_stereo_44100_16.wav' --baseline custom-meter-before --noplot
```

To reproduce the Windows cases, save a new local Criterion baseline and collect ten CLI samples:

```shell
export RUSTFLAGS="-C target-cpu=native"
cargo bench -p audiobatch-core --bench analysis -- 'meter/(block_meter|ebur128_full)/|analyze_file/music_stereo_44100_16.wav' --save-baseline windows-local --noplot
cargo bench -p audiobatch-core --bench normalize -- 'render/|normalize_file/(peak|lufs)/music_stereo_44100_16.wav' --save-baseline windows-local --noplot
./scripts/cli_benchmark.sh --corpus ../fixtures/normalization --jobs 32 --runs 10 --warmup 1
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
