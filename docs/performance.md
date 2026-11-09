# Performance

Rust analysis is 3.8 to 4.7 times faster than C++ on the measured macOS corpus,
and 4.9 to 5.3 times faster on the larger Windows corpus.
Single-worker normalization is 2.7 to 2.9 times faster on macOS and 3.6 to 3.7 times faster on Windows.
VST3 processing is closer, with the result depending on the C++ compiler and plugin chain.

CLI and VST3 times are means with sample standard deviations.
The C++ / Rust ratio divides the C++ mean by the Rust mean.
Ratios above one favor Rust, and ratios below one favor C++.
CLI and VST3 measurements use warm filesystem caches and sequential runs without concurrent builds or tests.
The shared-cache checks below are short development measurements with their own stated limits.
Different hardware, corpora, and platform decoders prevent an isolated operating-system comparison.

## macOS CLI

Measured November 2, 2026, on an Apple M4 Pro with 14 logical CPUs and 48 GiB memory, running macOS 27.2.
Rust 1.99.1 uses native CPU targeting, release LTO, and one code generation unit.
C++ uses Apple clang 21 with `-O3 -DNDEBUG`.

The corpus contains 29 eligible normalization fixtures, excluding three known failure inputs.
Each command has one warmup and three measured runs, alternating C++ and Rust.
Normalization includes initial analysis, rendering, output measurement, metadata, validation, and cache writes.
Input copies and correctness checks are outside timing.
All measured pairs pass the level, normalization-target, sample, and metadata checks.

Times are in seconds.

| Workers | Operation | C++, s | Rust, s | C++ / Rust |
| ---: | :--- | ---: | ---: | ---: |
| 1 | Analysis | 1.375623 ± 0.022020 | 0.366561 ± 0.016009 | 3.753 |
| 1 | Peak normalization, 0 dBFS | 3.197021 ± 0.028894 | 1.116897 ± 0.034210 | 2.862 |
| 1 | Loudness normalization, -14 LUFS | 3.205563 ± 0.030976 | 1.176975 ± 0.019905 | 2.724 |
| 14 | Analysis | 0.279153 ± 0.002610 | 0.059650 ± 0.002875 | 4.680 |
| 14 | Peak normalization, 0 dBFS | 0.551336 ± 0.014720 | 0.308543 ± 0.009541 | 1.787 |
| 14 | Loudness normalization, -14 LUFS | 0.544206 ± 0.001968 | 0.307189 ± 0.003188 | 1.772 |

## Windows CLI

Measured November 1, 2026, on an AMD Ryzen 9 7950X with 16 cores, 32 logical CPUs, and 64 GiB installed memory,
running Windows 11 Pro build 26300.
Rust 1.99.1 uses native CPU targeting, release LTO, and one code generation unit.
C++ uses MSVC 19.51 with Release LTO, `/arch:AVX512`, `/favor:AMD64`, and precise floating-point arithmetic.

The corpus combines normalization fixtures and generated benchmark audio,
with 65 eligible files and ten failures excluded.
Each command has one warmup and five measured runs, alternating C++ and Rust.
The timed work includes the same analysis and normalization stages as the macOS comparison.
All measured pairs pass the level, normalization-target, sample, and metadata checks.
This larger corpus is not directly comparable with the macOS workload.

Times are in seconds.

| Workers | Operation | C++, s | Rust, s | C++ / Rust |
| ---: | :--- | ---: | ---: | ---: |
| 1 | Analysis | 14.392315 ± 0.170281 | 2.917110 ± 0.008495 | 4.934 |
| 1 | Peak normalization, 0 dBFS | 29.317068 ± 0.236124 | 7.983531 ± 0.068688 | 3.672 |
| 1 | Loudness normalization, -14 LUFS | 29.287836 ± 0.157043 | 8.130118 ± 0.244334 | 3.602 |
| 32 | Analysis | 1.699614 ± 0.030330 | 0.319620 ± 0.039085 | 5.318 |
| 32 | Peak normalization, 0 dBFS | 3.355598 ± 0.032431 | 0.882454 ± 0.074843 | 3.803 |
| 32 | Loudness normalization, -14 LUFS | 3.301726 ± 0.023186 | 0.814784 ± 0.028366 | 4.052 |

## Windows VST3

