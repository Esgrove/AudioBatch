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

The application code lives in `src/`, in one subdirectory per domain.
Headers sit next to their sources, and there is no separate `include/` directory,
because the project is an application with no public headers.
Directories describe what the code does, not which target links it.
The source lists in `CMakeLists.txt` decide which target builds each file.

- `src/analysis/` holds audio analysis and its result cache.
    - `AudioAnalysis.*` and `AudioAnalysisTypes.h` perform audio analysis as free functions.
      `AudioAnalysisFormatting.*` implement the display formatting,
      and `BitDepthDetection.*` detects the source bit depth from the audio file, with a fixed default for MP3 files.
      `DecodeFailureTracker.h` holds the shared rules for read failures while decoding a file block by block.
    - `AnalysisCache.*` manage the SQLite result cache.
      Its implementation is split into `AnalysisCache.cpp` (analysis, tag and waveform persistence)
      and `AnalysisCacheSchema.cpp` (database lifecycle and schema),
      which share the constants and helpers in `AnalysisCacheSql.h`.
    - `AnalysisCoordinator.*` orchestrates background analysis.
- `src/normalization/` holds `AudioNormalization.*` for normalization and AIFF conversion,
  `NormalizationFormatSupport.cpp`, which reports which formats this build can normalize,
  and `NormalizeCoordinator.*`, which orchestrates background normalization.

- `src/serato/` holds the Serato tag parsing (`SeratoTags.*`, `SeratoTagReader.*`, `SeratoTagTypes.h`),
  the overview waveform (`SeratoOverview.*`, `SeratoWaveformColouring.*`),
  and the background preview loading in `SeratoPreviewLoader.*`.
- `src/plugins/` holds VST3 / Audio Unit plugin hosting.
    - `PluginScanner.*` implement out-of-process plugin scanning and the known-plugins list persistence.
      Each executable relaunches itself as its scan worker:
      `gui/Main.cpp` and `cli/CliMain.cpp` detect the worker command line before doing anything else.
      The CLI links `juce_audio_processors` for this, which also pulls in the JUCE GUI modules.
    - `PluginChain.*`, `PluginChainPreparation.*`, `PluginProcessing.*`, and `PluginProcessingCoordinator.*`
      implement the plugin chain and batch processing through it.
- `src/utils/` holds the shared helpers, split by area, all in the `utils` namespace unless noted:
    - `Log.*` holds the leveled logging functions.
    - `AppInfo.*` holds startup flag queries, the version line, the About text, system information,
      and the settings file options.
    - `FileUtils.*` holds the file system helpers, such as moving files to the trash and extension checks.
      It also chooses the default folder for file browsing.
    - `GuiUtils.*` holds reusable window activation, foreground permission transfer,
      read-only label styling, and failure-summary dialogs.
      Native Windows request types and APIs stay private to its implementation.
    - `AudioFormatUtils.*` holds the per-thread format manager, the AIFF writer lookup, and peak math.
    - `AudioMetadata.*` reads and writes tags and embedded art through TagLib
      and provides shared TagLib filename conversion.
      Metadata types and operations remain in `audiobatch`, with filename conversion in `utils`.
    - `ThreadUtils.*` holds the `CancellationCheck` callback the services poll
      and the cancellable blocking call from a worker to the message thread.
    - `StringFormat.h` holds `utils::format` and the fmt formatters for JUCE types.
    - `ConsolePrint.*` holds the styled console output helpers in the `console` namespace.
    - `version.h` holds the build version constants.
      Put new generic helpers in the matching file instead of a file-local `static` function,
      so they are not duplicated across services.
- `src/cli/` holds the CLI front end.
    - `CliMain.cpp` is the CLI entry point.
    - `AudioAnalysisCli.{h,cpp}` declare the CLI options and the `cli` functions that run the analysis workflow,
      `AudioAnalysisCliParse.cpp` parses and validates the arguments,
      and `AudioAnalysisCliOutput.*` prints the results as a table, CSV, or JSON.
    - `PluginListCli.{h,cpp}` implement the CLI plugin listing and rescan.
- `src/gui/` holds the GUI application shell and its main screen.
    - `Main.cpp` is the GUI application entry point, and `MainWindow.*` is its top-level window and menu bar.
      `GuiLayout.h` holds shared UI dimensions and compile-time layout calculations,
      grouped by the window, workspace, table, inspector, plugin editor, waveform, and theme.
    - `AudioBatchComponent.*` is the main GUI component.
      Its implementation is split by topic into `AudioBatchComponent.cpp` (construction, layout, table, selection),
      `AudioBatchComponentActions.cpp`, `AudioBatchComponentOperations.cpp`, `AudioBatchComponentPreview.cpp`,
      and `AudioBatchComponentRoots.cpp`, which share main-view filters, ranges, and menu ids
      in `AudioBatchComponentConstants.h` and UI geometry in `GuiLayout.h`.
      Put new member functions in the file of their topic.
      `AudioBatchComponentState.h` holds the progress state of the analysis run and of the normalization
      and plugin processing passes.
    - `AudioPreviewPlayer.*` owns the audio device, transport, and read-ahead thread of the file preview.
    - `AudioFileTableModel.*` backs the sortable results table, and `ResultsSorting.*` sorts it.
    - `FileActivityTracker.*` holds the per-file activity labels and operation messages of the table.
