# AudioBatch

Audio analysis and batch processing tools for macOS and Windows.
Measure sample peak, true peak, and integrated loudness (EBU R128), cache results in SQLite,
and normalize or process files through plugin chains.
Both applications provide a GUI with waveform and spectrogram previews, artwork, and playback, plus a CLI.

## Projects

- [`cpp/`](cpp/README.md): the C++23 and JUCE reference application, `AudioBatch` and `audiobatch`.
  Hosts VST3 effects and, on macOS, Audio Units.
- [`rust/`](rust/README.md): the Rust application, `AudioBatchRs` and `audiobatch-rs`, built on gpui-kit.
  Hosts VST3 effects on both supported platforms.
- [`image-control-plugin/`](image-control-plugin/README.md): ImageControl, an independent four-band stereo imaging effect.
  Available as VST3 and, on macOS, an Audio Unit.

The applications share `AudioBatch/analysis.db`, but keep settings, logs, plugin lists, chains, and presets separate.
Their CLIs use matching analysis and normalization options and table, CSV, and JSON output.

Shared resources include [`assets/`](assets), [test fixtures](fixtures/README.md),
and the [test VST3 plugin](test-plugin/README.md).
See [Performance](docs/performance.md) for dated measurements and reproduction instructions.

## Build And Install

Follow each project's README for prerequisites.
The root wrappers build both applications and install them alongside C++ ImageControl and Rust ImageControl RS:

```shell
./build.sh
./install.sh
./format.sh --check
```

Select an application to run just its script, for example `./build.sh rust` or `./install.sh cpp --app-only`.
Use `--debug` for Debug application builds and each script's `--help` for its options.
Options passed to both applications must be supported by both scripts.

## Formats And Compatibility

Both applications read WAV, AIFF, FLAC, and MP3 and write normalized or processed output as AIFF.
C++ also registers Ogg Vorbis and uses Core Audio for AAC and Apple Lossless on macOS.
Rust reads AAC and Apple Lossless on both platforms, using a native macOS backend for selected AAC streams.
See the project READMEs for format boundaries.

Lossless normalization is sample-exact between implementations in peak mode
and differs by at most one PCM step in loudness mode.
Lossy decoding can differ.
Plugin chains and presets are implementation-specific and cannot be shared.
Only plugin scanning is isolated in a worker process.
A plugin crash or hang during processing, playback, or editing can still affect the application.

## File Safety

**Normalization and plugin processing replace originals by default.**
Replaced files go to the system trash unless the CLI uses `--delete`.
Existing output is protected unless overwrite is enabled.
Cancellation preserves uncommitted originals and keeps completed output.
Both CLIs handle Ctrl-C and exit with status 130.

## Development

Run project build and test scripts from their directories.
Rust Cargo commands also work at the repository root, sharing [workspace dependencies](Cargo.toml) and `rust/target`.
For example, `cargo check -p audiobatch-cli` or `cargo test -p serato-tags`.
Read [repository agent instructions](AGENTS.md) and the project's own instructions before making changes.
Implementation details are in the [C++ architecture](cpp/ARCHITECTURE.md)
and [Rust architecture](rust/ARCHITECTURE.md).
