# AGENTS.md

AudioBatch is a JUCE-based C++23 application for analyzing and batch processing audio files.
It builds two deliverables from a shared core:

- GUI target: `AudioBatch` producing `AudioBatch.app` / `AudioBatchApp.exe`. This is the primary deliverable.
- CLI target: `AudioBatchCli` producing `audiobatch`.

The GUI is the primary interface and the main focus of development.
The CLI is a thin console front end over the same analysis and normalization core,
so changes to shared services must keep both targets working.

## What It Does

- Analyze audio files for decoded sample peak, true peak, and integrated loudness (EBU R128).
- Cache analysis results in SQLite so unchanged files are not re-read on later runs.
- Show sortable peak, true peak, and loudness data in a GUI results table with per-file status.
- Normalize selected files to a target sample peak or integrated loudness from the GUI or the CLI.
- Convert normalized output to AIFF while preserving source metadata,
  including embedded album art and custom tags, written back as ID3v2.4.
- Batch process selected files through a chain of VST3 or Audio Unit plugins from the GUI.

Analysis assumes mono or stereo sources, including MP3 joint stereo.
Multichannel files are not a target.

## Source Layout

The application code lives in `src/`.
The two targets share a core set of sources.
The CLI adds its front end files, and the GUI adds its components and plugin hosting.

Entry points:

- `src/Main.cpp` is the GUI application entry point.
- `src/CliMain.cpp` is the CLI entry point.

CLI only:

- `src/AudioAnalysisCli.{h,cpp}` holds CLI argument parsing and output formatting.
- `src/PluginListCli.{h,cpp}` implement the CLI plugin listing and rescan.

Shared core (linked into both targets):

- `src/AudioAnalysisService.*` and `src/AudioAnalysisTypes.h` perform audio analysis.
- `src/AudioNormalizationService.*` handle normalization and AIFF conversion.
- `src/MetadataService.*` read and write tags and embedded art through TagLib.
- `src/AnalysisCache.*` manage the SQLite result cache.
- `src/AnalysisCoordinator.*` and `src/NormalizeCoordinator.*` orchestrate background work.
- `src/utils.*`, `src/StringFormat.h`, and `src/version.h` are shared helpers.
- `src/ConsolePrint.*` provide styled console output helpers.
  They are used mainly by the CLI, and `utils.h` includes them in both targets.
- `src/PluginScanner.*` implement out-of-process plugin scanning and the known-plugins list persistence.
  Each executable relaunches itself as its scan worker:
  `src/Main.cpp` and `src/CliMain.cpp` detect the worker command line before doing anything else.
  The CLI links `juce_audio_processors` for this, which also pulls in the JUCE GUI modules.

GUI only:

- `src/AudioBatchComponent.*` is the main GUI component.
- `src/AudioFileTableModel.*` backs the sortable results table.
- `src/CustomLookAndFeel.*`, `src/ThumbnailComponent.*`, and `src/IntervalStepSlider.h` are UI pieces.
- `src/PluginChain.*`, `src/PluginChainEditor.*`, `src/PluginProcessing.h`, `src/PluginProcessingCoordinator.*`,
  and `src/PluginProcessingService.*` implement VST3 / Audio Unit plugin hosting and the plugin chain.

The source lists are defined in `CMakeLists.txt`.
`AUDIOBATCH_CORE_SOURCES` holds the shared core and `AUDIOBATCH_CLI_SOURCES` the CLI front end,
and each target adds its own entry point and target-specific files.
Add new shared files to the core list, not to each target,
and remember that a shared file must compile cleanly for both the GUI and the CLI.
List sources as relative `src/...` paths,
because the strict warning set is applied by matching that pattern.

Tests:

- `tests/` holds the `AudioBatchTests` executable, which links the shared core and the CLI front end.
  Suites are `juce::UnitTest` classes, one per file, created through the factories in `tests/TestSuites.h`.