Measured November 1, 2026, on the Windows machine above.
Both C++ builds use Release LTO and native CPU targeting, with nonfused floating-point arithmetic.
MSVC uses `/arch:AVX512` and `/favor:AMD64`, and clang-cl 23.1.4 uses `-march=native` and `-mtune=native`.
Rust uses the same release settings as the CLI.
Both comparisons load the identical MSVC-built AudioBatch Test MS VST3 bundle.

Each case processes 100 seconds of repeated fixture audio in 1024-frame blocks,
with one warmup and 30 measured renders using freshly restored plugin state.
Host mode includes a single host and its input/output transfers.
Chain mode adds production channel adaptation and finite checks.
Latency drain and final block padding are timed, with latency compensation applied before verification.
Loading, decoding, allocation, preparation, state restoration, and sample exports are outside timing.
Every case has zero floating-point residual and zero differing 16-bit PCM samples against independent arithmetic.

Times are in milliseconds.
Each compiler comparison has separate Rust control measurements using the same executable.

| C++ compiler | Mode | Input | Plugins | Latency, frames | C++, ms | Rust, ms | C++ / Rust |
| :--- | :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| MSVC | Host | 44.1 kHz stereo | 1 | 0 | 7.309 +/- 0.217 | 7.353 +/- 0.541 | 0.994 |
| MSVC | Chain | 44.1 kHz stereo | 1 | 0 | 10.290 +/- 0.363 | 9.406 +/- 0.406 | 1.094 |
| MSVC | Chain | 44.1 kHz mono | 1 | 0 | 4.197 +/- 0.363 | 3.610 +/- 0.207 | 1.163 |
| MSVC | Chain | 48 kHz stereo | 4 | 0 | 23.716 +/- 0.486 | 21.464 +/- 0.395 | 1.105 |
| MSVC | Chain | 96 kHz stereo | 8 | 0 | 80.464 +/- 0.903 | 73.582 +/- 0.877 | 1.094 |
| MSVC | Chain | 44.1 kHz stereo | 4 | 1024 | 95.365 +/- 0.891 | 93.144 +/- 0.965 | 1.024 |
| clang-cl | Host | 44.1 kHz stereo | 1 | 0 | 7.291 +/- 0.271 | 7.240 +/- 0.507 | 1.007 |
| clang-cl | Chain | 44.1 kHz stereo | 1 | 0 | 8.732 +/- 0.335 | 9.245 +/- 0.392 | 0.945 |
| clang-cl | Chain | 44.1 kHz mono | 1 | 0 | 3.207 +/- 0.219 | 3.679 +/- 0.319 | 0.872 |
| clang-cl | Chain | 48 kHz stereo | 4 | 0 | 21.741 +/- 0.417 | 21.423 +/- 0.383 | 1.015 |
| clang-cl | Chain | 96 kHz stereo | 8 | 0 | 76.485 +/- 0.667 | 76.541 +/- 0.929 | 0.999 |
| clang-cl | Chain | 44.1 kHz stereo | 4 | 1024 | 93.658 +/- 0.787 | 93.355 +/- 0.865 | 1.003 |

Rust has lower observed chain times than MSVC, while the single-host measurements are close.
Clang-built C++ is faster in the mono and single-plugin stereo chains, with the remaining cases close to parity.
Rust control times vary between comparisons, particularly at 96 kHz.
These measurements exclude file decoding and writing and do not establish an end-to-end processing speedup.

## macOS VST3

Measured November 2, 2026, on the M4 Pro above.
C++ uses Apple clang 21 with the Release preset, and Rust uses the same release settings as the CLI.
Both hosts load the identical AudioBatch Test MS VST3 bundle built by the C++ project.

Each case processes 100 seconds of repeated fixture audio in 1024-frame blocks,
with one warmup and 30 measured renders using freshly restored plugin state.
The modes, timed work, and verification are the same as in the Windows measurements.
Every case has zero floating-point residual and zero differing 16-bit PCM samples against independent arithmetic.

Times are in milliseconds.

