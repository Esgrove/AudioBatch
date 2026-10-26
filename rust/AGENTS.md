# AGENTS.md

This file governs everything under `rust/`, the Rust port of AudioBatch.
`cpp/AGENTS.md` describes the C++ application in `cpp/`, which is the reference implementation for this port.
Its build, format, lint, and CMake instructions do not apply here.
Its prose style, naming, and documentation rules do.
When the two files disagree about code under `rust/`, this file wins.

## Working Next To The C++ Project

- Read the C++ sources freely when porting behaviour.
  `cpp/src/analysis/AudioAnalysis.cpp`, `cpp/src/analysis/AnalysisCache*.cpp`, and `cpp/src/cli/`
  are the closest counterparts of `crates/core` and `crates/cli`.
- The tests read the shared fixtures in place from the root `fixtures` directory through the `test-fixtures` crate.
  Do not copy fixtures into `rust/`.
- The GUI embeds the Roboto fonts from the root `assets` directory, the same files the C++ application bundles.
  Do not copy them into `rust/` either.
- Stage with explicit paths, for example `git add rust`.
  Avoid `git add -A`, `git add .`, and `git commit -a`, so unrelated work in progress stays out of Rust commits.

**NEVER run destructive git commands**, including `git checkout -- <file>`, `git restore`, `git reset --hard`,
`git clean`, and `git stash drop`.
They can destroy uncommitted work.
Ask the user to do it manually if something needs undoing.

## What It Does

The port covers the key features of the C++ application:

- Analyze audio files for decoded sample peak, true peak, and integrated loudness (EBU R128).
- Cache analysis results in SQLite so unchanged files are not decoded again.
- List the files with their stats in the CLI and the GUI.
- Draw a waveform overview of the selected file in the GUI.
- Play the selected file in the GUI.
- Accept files and folders dropped onto the GUI.

Normalization, AIFF conversion, and VST3 / Audio Unit plugin processing stay in the C++ application for now.

## Workspace Layout

`rust/Cargo.toml` is a virtual workspace.
Shared package metadata, dependency versions, and lints are defined once there,
and every crate inherits them with `.workspace = true`.

- `crates/core` (`audiobatch-core`): the shared library used by the CLI and the GUI.
  - `analysis.rs` decodes a file and measures peaks and loudness, and builds the waveform overview in the same pass.
  - `decode.rs` wraps symphonia: probing, the decode loop, and the read failure rules.
  - `record.rs` holds `AnalysisRecord`, its status, and the short-term loudness history.
  - `waveform.rs` holds the min/max peak overview and its resolution levels.
  - `discovery.rs` expands input paths into the sorted list of supported audio files.
  - `cache.rs` is the SQLite result cache,
    `coordinator.rs` runs analysis in parallel with cancellation and progress events.
  - `format.rs` formats levels and file properties for display, `sorting.rs` sorts records.
  - `tags.rs` reads display tags and Serato data with lofty.
- `crates/playback` (`audiobatch-playback`): the preview engine.
  A decode thread feeds an `rtrb` ring buffer that the `cpal` output callback reads,
  with `rubato` resampling when the device rate differs from the file rate.
  It has no GUI dependency.
- `crates/serato` (`serato-tags`): parsers for the Serato GEOB tags, extracted from the track-rename project.
  They take raw frame bytes and do no terminal printing, so any tag library and front end can use them.
- `crates/cli` (`audiobatch-cli`): the `audiobatch-rs` binary.
  Its options, JSON field names, and exit codes match the C++ `audiobatch` CLI,
  so `scripts/parity.sh` can compare the two directly.
  The `completion` subcommand in `completion.rs` is the only addition the C++ CLI does not have.
- `crates/gui` (`audiobatch-gui`): the `AudioBatchRs` GUI binary, built on gpui-kit.
  It is the only crate that depends on gpui-kit, which is pinned to an exact version and upgraded by hand.
- `crates/test-fixtures`: a dev-only helper that locates the shared fixtures and lists their expected properties.

Keep the core and playback crates free of GUI dependencies,
so the GUI framework stays replaceable without touching them.

## Running Both Versions Side By Side

