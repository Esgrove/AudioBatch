# Test Fixtures

Small committed audio files that the C++ and the Rust test suites both read in place,
so the two applications are tested against the same audio and must measure the same values.
The C++ tests locate them relative to the source tree, and the Rust tests through the `test-fixtures` crate.
Tests that modify a file work on a copy in a temporary folder, so the committed files never change.

The files are committed, so the tests do not depend on ffmpeg.
Regenerate them with:

```shell
./generate.sh
```

It needs ffmpeg with the libmp3lame encoder, and uv for the Python scripts.
When a fixture changes, update the expected values in the C++ tests and in the Rust `test-fixtures` crate.

## Input Identities

`checksums.sha256` records the exact bytes and inventory of the ordinary fixtures and plugin expected outputs.
Check it from this directory with `sha256sum --check checksums.sha256`.
The Rust `test-fixtures` tests also check it, and reject missing expectations or added, removed, or changed audio files.
Downloaded normalization files are checked against their existing Git LFS SHA-256 object IDs by the decoder diagnostic test.
Their hashes are not duplicated in this manifest.

The Rust in-memory signal corpus has a separate, shared manifest at `../rust/crates/core/tests/golden/input/signals.txt`,
independent of the measurement snapshots.
Its 88 cases cover every generated kind and sample rate, the channel-count corpus, and short block boundaries.
The canonical PCM SHA-256 input is `AudioBatch PCM SHA-256 v1` followed by a NUL byte,
then the sample rate and channel count as little-endian `u32`, the frame count as little-endian `u64`,
and every channel's ordered `f32` sample bits in little-endian, channel-major order.
It preserves signed zero and subnormals, rejects nonfinite samples and unequal planes,
and does not depend on decoder packet boundaries.
The initial manifest was captured on Windows.
Matching macOS verification is still required.
These hashes prove input identity, not measurement correctness.

From `../rust/`, verify inputs and capture diagnostic reports without changing any expectations:

```shell
cargo test -p test-fixtures test_identity --lib
cargo test -p audiobatch-core generated_inputs_have_shared_identities --lib
AUDIOBATCH_DIAGNOSTICS_DIR=target/numerical-diagnostics/debug cargo test -p audiobatch-core numerical_diagnostics_ --lib -- --ignored
AUDIOBATCH_DIAGNOSTICS_DIR=target/numerical-diagnostics/release cargo test --release -p audiobatch-core numerical_diagnostics_ --lib -- --ignored
```

Diagnostic paths are relative to `rust/`, unless an absolute path is supplied.
Reports separate source bytes, stream properties, decoded PCM, coefficient and filter-state bits,
per-frame and block energies, gate membership, pre-log energy, and loudness and history bits.
The decoder report includes whether normalization audio was downloaded and records rejected files and decode notices.
Lossy decoded-PCM hashes are diagnostic evidence, not new platform-specific pass expectations.
Compare matching commits, lockfiles, compiler versions, profiles, and CPU settings before locating the first differing stage.
Repeat with `RUSTFLAGS=""` for the portable CI setting.
This does not disable runtime SIMD dispatch.
The Rust CI matrix captures both profiles and uploads provenance and reports before the output tests.
It does not download normalization audio, so that collection remains an explicit CI coverage gap.
Rust decoder diagnostics support AAC and ALAC.
These must not be run through JUCE on Windows.

## Regeneration Provenance

Installing a newer ffmpeg or Python package does not alter committed fixtures used by existing tests.
Regeneration is an intentional change.
Ffmpeg's bitexact flags do not guarantee identical bytes across encoder versions and builds.
`generate.sh` prints the repository revision, modified generator paths, ffmpeg version and build configuration,
and uv version before writing fixtures.
Retain this output and record the resolved Python interpreter and package versions
when regenerating through Python scripts.
Compare byte hashes and decoded PCM before deciding whether differences are metadata-only or changed audio.
Do not regenerate fixtures or accept new input hashes merely to make measurement failures pass.

After reviewing an intentional input change, capture only the affected manifest test from `../rust/`:

```shell
UPDATE_INPUT_IDENTITIES=1 cargo test -p test-fixtures committed_fixture_bytes_have_shared_identities --lib
UPDATE_INPUT_IDENTITIES=1 cargo test -p audiobatch-core generated_inputs_have_shared_identities --lib
```

Then rerun the verification commands with the capture variable unset, and verify both platforms.
Do not enable capture for the whole helper suite, whose missing and changed manifest tests must fail closed.
`UPDATE_INPUT_IDENTITIES` is separate from `UPDATE_GOLDEN` and never changes measured-output expectations.

## Tones

Unless the name says otherwise, every tone is one second of a 1 kHz sine with a peak amplitude of 0.5,
which is -6.02 dBFS.
Stereo tones carry the same signal on both channels and measure -6.02 LUFS integrated,
and mono tones are analyzed as dual mono, so they measure the same loudness.