| Mode | Input | Plugins | Latency, frames | C++, ms | Rust, ms | C++ / Rust |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| Host | 44.1 kHz stereo | 1 | 0 | 3.016 +/- 0.044 | 2.960 +/- 0.120 | 1.019 |
| Chain | 44.1 kHz stereo | 1 | 0 | 5.381 +/- 0.163 | 5.136 +/- 0.104 | 1.048 |
| Chain | 44.1 kHz mono | 1 | 0 | 2.771 +/- 0.033 | 2.716 +/- 0.113 | 1.020 |
| Chain | 48 kHz stereo | 4 | 0 | 12.452 +/- 0.365 | 11.483 +/- 0.181 | 1.084 |
| Chain | 96 kHz stereo | 8 | 0 | 47.535 +/- 1.528 | 44.063 +/- 1.654 | 1.079 |
| Chain | 44.1 kHz stereo | 4 | 1024 | 35.499 +/- 0.337 | 35.526 +/- 0.529 | 0.999 |

Rust is up to 8 percent faster in the multi-plugin chains and equal when latency dominates.
These measurements exclude file decoding and writing and do not establish an end-to-end processing speedup.

## Rust Component Benchmarks

Complete-meter input is ten seconds of stereo music at 44.1 kHz.
File operations use a generated 60-second stereo WAV and start with a current source analysis record.
Unlike CLI normalization, these normalization cases exclude initial source analysis.
Writer-only cases exclude output metering.
Times are Criterion point estimates with 95 percent confidence intervals, in milliseconds.

### macOS

Measured November 2, 2026, on the M4 Pro.
Sorting uses 5000 records, and natural comparison scans adjacent names.

| Case | Estimate, ms | 95 percent interval, ms |
| :--- | ---: | ---: |
| Complete meter, 44.1 kHz stereo | 5.1375 | 5.0948 to 5.1966 |
| Analyze 60-second WAV | 31.791 | 31.545 to 32.104 |
| Normalize WAV, peak | 77.738 | 77.371 to 78.139 |
| Normalize WAV, loudness | 76.966 | 76.402 to 77.488 |
| Gain and AIFF writer, 16 bit | 1.7758 | 1.7690 to 1.7839 |
| Gain and AIFF writer, 24 bit | 1.9635 | 1.9561 to 1.9731 |
| Gain only | 0.023280 | 0.021833 to 0.024787 |
| Natural comparison | 0.034878 | 0.034615 to 0.035140 |
| Sort by name | 6.0061 | 5.9704 to 6.0499 |
| Sort by path | 8.3098 | 8.2593 to 8.3713 |
| Sort by peak | 0.66162 | 0.65792 to 0.66593 |
| Sort by loudness | 0.65319 | 0.65006 to 0.65686 |

### Windows

Measured November 1, 2026, on the Ryzen 9 7950X.
Sorting uses 5000 records, and natural comparison scans adjacent names.

| Case | Estimate, ms | 95 percent interval, ms |
| :--- | ---: | ---: |
| Complete meter, 44.1 kHz stereo | 5.726110 | 5.719597 to 5.736626 |
| Analyze 60-second WAV | 38.228325 | 38.149994 to 38.321970 |
| Normalize WAV, peak | 75.046413 | 74.205293 to 76.202962 |
| Normalize WAV, loudness | 79.796329 | 74.860976 to 85.034327 |
| Gain and AIFF writer, 16 bit | 2.366786 | 2.359607 to 2.374072 |
| Gain and AIFF writer, 24 bit | 2.527422 | 2.522260 to 2.533753 |
| Natural comparison | 0.054017 | 0.053915 to 0.054123 |
| Sort by name | 6.645268 | 6.619916 to 6.670930 |
| Sort by path | 8.231340 | 8.211426 to 8.251712 |
| Sort by peak | 0.934237 | 0.926785 to 0.942741 |
| Sort by loudness | 0.944384 | 0.940087 to 0.949222 |

### Meter Versus Ebur128

Windows measurements from October 31, 2026, compare the complete Rust meter with the full ebur128 reference.
Each input contains ten seconds of audio.
Times are in milliseconds.

| Input | Rust meter, ms | Ebur128, ms | Ebur128 / Rust |
| :--- | ---: | ---: | ---: |
| 44.1 kHz stereo | 6.1924 | 26.1917 | 4.23 |
| 48 kHz stereo | 6.7305 | 28.1049 | 4.18 |
| 88.2 kHz stereo | 12.1193 | 51.1557 | 4.22 |
| 96 kHz stereo | 19.4939 | 54.5689 | 2.80 |
| 192 kHz stereo | 14.8851 | 88.6395 | 5.95 |
| 44.1 kHz mono | 3.1724 | 12.5640 | 3.96 |