- The Rust CLI is `audiobatch-rs`, and the Rust app is `AudioBatchRs` with the bundle id `com.esgrove.audiobatch-rs`.
  The C++ versions are `audiobatch` and `AudioBatch` with `com.esgrove.audiobatch`.
- The Rust cache lives in its own data directory, `AudioBatch-rs`.
  Never open the C++ `analysis.db`, because the two schemas would overwrite each other.

## Build And Test Commands

Run all commands from the `rust/` directory.
After making code changes, always run:

```shell
cargo clippy --fix --allow-dirty --all-targets
cargo fmt
cargo nextest run
```

`cargo test` works too.
CI runs `cargo clippy --all-targets -- -D warnings`, `cargo fmt --check`, and the tests on macOS and Windows.

Other commands:

```shell
# Run the CLI
cargo run --bin audiobatch-rs -- [args]

# Run the GUI
cargo run --bin AudioBatchRs

# Compare the Rust and C++ CLI output on a folder
./scripts/parity.sh <path>

# Save an image of the GUI window and quit, for checking the layout without screen recording permission
cargo build --release -p audiobatch-gui --features snapshot
AUDIOBATCH_GUI_SNAPSHOT=/tmp/gui.png ./target/release/AudioBatchRs --autoplay ../fixtures
```

The `snapshot` feature also reads `AUDIOBATCH_GUI_SNAPSHOT_DELAY` in seconds,
and `AUDIOBATCH_GUI_SNAPSHOT_ACTIONS` with action names such as `audiobatch::ToggleLoudnessGraph`
that are dispatched before the image is taken.
The GUI follows the look of the C++ application, so compare it with `cpp/src/gui/` when changing the layout.

Dependencies are compiled with optimizations even in debug builds, because decoding is too slow otherwise.

## Code Organization

- All enums before structs.
- Put all struct definitions before any implementations.
- Implementations only after the last struct definition, in the order of the struct definitions.
- Functions after implementations.
- In implementations, put constructors first:
  `new` first, then other associated functions that return `Self` (`default`, `from_*`, and similar).
- After constructors, order public methods before private methods.
- Split a source file that passes about 1000 lines of non-test code along its concerns,
  and give each new file its own `//!` documentation.
- Keep an item's visibility as narrow as possible, for example `pub(crate)` instead of `pub`.
- All `use` imports go at the top of the file, never inside functions.

## Code Style and Conventions

- Rust 2024 edition, with clippy pedantic and nursery lints denied.
- Do not use plain `unwrap`. Use proper error handling, or `.expect()` in constants and tests.
- Use `anyhow` with `Result<T>` for errors in application code.
- Use `clap` with derive macros. Every CLI argument has both a short and a long option,
  except where the C++ CLI has only a long one, which the port matches.
- Use the `colored` crate for terminal colors.
- Use descriptive names. No single character variables.
- Prefer full names over abbreviations, for example `directories` instead of `dirs`.
  Established domain terms such as `lufs`, `db`, and `id` are fine.
- Give every struct, enum, and function a docstring that explains what it does and why,
  including threading constraints and non-obvious behaviour.
- Every Rust source file starts with module-level `//!` documentation describing its purpose.
- Avoid trailing comments. Put comments on their own line above the code.
- Compare floating point values for equality with a tolerance, not with `==`.
- The audio callback in `crates/playback` must not allocate, lock, or block.

### Comments and documentation formatting

Use semantic line breaks for comments, docstrings, and documentation.
Start a new line at sentence boundaries or natural clause boundaries.
Target about 120 characters per line, but prefer a clean sentence break over a strict width.
Avoid dashes and semicolons in prose. Use commas and periods to separate ideas.
Run `slb` in `rust/` to check the prose, and `slb --fix` to reformat it.

## Testing

- Only add lightweight, focused unit tests that complete in under one second.
  Do not add GUI tests or rendering-based assertions.
- Test the core and the CLI against the shared fixtures through `test-fixtures`.
  Tests that modify files work on copies in a temporary directory.
- **NEVER use nested modules inside test modules.**
  All test modules are separate root-level `#[cfg(test)]` modules, never wrapped in a single parent `mod tests`.
- Group related tests into modules of about three to five tests or more, named by area, such as `test_peaks`.
