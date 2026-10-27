# AGENTS.md

This file governs everything under `rust/`, the Rust version of AudioBatch.
`cpp/AGENTS.md` describes the C++ application in `cpp/`, a sibling implementation that is still in progress.
Its build, format, lint, and CMake instructions do not apply here.
Its prose style, naming, and documentation rules do.
When the two files disagree about code under `rust/`, this file wins.

## Working Next To The C++ Project

- Read the C++ sources freely to understand how a feature behaves,
  but question anything that looks wrong before carrying it over, because that application is not finished either.
  `cpp/src/analysis/AudioAnalysis.cpp`, `cpp/src/analysis/AnalysisCache*.cpp`, and `cpp/src/cli/`
  are the closest counterparts of `crates/core` and `crates/cli`.
- The tests read the shared fixtures in place from the root `fixtures` directory through the `test-fixtures` crate.
  Do not copy fixtures into `rust/`.
- The GUI embeds the Roboto fonts from the root `assets` directory, the same files the C++ application bundles,
  and uses the same app icons from there.
  Do not copy them into `rust/` either.
- Stage with explicit paths, for example `git add rust`.
  Avoid `git add -A`, `git add .`, and `git commit -a`, so unrelated work in progress stays out of Rust commits.

**NEVER run destructive git commands**, including `git checkout -- <file>`, `git restore`, `git reset --hard`,
`git clean`, and `git stash drop`.
They can destroy uncommitted work.
Ask the user to do it manually if something needs undoing.

## What It Does

The Rust version covers the key features of the C++ application:

- Analyze audio files for decoded sample peak, true peak, and integrated loudness (EBU R128).
- Cache analysis results in SQLite so unchanged files are not decoded again.
- List the files with their stats in the CLI and the GUI.
- Draw a waveform overview of the selected file in the GUI,
  coloured from the Serato Overview tag when the file has one.
- Cache the Serato Overview of each file in a `serato_tag_cache` table with the same schema as the C++ cache.
- Play the selected file in the GUI.
- Keep a per-file custom gain in the GUI and the cache, applied to the preview and the waveform.
- Accept files and folders dropped onto the GUI.
- Normalize files from the CLI and the GUI to a sample peak or loudness target,
  written as AIFF with tags and Serato data.
  Lossless sources produce the same samples as the C++ application.

VST3 / Audio Unit plugin processing stays in the C++ application for now.

## Workspace Layout

`rust/Cargo.toml` is a virtual workspace.
Shared package metadata, dependency versions, and lints are defined once there,
and every crate inherits them with `.workspace = true`.

- `crates/core` (`audiobatch-core`): the shared library used by the CLI and the GUI.
  - `analysis.rs` decodes a file and measures peaks and loudness, and builds the waveform overview in the same pass.
  - `decode.rs` wraps symphonia: probing, the decode loop, and the read failure rules.
    It trims the gapless padding the way the decoders of the C++ application do, so both measure the same samples,
    reading the MP4 edit list in `decode/gapless.rs`, which symphonia parses but never applies,
    and leaving out the MP3 synthesis delay when no LAME header states the encoder delay.
  - `record.rs` holds `AnalysisRecord`, its status, and the short-term loudness history.
  - `waveform.rs` holds the min/max peak overview and its resolution levels.
  - `discovery.rs` expands input paths into the sorted list of supported audio files.
  - `cache.rs` is the SQLite result cache, with its Serato tag table in `cache/serato.rs`,
    `coordinator.rs` runs analysis in parallel with cancellation and progress events,
    and `normalize_coordinator.rs` does the same for normalization, keeping the cache in step with the outputs.
  - `format.rs` formats levels and file properties for display, `sorting.rs` sorts records.
  - `tags.rs` reads display tags and Serato data with lofty, and tells what the Serato Overview read found.
    Serato data comes from the `GEOB` frames of MP3, AIFF, and WAV files, and the `SERATO_*` comments of FLAC files.
  - `normalize.rs` computes the normalization gain and writes the normalized file.
    `aiff.rs` is its AIFF writer, `metadata.rs` copies the tags onto the output,
    and `output_file.rs` commits the output and disposes of the replaced files.