## Processing Cost And Memory

Rust combines fourth-order K weighting, energy-window gating, and SIMD true-peak interpolation.
Normalization applies gain and meters the quantized PCM while writing AIFF, without decoding the output again.
VST3 mono/stereo chains process the signal in place instead of copying through separate host buffers.

- The exact final short-term window uses a three-second energy ring, about 1.15 MB at 48 kHz regardless of channel count.
- Hot stereo true-peak construction and destruction take approximately 72 ns at both 48 and 96 kHz.
- Steady-state processing allocates nothing across 600 complete meter blocks,
  and across 599 fused writer/meter blocks at both 16 and 24 bits after scratch buffers are initialized.
  This excludes decoding, construction, metadata, and unknown-length history growth.
- A 60-second WAV profile attributes 85.6 percent of 729 analysis-worker samples to `BlockMeter::measure_planes`,
  and 8.0 percent to PCM decoding.

## Shared-Cache Development Checks

Measured November 2, 2026, on an Apple M3 Max with 16 logical CPUs and 64 GiB memory, running macOS 27.2.
Rust 1.99.1 uses the optimized bench profile with native CPU targeting, LTO, and one code generation unit.
These short checks are not the isolated CLI or VST3 comparisons above.
Other workspace validation was running, so the timings are observations, not a release regression threshold.

The `misc` benchmark stores 500 source identities with simulated ten-minute stereo histories and bounded previews.
The source files are copies of the one-second WAV fixture, not 500 ten-minute recordings.
The Rust-native case uses Rust provenance.
The C++-provenance case relabels the same compatible measurements through the Rust cache API,
so it isolates the foreign-hit read path rather than measuring C++ analysis or decoder differences.
The optional C++ helper reads the original Rust-produced rows with the real C++ cache implementation.

A direct release-executable run used ten Criterion samples with 0.5 seconds each for warmup and measurement.
Point estimates and 95 percent confidence intervals were:

- Rust-native first result on a new connection, 3.846 ms, interval 3.762 to 3.933 ms.
- C++-provenance first result in Rust on a new connection, 3.732 ms, interval 3.667 to 3.811 ms.
- Rust-native retained batch of 500 rows, 22.442 ms, interval 21.460 to 23.349 ms.
- C++-provenance retained batch in Rust, 21.352 ms, interval 21.158 to 21.579 ms.
- C++-provenance retained batch with an independent Rust gain writer, 24.028 ms, interval 23.734 to 24.341 ms.

First-result timing includes opening SQLite, applying pragmas, validating the schema, checking the source identity,
and loading one measurement with its histories.
It excludes closing the connection, process startup, directory discovery, GUI result delivery, and rendering.
Retained batches use an already-open connection and include allocation, validation, and release of the 500 loaded rows.
An earlier short run observed 17.811 ms for native and 17.468 ms for foreign-provenance batches.
Different background load and benchmark setup prevent interpreting the difference as a measured regression.

The real C++ helper accepted all 10,000 reads across 20 passes, averaging 347.105 ms per 500 rows.
An earlier run averaged 302.071 ms.
This helper is a Debug build and releases one record at a time, unlike the optimized retained Rust batch.
Its result validates warm Rust-to-C++ reuse, it is not a fair cross-build speed comparison.

The independent gain writer completed 5583 edits in 1.475 seconds with a maximum worker write latency of 4.998 ms.
The WAL grew from 4,177,712 to 4,803,952 bytes during this contention stage.
An earlier run observed an 18.002 ms maximum and WAL growth from 4,177,712 to 5,574,392 bytes.
These are sampled worker latencies and file sizes, not upper bounds or GUI input-to-persistence latency.

### Background Filling And Retention

A separate probe clears 16 previews, then fills them serially with the production waveform builder and cache writer.
It uses synthetic constant samples for ten-minute stereo timelines, without source decoding or metadata reads.
An independent foreground loop requests a wakeup two milliseconds after each completed probe,
then measures a cache read and explicit gain write separately on the same connection mutex.
This deliberately exercises contention, but it is not the GUI executor or its asynchronous gain writer.

The worker filled all 16 previews in 98.351 ms.
Across 64 idle and 37 overlapping probes, respectively:

- Synthetic wake delay p95 was 0.533 and 0.564 ms, with maxima of 0.938 and 0.652 ms.
- Measurement-read p95 was 0.127 and 0.325 ms, with maxima of 0.254 and 0.558 ms.
- Gain-write p95 was 0.668 and 0.154 ms, with maxima of 1.012 and 0.161 ms.

The independent timing avoids inferring foreground responsiveness from the worker's completion time.
The short sequential baseline and fill stages have different scheduling and filesystem conditions,
so lower gain latency during filling does not establish a speedup.
No interactive playback, selection, scrolling, cancellation, or GUI responsiveness was verified.
No GUI tests or screenshots were used, and Windows shared-cache performance was unavailable.

The retained 500-row batch contains 18,000,000 bytes of required loudness and overload payloads, plus row overhead.
All loaded rows assert that their waveform is absent, excluding 6,476,000 serialized native-preview bytes.
Each generated background preview also passes a weak-reference release check immediately after persistence.
This verifies the benchmark's measurement-only read and release paths, not total GUI retention under every interaction.
Required histories are still retained for every loaded row, they are not claimed to be lazy or constant-memory.

Process-only `/usr/bin/time -l` reported 64,225,280 bytes maximum resident set size, about 61.3 MiB,
and 57,311,760 bytes peak memory footprint, about 54.7 MiB.
The command ran the already-built Rust benchmark executable without the optional C++ subprocess.
It includes fixture setup, SQLite, Criterion, and all benchmark functions' setup, not only the retained batch.
The earlier 1,137,197,056-byte maximum from timing `cargo bench` included compilation,
and must not be reported as cache or application process memory.

### Shared-Cache Reproduction

Run from `rust/` and use the executable path printed by the build, its hash can change.
Build the C++ test helper separately before the interoperability command.
All cache databases and copied inputs are temporary, neither application's user cache is opened.
Avoid overlapping validation jobs when collecting comparable timing results.

```shell
cargo clippy -p audiobatch-core --bench misc -- -D warnings
rustfmt --edition 2024 --check crates/core/benches/misc.rs
cargo bench -p audiobatch-core --bench misc --no-run
bench=./target/release/deps/misc-9423a221e819b0f6
# No Cargo or C++ child process is included in this memory measurement.
env -u AUDIOBATCH_CPP_TESTS /usr/bin/time -l "$bench" --bench cache/shared \
  --sample-size 10 --measurement-time 0.5 --warm-up-time 0.5 --noplot
# Runs the real Rust-to-C++ reader before the Rust benchmarks.
AUDIOBATCH_CPP_TESTS="$PWD/../cpp/cmake-build-mac-debug/AudioBatchTests_artefacts/Debug/AudioBatchTests" \
  "$bench" --bench cache/shared --sample-size 10 --measurement-time 0.5 --warm-up-time 0.5 --noplot
```

## Reproducing the Measurements

Keep the source revision, lockfiles, compiler versions, CPU targeting, corpus, and worker counts with each result.
Use Release builds, avoid concurrent builds or validation, and distinguish warm filesystem caches from cold storage.
The commands below run the current tools, they do not reproduce historical numbers on changed code or hardware.

### CLI Comparison

Install hyperfine and uv, and use the platform build prerequisites in the project READMEs.
Initialize the JUCE submodule and build the C++ CLI separately from `cpp/`:

```shell
cmake --preset macos-release
cmake --build --preset macos-release --target AudioBatchCli
```

On Windows, use `windows-release` instead of `macos-release` and run shell scripts from Git Bash with `cygpath`.
Download the normalization collection with Git LFS before measuring it.
Optionally generate the larger corpus from the repository root with `uv run fixtures/benchmark/generate.py`,
which requires ffmpeg and uv.
Then run from `rust/`:

```shell
./scripts/cli_benchmark.sh --corpus ../fixtures/normalization --jobs 14 --runs 3 --warmup 1
```

