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

The unit tests do not read it.
Only `../rust/scripts/parity.sh` and the parity workflow do,
comparing the levels both CLIs report and the files they write when normalizing.