- `src/gui/components/` holds the reusable UI pieces the main component is built from:
  `AudioInfoPanel.*`, `NormalizationTargetControl.*`, `CustomLookAndFeel.*`, `ThumbnailComponent.*`,
  `IntervalStepSlider.h`, and the plugin chain editor in `PluginChainEditor.*`.

Put a new file in the directory of its domain, and add a directory only for a new domain with several files.
Include project headers by their path relative to `src/`, for example `#include "analysis/AudioAnalysis.h"`,
also from within the same directory, and never with `../`.

The source lists are defined in `CMakeLists.txt`.
`AUDIOBATCH_CORE_SOURCES` holds the shared core and `AUDIOBATCH_CLI_SOURCES` the CLI front end,
and each target adds its own entry point and target-specific files.
The shared core is everything in `analysis/`, `normalization/`, and `utils/`,
plus `plugins/PluginScanner.*` and the Serato tag and overview files.
Add new shared files to the core list, not to each target,
and remember that a shared file must compile cleanly for both the GUI and the CLI.
List sources as relative `src/...` paths,
because the strict warning set is applied by matching that pattern.

Tests:

- `tests/` holds the `AudioBatchTests` executable, which links the shared core and the CLI front end.
  Suites are `juce::UnitTest` classes, one per file, created through the factories in `tests/TestSuites.h`.
- `tests/fixtures/` holds small committed audio files:
  one second 1 kHz tones at -6.02 dBFS in WAV, AIFF, FLAC, MP3, and Ogg, mono and stereo,
  at several sample rates and bit depths, plus a tagged FLAC file, digital silence, and a broken file.
  `tests/fixtures/generate.sh` regenerates them with ffmpeg.
  Tests that modify files work on copies in a temporary directory.

## Dependencies

- JUCE, vendored under `JUCE/` as a submodule.
- SQLite 3, resolved via CMake (system package if present, otherwise the fetched amalgamation).
- libebur128, fetched via CMake, for loudness and true peak analysis.
- TagLib, fetched via CMake, for cross-format metadata handling.
- {fmt}, fetched via CMake, for string formatting (see `src/utils/StringFormat.h`).

## Working Rules

- Prefer modern C++23 coding standards and idioms.
- Do not edit vendored code under `JUCE/` unless the task explicitly requires it.
- Prefer fixing warnings and build issues in project code or project configuration before considering third-party changes.
- Follow the domain directories under `src/` described in Source Layout and the current naming used in `CMakeLists.txt`.
- Keep reusable helpers in the matching shared utility files rather than application or component class files.
  Keep UI sizing constants in `gui/GuiLayout.h`.
  Component-specific presentation algorithms and private rendering details remain with their owning component.
- Changes to shared services must keep both the GUI and CLI targets building and behaving correctly.

## Code Style

- Prefer direct brace initialization for data members and namespace-scope constants,
  such as `int blockSize {512};` and `constexpr int fileSize {0};`,
  instead of copy initialization with `=`.
  Apply this to classes and structs, including those defined in source files.
  Do not extend this preference to local variables, loop initializers, or constructor initializer lists.
  Only add an empty `{}` initializer when value initialization is needed.
  Do not add redundant empty initializers to default-constructed class objects or members initialized by a constructor.
  Keep another initialization form when braces would change type deduction or constructor overload selection.
  Examples include container size constructors and copies of `juce::Array`.
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
  Document a function on its definition in the `.cpp` file when the header has a matching `.cpp`.
  Do not repeat the docstring on the declaration in the header.
  Header-only code, inline functions, and templates are documented where they are defined.
  Types, enums, aliases, constants, and data members keep their docs in the header.
- Prefer full names instead of abbreviations in identifiers,
  for example `directories` instead of `dirs` and `description` instead of `desc`.
  Established domain terms and API names (`midi`, `xml`, `id`, `dB`) are fine.
- Avoid single character variable names.
  They are only allowed in for loops when it makes sense
  and it is clear what the variable is, for example a plain loop index.
- Prefer `utils::format` (fmt-based, in `src/utils/StringFormat.h`)
  over chained `juce::String` concatenation when building display and log text.
  Format strings must be literals so fmt's compile-time checking applies.
  Keep `.quoted()` calls on arguments instead of putting quote characters in the format string,
  and keep `juce::newLine` outside format strings in text that leaves the process,
  because it is `\r\n` on Windows.