The names give the channels, the sample rate, and the bit depth or the bitrate,
such as `sine_stereo_48000_24.wav` or `sine_mono_44100_128k.mp3`.
The tones cover WAV, AIFF, FLAC, MP3, Ogg Vorbis, and AAC in M4A,
at 24, 44.1, 48, and 96 kHz, with 16 and 24 bit integer and 32 bit float samples.

## Special Cases

| File                                     | What it tests                                                             |
| ---------------------------------------- | ------------------------------------------------------------------------- |
| `sine_stereo_44100_aac.m4a`              | AAC priming and padding that only the MP4 edit list records               |
| `sine_stereo_44100_192k_no_xing.mp3`     | An MP3 without the Xing frame, so no header states its delay or length    |
| `sine_joint_stereo_44100_192k.mp3`       | Joint stereo MP3 decoding                                                 |
| `tagged_stereo_44100_16.flac`            | Tag reading and the metadata round trip                                   |
| `sine_3ch_44100_16.aif`                  | Three channels, which processing and the preview refuse                   |
| `intersample_stereo_44100_24.wav`        | Sample peak, true peak, and loudness that all differ                      |
| `click_mono_44100_16.wav` and `click_mono_44100_320k.mp3` | A single click at sample 11025, which shows any shift of the audio start  |
| `serato_rainbow_stereo_44100_*`          | A synthetic rainbow Serato Overview tag in MP3, AIFF, and FLAC            |
| `silence_stereo_44100_16.wav`            | Digital silence, with no peak and no measurable loudness                  |
| `stereo_split_44100_16.wav`              | Different left and right tones, so mid and side both carry audio          |
| `broken.wav`                             | A file with an audio extension that holds no audio                        |

The intersample tone is a quarter sample rate sine whose samples fall 45 degrees from its peaks.
Its samples reach -9.03 dBFS, the waveform between them peaks near -6 dBTP,
and the K-weighting boost at 11 kHz raises its integrated loudness to about -2.7 LUFS.

ffmpeg cannot write Serato tags, so `serato_overview.py` adds the Overview tag the way Serato stores it:
a GEOB frame in an ID3v2 tag for MP3 and AIFF, and a `SERATO_OVERVIEW` comment for FLAC.

## Plugin Expected Outputs

`plugin_expected/` holds the files the `AudioBatch Test MS` plugin in `../test-plugin/` must make of the 16-bit tones,
one file per combination of source, gain, width, and latency.
`plugin_expected.py` computes them with numpy, repeating the plugin's arithmetic in float32 operation by operation,
then flooring to 16 bits without dither the way both applications write processed files.
Regenerate them from the repository root with:

```shell
uv run fixtures/plugin_expected.py
```

The C++ tests in `../cpp/tests/PluginPresetTests.cpp` and the Rust `test-fixtures` crate list the same cases.
The format workflow in CI fails when the committed files differ from what the script writes.

## Normalization Collection

`normalization/` holds a larger collection for comparing the normalized output of both applications.
It has music-like audio in every supported format, sample rate, and bit depth,
from a fifth of a second to 30 seconds long, with a wide range of tags and edge cases,
such as silence, a gain above the 40 dB limit, float samples above full scale, a non-ASCII file name,
and two sources that share one output name.

The audio is stored with Git LFS, so run `git lfs install` once per clone before pulling it.
`normalization/generate.py` regenerates it with ffmpeg and mutagen:

```shell
uv run fixtures/normalization/generate.py
```

`../rust/scripts/parity.sh` and the parity workflow compare the levels both CLIs report
and the files they write when normalizing.
The Rust golden tests also read the collection when the LFS files have been downloaded.

## Benchmark Collection

`benchmark/generate.py` generates 60 second inputs for the Rust Criterion benchmarks into `benchmark/output/`.
The output directory is ignored by git.
Run it from the repository root with:

```shell
uv run fixtures/benchmark/generate.py
```

It needs ffmpeg and uv, keeps existing files, and accepts `--force` to regenerate them.
Music-like stereo signals cover 44.1, 48, 88.2, 96, and 192 kHz as WAV and FLAC at 16 and 24 bits.
The 44.1 and 48 kHz sets also include MP3 at 320 kbps, AAC at 256 kbps, and Ogg Vorbis at quality 5.
Mono sets at 44.1 and 48 kHz cover dual mono measurement, without Ogg because the chosen encoder requires stereo.
White noise covers the FLAC decoding worst case, and a fading signal covers a quiet tail.
The fade is stored as 24 bit PCM, so it reaches digital silence rather than retaining floating-point subnormals.
The in-memory signal tests cover actual subnormal samples.
File benchmarks skip missing inputs with a message, while the in-memory benchmarks always run.

Run the benchmarks from `../rust/` with `cargo bench -p audiobatch-core`.
See `../rust/docs/performance.md` for the measurement procedure and results.