Use the measured machine's worker count, such as 32 for the historical Windows comparison.
The default `--corpus auto` also includes generated benchmark audio when present.
The runner builds the Rust Release CLI unless `--no-build` is supplied.
`AUDIOBATCH_CPP_CLI` selects a different C++ executable,
and an unrecognized custom build path also requires `AUDIOBATCH_CPP_BUILD_TYPE=Release`.
Fresh temporary input copies and correctness checks are outside timing.
Raw hyperfine JSON, combined statistics, Markdown, CLI logs,
and machine/build metadata remain in a unique `rust/target/cli-benchmark/` run directory.
HOME, CFFIXED_USER_HOME, and XDG configuration directories are isolated only on a best-effort basis.
Native platform APIs may still use real user storage, which the runner never removes.
Do not treat these environment overrides as guaranteed application-storage isolation.

### In-Memory VST3 Comparison

With CMake, Cargo, uv, the C++ toolchain, and the JUCE submodule available, run from `rust/`:

```shell
./scripts/vst3_benchmark.sh --iterations 30 --repeat 100
```

This builds the Release C++ test executable and Rust example and uses the same real test-plugin bundle in both hosts.
`--iterations` selects measured renders, and `--repeat` repeats each one-second fixture in memory.
Specify both options explicitly to match the historical workload rather than relying on script defaults.
Use `--no-build` only with existing Release executables and bundle.
`--cpp` and `--bundle` override their paths.
The runner retains reports, comparisons, and planar sample exports in `rust/target/vst3-benchmark/`.
It checks independent float arithmetic and final 16-bit PCM outside timing, failing on missing bundles or differences.

### Playback Callback Comparison

`tests/callback_benchmark.py` compares hardware-free production callback runs from explicit Release executables.
Capture a baseline before changing synchronization,
then run baseline and candidate with the same workload and toolchain.
Commands are JSON argument arrays, not shell expressions.
Use `--native-schema cpp` for the C++ version-one seconds reports
and `--native-schema rust` for Rust latency-mode nanosecond reports.
Both preserve the original raw report.
Rust paced/idle CPU reports remain separate evidence and are not converted into invented zero-duration callbacks.
The runner alternates their order, isolates application storage, and retains raw output, binary hashes, and reports.
Supply compiler flags, CPU targeting, hardening, fixture identities, queue sizes, JUCE revision,
and native plugin identity in the metadata objects.
For paired C++ native-wet runs, `AUDIOBATCH_CALLBACK_BENCHMARK_VST3` selects the same frozen repository build-tree bundle.
Installed plugins are never benchmark inputs.
Use identical baseline/candidate commands first to measure baseline-versus-baseline noise.

Each callback report must include raw durations, thread and process CPU, actual audio work, and an output checksum.
A semantic difference is a failure, not a speed improvement.
Cold callbacks, warmed processing, paced idle, contention, and lifecycle transitions are separate workloads.
Keep plugin preparation, buffer allocation, validation, serialization, and disposal outside callback timing.
Do not run builds, sanitizers, or unrelated validation concurrently with performance measurements.

Comparisons resample paired process runs, not individual autocorrelated callbacks.
Positive confidence bounds identify regressions. Bounds overlapping zero are inconclusive, not proof of equivalence.
There is no universal percentage allowance, and a faster average does not excuse worse tails or transition silence.
Callback wall time is not CPU utilization, and software deadline misses are not physical-device underrun measurements.
RTSan is a separate safety gate, not performance evidence or a guarantee for arbitrary hosted plugins.

The first-party callback-admission comparison on Apple Silicon used frozen mutex baseline executables
and alternating Release candidate runs, with the JUCE revision unchanged.
Reports remain in `cpp/cmake-build-mac-release/callback-evidence/` and `rust/target/callback-evidence/`.
The baseline binaries and initial captures remain in `cpp/cmake-build-mac-callback-baseline/`
and `rust/target/callback-baseline/`.
All paired audio-work, frame-count, silence, and checksum checks matched.
Thirteen C++ latency/paced comparisons were inconclusive, with no detected timing regression.
Three initially flagged Rust workloads were repeated with 32 pairs and were inconclusive on repetition.
For 48 kHz, 64-frame dry callbacks, the candidate-minus-baseline p99 bounds were approximately -104 to +98 ns in C++
and -38 to +24 ns in Rust.
These bounds overlap zero. They do not establish equivalence or prove no performance degradation.
The observed variation was accepted as measurement drift for this change.
This engineering acceptance does not change the statistical outcome or the coverage limits below.
Initial Rust regression reports remain retained evidence even though their larger repeats were inconclusive.
Most matrix manifests lack complete compiler/fixture/plugin metadata, so binary hashes alone are not full provenance.
The frozen C++ `idle` workload covers device-not-ready admission, not ready-but-paused rendering.
Earlier Rust transition reports lack a complete per-transition behavioral oracle,
and the initial native-wet fixture flushed parameters before its first measured audio callback.
Treat these captures as historical, limited evidence. Benchmark validation changes require new labelled captures,
not retroactive claims that the earlier runs covered ready-idle, failed transitions, or fully cold native automation.
The revised harness adds separate ready-idle coverage, rejects contention runs without active measured rendering,
verifies Rust transition output against an independent oracle, and leaves native parameter delivery cold.
Its validation-contract identity prevents comparisons against incompatible legacy transition or pacing captures.
Report extraction scans retained diagnostic streams incrementally and rejects JSON markers larger than 64 MiB.
Native Windows execution and performance remain unverified by the macOS metadata-only cross-checks.

