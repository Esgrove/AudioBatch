# AudioBatch

Tools for analyzing and batch processing audio files.
They measure sample peak, true peak, and integrated loudness (EBU R128),
cache the results in SQLite, and list them in a GUI and a CLI.

The repository holds two versions of the application side by side:

- [`cpp/`](cpp/README.md): the C++23 and JUCE application, the `AudioBatch` GUI and the `audiobatch` CLI.
  It is the reference implementation and the full-featured version,
  with normalization, AIFF conversion, and VST3 / Audio Unit plugin processing.
- [`rust/`](rust/README.md): the Rust port, the `AudioBatchRs` GUI built on gpui-kit and the `audiobatch-rs` CLI.
  It covers analysis, the results table, the waveform overview, and playback.

Both versions run side by side.
They keep separate analysis caches, and their CLIs print the same table, CSV, and JSON output.

## Layout

```text
AudioBatch/
├── assets/      icons and fonts for both applications
├── fixtures/    committed audio fixtures shared by both test suites, and generate.sh
│   └── normalization/  a Git LFS collection for comparing the normalized output of both CLIs
├── cpp/         the C++ project with its CMake build and the JUCE submodule
└── rust/        the Rust Cargo workspace
```

Build and test each version from its own directory, as described in its README.

## Future work

- [ ] Show the embedded album art of the selected file in the info panel, in both the C++ and the Rust application.