- `crates/playback` (`audiobatch-playback`): the preview engine.
  A decode thread feeds an `rtrb` ring buffer that the `cpal` output callback reads,
  with `rubato` resampling when the device rate differs from the file rate.
  It has no GUI dependency.
- `crates/serato` (`serato-tags`): parsers for the Serato GEOB tags, extracted from the track-rename project.
  They take raw frame bytes and do no terminal printing, so any tag library and front end can use them.
  `overview.rs` parses the Overview tag strictly and defines its cache representation,
  and `colouring.rs` decodes the palette indices and computes the column colours of the waveform.
- `crates/cli` (`audiobatch-cli`): the `audiobatch-rs` binary.
  Its options, JSON field names, and exit codes match the C++ `audiobatch` CLI,
  so `scripts/parity.sh` can compare the two directly.
  With `--normalize`, `scripts/compare_outputs.py` also compares the files both CLIs wrote,
  sample by sample and ID3v2 frame by frame, so a change to decoding or tag copying must keep them in agreement.
  The `completion` subcommand in `completion.rs` is the only addition.
- `crates/gui` (`audiobatch-gui`): the `AudioBatchRs` GUI binary, built on gpui-kit.
  It is the only crate that depends on gpui-kit, which is pinned to an exact version and upgraded by hand.
  Its `build.rs` embeds the app icon from the root `assets` directory into the Windows executable.
  On macOS, `build.sh` and `install.sh` package it as `AudioBatchRs.app` through `create_mac_app_bundle` in `common.sh`.
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
CI runs `cargo clippy --all-targets -- -D warnings`, `cargo fmt --check`, and the tests on macOS and Windows,
and a coverage run on macOS.

Other commands:

```shell
# Build the release binaries into rust/, or install them and the CLI shell completions.
# On macOS, the GUI becomes AudioBatchRs.app, and install.sh puts it in /Applications.
./build.sh
./install.sh

# Run the tests with coverage and print the summary, add --open for the HTML report
./coverage.sh
cargo llvm-cov nextest --workspace

# Run the CLI
cargo run --bin audiobatch-rs -- [args]

# Run the GUI
cargo run --bin AudioBatchRs

# Compare the Rust and C++ CLI output on a folder, or their normalized output of copies of it
./scripts/parity.sh <path>
./scripts/parity.sh --normalize [--peak <dBFS> | --lufs <LUFS>] ../fixtures/normalization

# Save an image of the GUI window and quit, for checking the layout without screen recording permission
cargo build --release -p audiobatch-gui --features snapshot
AUDIOBATCH_GUI_SNAPSHOT=/tmp/gui.png ./target/release/AudioBatchRs --autoplay ../fixtures
```

The `snapshot` feature also reads `AUDIOBATCH_GUI_SNAPSHOT_DELAY` in seconds,
and `AUDIOBATCH_GUI_SNAPSHOT_ACTIONS` with action names such as `audiobatch::ToggleLoudnessGraph`
that are dispatched before the image is taken.
The GUI shares its palette, fonts, and icons with the C++ application,
so both versions look alike, and `cpp/src/gui/` is worth comparing when changing the layout.

Dependencies are compiled with optimizations even in debug builds, because decoding is too slow otherwise.
`.cargo/config.toml` compiles for the native CPU, and CI clears it with an empty `RUSTFLAGS`.
The shell scripts in `rust/` source `common.sh`, which reads the binary names from the crate manifests
and sources `../common.sh` for the platform detection and print helpers shared with `cpp/`.

## Code Organization

Order the items of a source file like this:

1. Constants and statics.
2. Enums, public before private.
3. Structs, public before private.
4. Implementations, in the order the types are defined.
   A type's trait implementations directly follow its inherent `impl` block.
5. Functions, public before private.
6. Root-level test modules.

Within an `impl` block:

- Constructors come first.
  `new` comes first when there is one,
  then the other associated functions that return `Self`, `Result<Self>`, or `Arc<Self>`, such as `from_*` and `open_*`.
- After the constructors, public methods come before private ones.
  Within each visibility, methods that take `self` come before associated functions.

Module boundaries:

- About 1000 lines of non-test code is the signal to look for separate concerns in a file, not a hard limit.
  A long test section or one tightly coupled implementation does not need a split.
  Give each new file its own `//!` documentation naming its concern.
- Put a helper that is not specific to one front end in `crates/core`, never in the CLI or the GUI crate.
- Keep an item's visibility as narrow as possible.
  In the library crates, prefer `pub(crate)` or `pub(super)` over `pub`.
  The binary crates use `pub` for items shared between their modules,
  because the nursery lint `redundant_pub_crate` rejects `pub(crate)` there.
- All `use` imports go at the top of the file, never inside functions.
  Imports used only on some platforms go there too, behind the same `#[cfg]` as the code using them.

## Code Style and Conventions

- Rust 2024 edition, with clippy pedantic and nursery lints denied.
- Do not use plain `unwrap`. Use proper error handling, or `.expect()` in constants and tests.
- Use `anyhow` with `Result<T>` for errors in application code.
- Use `clap` with derive macros. Every CLI argument has both a short and a long option,
  except where the C++ CLI has only a long one, because both CLIs take the same arguments.
- Use the `colored` crate for terminal colors.
- After changing CLI arguments, update the usage in `README.md` from the short help,
  `cargo run --bin audiobatch-rs -- -h`.
- Use descriptive names. No single character variables, except `x` and `y` for plot and screen coordinates.
- Prefer full names over abbreviations, for example `directories` instead of `dirs`.
  Established domain terms such as `lufs`, `db`, and `id` are fine,
  and so are the names the frameworks use throughout, such as gpui's `cx` and `window` and clap's `args`.
- Give every struct, enum, and function a docstring that explains what it does and why,
  including threading constraints and non-obvious behaviour.
  Methods implementing a trait need none, since the trait documents them.
- Silence a lint with `#[expect(lint, reason = "...")]` rather than `#[allow]`,
  so the attribute goes away when it is no longer needed.
- Every Rust source file starts with module-level `//!` documentation describing its purpose.
- Avoid trailing comments. Put comments on their own line above the code.
- Compare floating point values for equality with a tolerance, not with `==`.
- The audio callback in `crates/playback` must not allocate, lock, or block.

### Comments and documentation formatting

Use semantic line breaks for comments, docstrings, and documentation.
Start a new line at sentence boundaries or natural clause boundaries.
Target about 120 characters per line, but prefer a clean sentence break over a strict width.
Avoid dashes and semicolons in prose. Use commas and periods to separate ideas.
Quote literal user input values, for example "y" and "n".
Do not wrap prose by splitting a sentence at an arbitrary point in the middle.
Run `slb` in `rust/` to check the prose, and `slb --fix` to reformat it.

Comments, docstrings, and documentation describe the current behaviour and why it is so.
They contain no development history and no notes about porting,
such as "ported from `PluginProcessing.cpp`", "follows the C++ panel", or "like the C++ application".
Mention the C++ application only when it matters for the implementation:

- A requirement shared with it, stated with its reason.
  Examples are the CLI options, messages, and exit codes that `scripts/parity.sh` compares,
  the normalized output that `scripts/compare_outputs.py` compares, the shared fixtures and assets,
  and the VST SDK version kept in step with JUCE's.
- A deliberate difference, with its reason.

## Testing

- Only add lightweight, focused unit tests that complete in under one second.
  Do not add GUI tests or rendering-based assertions.
- Test the core and the CLI against the shared fixtures through `test-fixtures`.
  Tests that modify files work on copies in a temporary directory.
- **NEVER use nested modules inside test modules.**
  All test modules are separate root-level `#[cfg(test)]` modules, never wrapped in a single parent `mod tests`.
- Group related tests into modules of about three to five tests or more, named by area, such as `test_peaks`.
  Avoid one-test modules by merging them into a related module,
  but a file with only a few tests can keep them in a single module.
  Split a module that grows past a few hundred lines by area.
- Put test helpers used by several modules in a `#[cfg(test)]` module declared from the crate root,
  such as `crates/core/src/test_helpers.rs`.
  Helpers that only locate fixtures or list their properties belong in `test-fixtures`,
  which cannot call into the core crate.