- Write console output with `fmt::print` or the `console::` helpers in `src/utils/ConsolePrint.h`.
  `std::format` and `std::print` are also fine.
  Prefer these over `std::cout` / `std::cerr`,
  unless the iostream version is clearly more readable and performant with much less code.
  Use `console::printError` and `console::printWarn` for user-facing error and warning messages.
  Avoid `fmt::println`, because it triggers MSVC warning C4702 inside fmt with warnings as errors.
- The logging helpers accept fmt format strings and arguments directly:
  `utils::logError("Analysis failed for {}: {}", path.quoted(), message)`.
  Do not wrap the message in a `utils::format` call first.

## Code Organization

The detailed rules are in `.agents/rules/code-organization.md`.
`.claude/rules` is a symlink to `.agents/rules`, so Claude Code reads the same files.

- Declare enums first, then structs, then classes.
- Within a class or struct, put `public` before `protected` before `private`.
- Put member functions before data members.
- Group functions and variables by return type and topic.
- Do not define structs, classes or enums inside classes.
  Declare them at namespace level before the class, or in the anonymous namespace of the `.cpp` that alone uses them.
- All project code lives in `namespace audiobatch`, with the helper namespaces
  such as `utils` and `console` nested inside it.
  Wrap whole headers and sources, so definitions of classes need no qualification.
  Only `main`, the JUCE application macro, and code that must be in the `juce` namespace stay outside.
- Do not write a class that only has static member functions.
  Use free functions in a namespace instead, and keep classes for types with state or an invariant.
- Do not use `using namespace`. Wrap definitions in the matching namespace block or qualify names.
- Do not use namespace aliases. Qualify references with the original namespace path.
- Split files that reach about 1000 lines into focused parts by topic, but avoid very small files.
  Fold tiny files into the file of the closest topic.
- Use `constexpr` and `consteval` whenever possible.
- Include order is handled by clang-format, so do not sort includes by hand.

## Build And Verification

- Do not use `build.sh`, it is only for making final release builds.
- When modifying code, use the CMake debug presets for validation
  so terminal builds reuse the same build directory as the editor or IDE.
- On macOS, use `cmake --preset macos-debug` then `cmake --build --preset macos-debug`.
- On Windows, prefer `cmake --preset windows-debug` then `cmake --build --preset windows-debug`.
- To run the tests under AddressSanitizer and UndefinedBehaviorSanitizer, run `./sanitize.sh`.
  It configures and builds the `macos-asan` preset in `cmake-build-mac-asan`, then runs ctest.
  The `AUDIOBATCH_SANITIZE` CMake option takes a comma separated list such as `address,undefined`.
- If you need Ninja and `compile_commands.json` on Windows,
  use `cmake --preset windows-ninja-debug` then `cmake --build --preset windows-ninja-debug`.
- Both `AudioBatch` and `AudioBatchCli` build with warnings as errors, so all warnings must be resolved.
- On top of the JUCE recommended warning flags,
  `audiobatch_set_source_warnings` in `CMakeLists.txt` adds a stricter warning
  set to the project's own `.cpp` files under `src/` and `tests/` only.
  JUCE module sources do not get it,
  and the JUCE headers are included as system headers so their warnings are not reported.
  Several of those flags are Clang-specific, so GCC is not a supported compiler.
  `src/gui/Main.cpp` additionally gets `-Wno-cast-qual`,
  because the `START_JUCE_APPLICATION` macro casts away `const` in code the project cannot change.

## Testing

- Only add lightweight, focused unit tests that complete in under one second.
  Do not add GUI tests, screenshot comparisons, or rendering-based assertions.
- Build the debug preset and run `ctest --test-dir cmake-build-mac-debug --output-on-failure`,
  or run the `AudioBatchTests` executable directly for the full test log.
- Add tests for new behaviour in the shared core and the CLI parsing.
  Prefer the committed fixtures over generating audio in a test.
- Avoid tests that convert non-AIFF sources through normalization,
  because a successful conversion moves the source to the system trash.
  Plugin processing tests set `PluginProcessingOptions::disposeFile` to a function that deletes the file instead,
  and `utils::commitTemporaryOutput` takes the same kind of function.
- `tests/.clang-tidy` disables `readability-magic-numbers`, since tests compare against literal expected values.

## Formatting

- After changing any C++ source or header file, run `clang-format -i --style=file` on each changed file before finishing.
- For repo-wide or multi-file formatting, use `./format.sh`.
  `./format.sh --check` only verifies formatting without changing files.
- C++ formatting is configured by `.clang-format`.
- `./format.sh` also formats `CMakeLists.txt` with cmake-format, configured by `.cmake-format.yaml`,
  and the shell scripts with shfmt when it is installed.
  cmake-format runs through `uvx` when it is not installed.
  The fallback uses Python 3.14 because cmakelang's token scanner is incompatible with Python 3.15.
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