- `tests/fixtures/` holds small committed audio files: one second 1 kHz tones at -6.02 dBFS
  in WAV, AIFF, FLAC, MP3, and Ogg, mono and stereo, at several sample rates and bit depths,
  plus a tagged FLAC file, digital silence, and a broken file.
  `tests/fixtures/generate.sh` regenerates them with ffmpeg.
  Tests that modify files work on copies in a temporary directory.

## Dependencies

- JUCE, vendored under `JUCE/` as a submodule.
- SQLite 3, resolved via CMake (system package if present, otherwise the fetched amalgamation).
- libebur128, fetched via CMake, for loudness and true peak analysis.
- TagLib, fetched via CMake, for cross-format metadata handling.
- {fmt}, fetched via CMake, for string formatting (see `src/StringFormat.h`).

## Working Rules

- Prefer modern C++23 coding standards and idioms.
- Do not edit vendored code under `JUCE/` unless the task explicitly requires it.
- Prefer fixing warnings and build issues in project code or project configuration before considering third-party changes.
- Preserve the existing source layout under `src/` and the current naming used in `CMakeLists.txt`.
- Changes to shared services must keep both the GUI and CLI targets building and behaving correctly.

## Code Style

- Every source and header file must begin with a file-level comment section
  that explains on a high level what the file contains and implements.
  Use a `///` comment block at the very top of the file,
  before `#pragma once` and the includes.
  A few sentences is enough: name the main classes or functions
  and describe their role in the application.
  In a `.cpp` file, focus on what the implementation covers
  instead of repeating the header text verbatim.
- Every function must have a docstring (`///` doc comment).
  Focus on what the function does and why, and document non-obvious behaviour,
  side effects, threading constraints, and ownership rules.
  Do not simply restate the signature in prose.
  Document a function where it is declared:
  on the declaration in the header for class members,
  and on the definition for free and file-local functions in `.cpp` files.
- Prefer full names instead of abbreviations in identifiers,
  for example `directories` instead of `dirs` and `description` instead of `desc`.
  Established domain terms and API names (`midi`, `xml`, `id`, `dB`) are fine.
- Avoid single character variable names.
  They are only allowed in for loops when it makes sense
  and it is clear what the variable is, for example a plain loop index.
- Prefer `utils::format` (fmt-based, in `src/StringFormat.h`)
  over chained `juce::String` concatenation when building display and log text.
  Format strings must be literals so fmt's compile-time checking applies.
  Keep `.quoted()` calls on arguments instead of putting quote characters in the format string,
  and keep `juce::newLine` outside format strings in text that leaves the process,
  because it is `\r\n` on Windows.
- Write console output with `fmt::print` or the `console::` helpers in `src/ConsolePrint.h`.
  `std::format` and `std::print` are also fine.
  Prefer these over `std::cout` / `std::cerr`,
  unless the iostream version is clearly more readable and performant with much less code.
  Use `console::printError` and `console::printWarn` for user-facing error and warning messages.
  Avoid `fmt::println`, because it triggers MSVC warning C4702 inside fmt with warnings as errors.
- The logging helpers accept fmt format strings and arguments directly:
  `utils::logError("Analysis failed for {}: {}", path.quoted(), message)`.
  Do not wrap the message in a `utils::format` call first.

## Build And Verification

- Do not use `build.sh`, it is only for making final release builds.
- When modifying code, use the CMake debug presets for validation
  so terminal builds reuse the same build directory as the editor or IDE.
- On macOS, use `cmake --preset macos-debug` then `cmake --build --preset macos-debug`.
- On Windows, prefer `cmake --preset windows-debug` then `cmake --build --preset windows-debug`.
- If you need Ninja and `compile_commands.json` on Windows,
  use `cmake --preset windows-ninja-debug` then `cmake --build --preset windows-ninja-debug`.
