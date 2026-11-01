# Performance

Rust analysis is 5.4 to 5.5 times faster than C++ on the measured macOS corpus,
and 4.9 to 5.3 times faster on the larger Windows corpus.
Single-worker normalization is 3.5 to 3.7 times faster on both machines.
VST3 processing is closer, with the result depending on the C++ compiler and plugin chain.

CLI and VST3 times are means with sample standard deviations.
The C++ / Rust ratio divides the C++ mean by the Rust mean.
Ratios above one favor Rust, and ratios below one favor C++.
Measurements use warm filesystem caches and sequential runs without concurrent builds or tests.
Different hardware, corpora, and platform decoders prevent an isolated operating-system comparison.

## macOS CLI

Measured October 30, 2026, on an Apple M3 Max with 16 logical CPUs and 64 GiB memory, running macOS 27.2.
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
| 1 | Analysis | 1.618250 ± 0.014608 | 0.297451 ± 0.007325 | 5.440 |
| 1 | Peak normalization, 0 dBFS | 3.277709 ± 0.054053 | 0.884520 ± 0.011951 | 3.706 |
| 1 | Loudness normalization, -14 LUFS | 3.270517 ± 0.040099 | 0.928473 ± 0.023419 | 3.522 |
| 16 | Analysis | 0.310800 ± 0.003239 | 0.056582 ± 0.001800 | 5.493 |
| 16 | Peak normalization, 0 dBFS | 0.594622 ± 0.011371 | 0.285083 ± 0.002519 | 2.086 |
| 16 | Loudness normalization, -14 LUFS | 0.595915 ± 0.019191 | 0.293995 ± 0.004707 | 2.027 |

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

## Rust Component Benchmarks

Complete-meter input is ten seconds of stereo music at 44.1 kHz.
File operations use a generated 60-second stereo WAV and start with a current source analysis record.
Unlike CLI normalization, these normalization cases exclude initial source analysis.
Writer-only cases exclude output metering.
Times are Criterion point estimates with 95 percent confidence intervals, in milliseconds.

### macOS

Measured October 30, 2026, on the M3 Max.

| Case | Estimate, ms | 95 percent interval, ms |
| :--- | ---: | ---: |
| Complete meter, 44.1 kHz stereo | 5.9701 | 5.9556 to 5.9906 |
| Analyze 60-second WAV | 41.396 | 41.275 to 41.510 |
| Normalize WAV, peak | 84.876 | 84.551 to 85.178 |
| Normalize WAV, loudness | 84.826 | 84.364 to 85.417 |
| Gain and AIFF writer, 16 bit | 2.1915 | 2.1822 to 2.2056 |
| Gain and AIFF writer, 24 bit | 3.2381 | 3.2310 to 3.2463 |
| Gain only | 0.021840 | 0.021706 to 0.022057 |

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

## Accuracy And Validation

- Integrated loudness, maximum short-term loudness, and every history value stay within 0.001 LU of ebur128.
  Integrated and maximum short-term values also match at two displayed decimal places.
- True peak is bit-exact against ebur128, including x86 arithmetic underflow.
  Chunk-size invariance, waveform bytes, sample extrema, and overload history have exact regression checks.
- In-render normalization measurements match an independent decode of the written output.
- Lossless C++/Rust normalization parity is sample-exact in peak mode and allows at most one PCM step in loudness mode.
  Lossy formats use their existing decoder tolerance.

On Windows, MSVC and clang-cl each pass all 30 C++ suites in Debug, Release, and AddressSanitizer builds.
Rust passes all 920 workspace tests in both Debug and Release, with three existing skips per run.
Full clang-tidy, Clippy, and formatting checks pass.

### Windows And macOS Numerical Differences

The Windows x86-64 and macOS arm64 measurements use Rust 1.99.1 and identical source fixtures.
All 88 generated input identities and 34 committed fixture checksums match.
Native floating-point coefficients and eight lossy fixture decodes differ in their last bits.
Within each platform, Debug and Release numerical stages agree exactly.

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