### Criterion And Profiling

Generate file inputs first as above, otherwise the file cases skip missing audio.
In-memory cases still run.
From `rust/`, run the component suites or save and compare a named baseline:

```shell
cargo bench -p audiobatch-core
cargo bench -p audiobatch-core --bench analysis -- --save-baseline before
cargo bench -p audiobatch-core --bench analysis -- --baseline before
```

The `analysis`, `decode`, `normalize`, `waveform`, `spectrogram`, and `misc`
targets retain Criterion results under `rust/target/criterion/`.
Use the shared-cache procedure above for its optional C++ helper and process-only memory measurement.
Install samply with `cargo install --locked samply`, then capture from `rust/`:

```shell
cargo build --profile profiling --bin audiobatch-rs
samply record --save-only -o target/profile.json.gz -- ./target/profiling/audiobatch-rs --refresh --jobs 1 ../fixtures/benchmark/output/music_stereo_44100_16.wav
```

The compressed profile is `rust/target/profile.json.gz`.
For C++, point samply at the already-built Release CLI instead.
Sampling is separate from benchmark timing and requires the platform's profiling permissions.

## Accuracy And Validation

The following contracts describe the accuracy checks, not a fresh validation run for every source revision.

- Integrated loudness, maximum short-term loudness, and every history value stay within 0.001 LU of ebur128.
  Integrated and maximum short-term values also match at two displayed decimal places.
- True peak is bit-exact against ebur128, including x86 arithmetic underflow.
  Chunk-size invariance, waveform bytes, sample extrema, and overload history have exact regression checks.
- In-render normalization measurements match an independent decode of the written output.
- Lossless C++/Rust normalization parity is sample-exact in peak mode and allows at most one PCM step in loudness mode.
  Lossy formats use their existing decoder tolerance.

The historical Windows validation reported all 30 C++ suites passing with MSVC
and clang-cl in Debug, Release, and AddressSanitizer builds.
It reported all 920 Rust workspace tests passing in Debug and Release, with three existing skips per run,
and passing clang-tidy, Clippy, and formatting checks.
These counts describe that validation, not the current suite inventory or a result inferred from source inspection.

### Windows And macOS Numerical Differences

The historical Windows x86-64 and macOS arm64 comparison used Rust 1.99.1 and identical source fixtures.
It reported matching identities for all 88 generated inputs and the then-tested 34 committed fixture checksums.
Native floating-point coefficients and eight lossy fixture decodes differed in their last bits.
Within each platform, Debug and Release numerical stages agreed exactly.
These are historical observations, not validation of the current checkout.
The current inventory is defined by `fixtures/checksums.sha256`, including real metadata samples.
Rust now selects `macos-native-aac` goldens on macOS for `fixtures/sine_stereo_44100_aac.m4a`
and `normalization/m4a_stereo_44100_aac_256k_tagged_20s.m4a`.
That decoder-specific snapshot selection means the older lossy-decode comparison cannot establish current AAC equality.

The largest measured absolute differences across the 34 Windows snapshot overrides are:

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
The maximum amplitude difference is half one 24-bit PCM step, or 1/512 of a 16-bit step.
The largest history difference occurs near -143.28 LUFS in the final 192 kHz DC-filter tail.
Displayed loudness values and normalized sample bytes in the golden tests agree.
These results describe the tested corpus, not identical decoding for every lossy input.