- Both `AudioBatch` and `AudioBatchCli` build with warnings as errors, so all warnings must be resolved.
- On top of the JUCE recommended warning flags,
  `audiobatch_set_source_warnings` in `CMakeLists.txt` adds a stricter warning set
  to the project's own `src/*.cpp` files only.
  JUCE module sources do not get it,
  and the JUCE headers are included as system headers so their warnings are not reported.
  Several of those flags are Clang-specific, so GCC is not a supported compiler.
  `src/Main.cpp` additionally gets `-Wno-cast-qual`,
  because the `START_JUCE_APPLICATION` macro casts away `const` in code the project cannot change.

## Testing

- Build the debug preset and run `ctest --test-dir cmake-build-mac-debug --output-on-failure`,
  or run the `AudioBatchTests` executable directly for the full test log.
- Add tests for new behaviour in the shared core and the CLI parsing.
  Prefer the committed fixtures over generating audio in a test.
- Avoid tests that convert non-AIFF sources through normalization,
  because a successful conversion moves the source to the system trash.
- `tests/.clang-tidy` disables `readability-magic-numbers`, since tests compare against literal expected values.

## Formatting

- After changing any C++ source or header file, run `clang-format -i --style=file` on each changed file before finishing.
- For repo-wide or multi-file formatting, use `./format.sh`.
  `./format.sh --check` only verifies formatting without changing files.
- C++ formatting is configured by `.clang-format`.
- `./format.sh` also formats `CMakeLists.txt` with cmake-format, configured by `.cmake-format.yaml`,
  and the shell scripts with shfmt when it is installed.
  cmake-format runs through `uvx` when it is not installed.
  After changing `CMakeLists.txt`, run `./format.sh` or cmake-format on it.
- `.pre-commit-config.yaml` runs clang-format, cmake-format, and basic file checks through [prek](https://prek.j178.dev).
- CI runs `./format.sh --check`.

## Linting

- After any C++ code change, run `./lint.sh` to check the project with `clang-tidy`.
  Fix any issues it reports before finishing.
  The script uses the active CMake debug build's `compile_commands.json`,
  so configure a debug preset first if no build directory exists.
- `./lint.sh --check` exits with code 1 when clang-tidy reports any warning.
  CI runs it on macOS with the latest Homebrew LLVM, so the build must stay free of clang-tidy warnings.
- If `clang-tidy` reports a check that is genuinely a poor fit for the project,
  disable it explicitly in `.clang-tidy` and add a short comment explaining why.
  Do not silence individual warnings with inline comments.

## Prose Style (Comments, Docstrings, Markdown)

Use [semantic line breaks](https://sembr.org/) for all prose written in this repository:
code comments, docstrings, commit messages, and Markdown files.

- Target a soft line length of about 120 characters.
  If a sentence (or short run of sentences) fits within that, leave it on one line.
  Only break a line when it would otherwise exceed the limit, or when a break aids readability.
- When you do break, break at semantic boundaries:
  after a sentence, or at a logical clause boundary.
  Do not hard-wrap mid-clause at an arbitrary column.
- Prefer simple sentences separated by periods and commas.
  Avoid semicolons and em dashes.
  Split a long sentence into shorter ones on separate lines
  rather than joining clauses with `;` or `—`.
- Never use trailing comments on the same line as code.
  Place the comment on its own line above the code it describes.

Example (Markdown):

```
All human beings are born free and equal in dignity and rights.
They are endowed with reason and conscience
and should act towards one another in a spirit of brotherhood.
```

Example (C++ doc comment):

```cpp
/// Processes a single file through the supplied plugin instance.
/// The plugin must already be prepared with the appropriate sample rate and block size.
/// The function resets the plugin internally before processing,
/// and releases its resources afterwards.
```

## Notes

- The main application code lives in `src/`.
- Shell helpers live in the repository root.
- `CLAUDE.md` is a symlink to this file, so both point at the same guidance. Only edit `AGENTS.md` directly.
