# AGENTS.md

ImageControl is a multiband mid/side stereo imaging effect built with C++23 and JUCE.
It builds as a VST3 on macOS and Windows, and as an Audio Unit on macOS.
This file governs everything under `image-control-plugin/`,
and paths below are relative to it unless they start with `../`.
Run the build, format, lint, and test commands below from `image-control-plugin/`.

ImageControl lives in the AudioBatch repository and is mainly used from AudioBatch's plugin chain,
but it is an independent plugin, not part of either application.

- Its names, ids, and namespace never carry AudioBatch branding.
  The namespace is `esgrove::image_control`, the bundle id `com.esgrove.imagecontrol`,
  the manufacturer code `Esgr`, and the plugin code `Egic`.
- The repository's feature scope and behavioral parity rules for the C++ and Rust applications do not apply here.
- It does not link AudioBatch's application code, so `audiobatch::utils`, `console::`, `utils::format`,
  `utils::enumFromId`, and the logging helpers are not available.
- The root [`AGENTS.md`](../AGENTS.md) still applies,
  including the platform scope, the Python and uv rules, staging with explicit paths,
  and never running destructive git commands.

[README.md](README.md) describes the features and commands for users.
[ARCHITECTURE.md](ARCHITECTURE.md) explains ownership, signal flow, threading, and the numerical contracts.
Read the architecture section of the area you change before changing DSP, state, threading, or rendering code.

## Rust experiment

`rust/` contains one independent DSP/editor implementation in the repository-root Cargo workspace.
It exports VST3 and CLAP on macOS and Windows, plus experimental AUv2 on macOS through pinned upstream clap-wrapper.
`rust/crates/dsp` owns framework-independent processing, `rust/crates/plugin` the VST3/Clack/Iced/wgpu adapters,
and `rust/crates/xtask` the bundler. `rust/au/` owns the native AU build and isolated qualification host.
The AU component statically hosts Rust CLAP, with distinct `aufx / Egir / Esgr` identity, never C++ `Egic`.
Preserve explicit upstream offline/state/contention diagnostics and do not generalize VST3 realtime evidence to AU.
The package and plugin identities are stable across directory renames.
There is no plugin-local GPUI workspace, vendored embedding patch, or nice-plug runtime.
AudioBatch's application GPUI dependencies remain separate.
Run `build.sh`, `test.sh`, `lint.sh --check`, and `format.sh --check` from `rust/` for this experiment.
Use root Rust lints and formatting, narrow documented unsafe ABI boundaries, and the plugin realtime contracts below.
For all Rust plugin code, including test helpers, benchmarks, and examples,
return domain-specific structs with named fields for values with distinct meanings instead of raw tuples,
especially three or more values or ambiguous two-value results.
For example, use `ParameterRange { minimum, maximum, step }` instead of `(f64, f64, f64)`.
Tuple aliases are not a substitute for named fields.
Keep conventional coordinates, homogeneous bounds, key-value iterator items, channel endpoints,
and tuples required by external traits when their positional meaning is clear.
Preserve external interfaces and serialized formats when improving internal return types.
Treat Rust state/class identities separately from the C++ persistence contracts.
Cross-implementation state migration is not implied.
The evaluation
and remaining platform/lifecycle gates are documented in [`rust/docs/rust-evaluation.md`](rust/docs/rust-evaluation.md).
The C++-specific organization, JUCE, and OpenGL instructions below apply to the C++ implementation.

## Layout

- `src/ImageControlProcessor.*` owns the host-facing lifecycle, the parameter state, the analyzer tap,
  correlation monitoring, and state synchronization.
- `src/ImageControlEditor.*` composes the controls, the analysis worker, and the OpenGL rendering.
- `src/Parameters.*` defines the stable parameter ids, version hints, ranges, defaults, display conversions,
  the crossover ordering rule, and the editor-setting property names.
  It also holds the plugin's `OptionalRef` alias and the helpers that several areas share.
- `src/ProcessingState.*` holds the detached sound snapshots and their versioned XML codec.
  `src/ParameterHistory.*` holds the local undo history, and `src/UserPresets.*` the user preset library.
- `src/dsp/` holds the filter design, the partitioned convolution, the imager, and the band correlation.
  It uses no components or OpenGL.
- `src/analysis/` holds the audio-to-editor SPSC queue and the spatial spectral analysis.
- `src/ui/` holds the reusable controls, gestures, display algorithms, and the OpenGL renderer.
- `tests/` holds the GoogleTest suites, `TestMain.cpp`, the shared `TestSignals.h`,
  the RTSan-only `RealtimeTests.cpp`, and `test_build_scripts.py` for the shell scripts.
  `SpectrogramOverlayTests.cpp` owns CPU-only overlay geometry and response-cache tests,
  and `ImageControlEditorTests.cpp` owns editor layout and analysis lifecycle tests.
  Processor correlation lifecycle tests live in `CorrelationTests.cpp`,
  and non-finite parameter notification tests live in `HostContractsTests.cpp`.
  Particle projection and band-strip gestures have their own topic suites.
- `benchmarks/` holds `Benchmark.cpp` for `ImageControlBenchmark`,
  `PresetWriter.cpp` for `ImageControlPresetWriter`,
  and `compare_formats.py` with its tests, which compare the VST3 and the Audio Unit through the AudioBatch CLI.
- `test-signals/generate.py` writes reference signals into the ignored `test-signals/output/` directory.
- `cmake/Verification.cmake` adds the sanitizer and coverage options.
  `CMakeLists.txt` also includes `../cpp/cmake/Optimizations.cmake`,
  so the plugin targets the same CPUs and optimizations as AudioBatch.

`CMakeLists.txt` lists every source explicitly in `IMAGE_CONTROL_SOURCES` and `IMAGE_CONTROL_TEST_SOURCES`,
so add a new file to the matching list.
Include project headers by their path relative to `src/`, such as `#include "dsp/MultibandImager.h"`,
and test helpers by their path relative to this directory, such as `#include "tests/TestSignals.h"`.
Never include with `../`.
`.clang-format`, `.clang-tidy`, `.cmake-format.yaml`, and `tests/.clang-tidy` are symlinks to the C++ project's files.
On Windows they need Developer Mode and `git config core.symlinks true` before checkout.

## Plugin Contracts

Hosts, saved projects, and automation depend on these contracts.
Treat a change to any of them as a deliberate compatibility decision, and test it.

- **Parameter ids and version hints are persistence and automation contracts.**
  Never rename, remove, reorder, or re-hint an existing parameter.
  Append a new parameter at the end of the layout in `Parameters.cpp` with the next version hint, currently 8,
  and give it a default that keeps older projects sounding the same.
- **Every parameter is a `FiniteParameter`.**
  It rejects non-finite values and invalid text while keeping the previous value.
- **Neutral settings are bit-exact transparent.**
  At neutral settings the output is the input delayed by the latency, bit for bit,
  through the exact-neutral shortcut rather than floating-point cancellation.
  Keep the processor and imager tests that check this passing without a tolerance.
- **Latency and tail are reported to hosts.**
  The latency is the kernel half-length plus one partition, 4352 samples at 48 kHz,
  and the tail covers the FIR ring beyond the latency-compensated endpoint.
  Update both reports when the filter design or partitioning changes. Bypass never changes latency.
- **Real-time callbacks do not allocate, lock, block, print, or log.**
  Preparation allocates every buffer and the initial kernels.
  Crossover kernels are designed on the kernel design thread and swapped in at partition boundaries,
  except that the processing thread designs them synchronously when the host sets non-realtime mode.
  The analyzer queue is single-producer, single-consumer, and drops frames instead of blocking.
  State save and restore allocate and lock, so they stay off the audio thread.
- **Saved state stays loadable.**
  Host state holds the parameter tree, the comparison state, and the editor state.
  Sound snapshots, comparison state, and preset files carry their own version constants.
  Missing properties and parameters restore from fixed defaults, and older states load without a gain-derived migration.
  Bump a version constant only with a tested migration or fallback for the older format.
- **Threads keep their ownership.**
  The live editor `ValueTree` belongs to the message thread.
  Host state callbacks use detached snapshots and never wait for message dispatch.
  Local history is never serialized, and host automation never enters it.
  Only the processing thread touches the correlation moments and convolution history.
- **Only matching mono-to-mono and stereo-to-stereo main bus layouts are supported.**
- **Validation is never skipped silently.**
  A missing pluginval or a failed auval run fails the command.
  RTSan `BLOCKED` audits of JUCE host and wrapper code are not clean results.

## Dependencies

- JUCE, from the [Esgrove/JUCE](https://github.com/Esgrove/JUCE) fork, as the shared `../cpp/JUCE` submodule.
  Fix JUCE bugs in the fork instead of working around them in plugin code.
  Commit the fix on the fork's `master` in JUCE's own code style, keep each patch small and upstreamable,
  push it before the commit that refers to it, and update the submodule in the commit that relies on the fix,
  with a test that pins the fixed behaviour, as `tests/HostContractsTests.cpp` does for non-finite parameter notifications.
  The C++ application and the test plugin build against the same submodule,
  so a submodule update must also pass the C++ workflow.
- GoogleTest, fetched by CMake at a pinned commit, for the unit tests.
- {fmt}, fetched by CMake at the same pin as `../cpp/CMakeLists.txt`,
  only for the console tools: the benchmark, the preset writer, and the RTSan test host.
  It is never linked into the plugin or its format wrappers.
- Do not edit third-party code. Fix warnings and build issues in plugin code or plugin configuration first.

## Working Rules

- Prefer modern C++23 idioms.
  Use concepts and `requires` clauses when they express a meaningful type requirement,
  instead of SFINAE or `std::enable_if`.
- Keep the DSP independent of components and OpenGL,
  `SpatialAnalyzer` independent of the editor and the renderer, and OpenGL code in `SpectrogramRenderer` only.
- Keep shared helpers in `Parameters.*` or the file of their topic rather than duplicating them in components.
- Preserve DSP operation order, precision, and rounding when optimizing.
  Do not substitute approximate constants, reciprocals, or fast math without verification.
  Prefer multiplication by one half over division by two for floating-point values.
  Measure runtime changes with `ImageControlBenchmark` against a matched baseline.

### macOS And Windows Portability

- Keep every source and test compatible with Apple Clang and libc++ on macOS and MSVC and its STL on Windows.
  A macOS build or clang-tidy pass does not establish Windows compatibility.
  Consider both Debug and Release, since checked iterators and optimizer diagnostics differ between them.
- Treat standard-library iterators as iterators, never as raw pointers.
  Do not declare the result of `std::ranges::find` with `auto*`, even when macOS clang-tidy suggests it.
  Prefer `std::ranges::contains` when only membership matters.
- Review lint autofixes for assumptions about iterator representation, pointer constness, and platform APIs.
- The editor needs OpenGL 3.3. macOS uses half-float accumulation textures and Windows full-float textures.
  Audio processing and controls must keep working when the renderer is unavailable.
- If only macOS validation is possible, state that limit rather than claiming both platforms were verified.

## Code Style

- Prefer direct brace initialization for data members and namespace-scope constants,
  such as `float sampleWeight {0.0F};` and `constexpr std::size_t BAND_COUNT {4};`,
  instead of copy initialization with `=`.
  Do not extend this to local variables, loop initializers, or constructor initializer lists.
  Only add an empty `{}` initializer when value initialization is needed,
  and keep another form when braces would change type deduction or constructor overload selection.
- Name shared constants in `SCREAMING_SNAKE_CASE`: namespace-scope constants, including file-local ones,
  and static class constants.
  Constants inside a function, including `constexpr` locals, are local variables and use camelCase.
- Prefer named constants over unexplained numeric literals for domain concepts, protocol fields,
  policy limits, and reused values. Keep self-evident arithmetic and conventional values inline.
- Group decimal integer parts only when they have five or more digits.
  Leave four or fewer integer digits ungrouped, such as `4000` and `4000.0`.
  Use apostrophes in C++, such as `48'000`, and underscores in Rust, such as `48_000`.
  Fractional groups remain allowed independently, such as Rust `1681.974_450_955_533`.
  C++ float literals use the uppercase `F` suffix.
- Every source and header file begins with a `///` file comment, before `#pragma once` and the includes,
  that names its main types or functions and their role in the plugin.
- Every function has a `///` docstring that explains what it does and why,
  including threading constraints, real-time safety, and ownership rules.
  Document a function on its definition in the `.cpp` when one exists, not again on the header declaration.
  Header-only code, inline functions, and templates are documented where they are defined.
  Types, enums, aliases, constants, and data members keep their docs in the header.
- When one data member of a struct has a docstring, give every data member one.
  Document either every enumerator of an enum or none of them.
- Prefer full names instead of abbreviations, such as `crossovers` instead of `xovers`.
  Established terms such as `dB`, `id`, `fft`, `min`, and `max` are fine.
  Avoid single character names, except plain loop indices.
- Compare floating-point values with `juce::approximatelyEqual`, and `!juce::approximatelyEqual` for inequality.
  Use `juce::exactlyEqual` only where a bit-exact comparison is the intent, such as neutral-setting checks.
  Direct `==` and `!=` on floating-point values trigger `-Wfloat-equal`, which is an error.
- Index per-sample audio loops with `operator[]`, not the throwing `at()`.
- Plugin code in `src/` never prints to stdout or stderr.
  It runs inside hosts without a console, and output from the audio thread would break the real-time contract.
  `DBG` is acceptable only for debug-build diagnostics off the audio thread,
  such as the shader link failure in `SpectrogramRenderer.cpp`, because Release builds compile it out.
- The console tools print with `fmt::print`, to `stdout` or `stderr`, with literal format strings.
  Avoid `fmt::println`, because it triggers MSVC warning C4702 inside fmt with warnings as errors.
- Every source builds with the strict warning set in `CMakeLists.txt` and warnings as errors,
  applied per file to the plugin, test, benchmark, and preset writer sources.
  JUCE headers are system includes, so their warnings are not reported.

## Ownership and Pointers

Express ownership in the type.
Prefer a value or a reference, then a smart pointer, and a raw pointer only at a documented API boundary.

- Hold sole ownership in `std::unique_ptr` created with `std::make_unique`,
  and use `std::shared_ptr` only for a genuinely shared lifetime.
- Do not write `delete`.
  Use `new` only where JUCE takes ownership, such as `createEditor` and `createPluginFilter`.
- Pass a required object as `T&` or `const T&`, contiguous elements as a `std::span`,
  and return a value or a `std::optional` instead of a nullable pointer.
- Use the plugin's `OptionalRef<T>` from `Parameters.h` for non-owning references that may be absent,
  and `std::reference_wrapper<T>` for non-null references in assignable containers,
  such as the parameter references into the parameter tree.
  Document who owns the referenced object and how long it stays valid.
- Keep a pointer to a component that may be destroyed first as a `juce::Component::SafePointer`
  and check it before use, especially in asynchronous callbacks.
- Give a class with a reference data member deleted copy and move operations.
- Keep raw pointers at JUCE, OpenGL, and platform API boundaries, such as channel pointer arrays,
  and document on each retained pointer who owns the object, whether it can be null, and how long it stays valid.

## Code Organization

The detailed rules are in [`../.agents/rules/code-organization.md`](../.agents/rules/code-organization.md)
and [`../.agents/rules/comments.md`](../.agents/rules/comments.md), which cover this plugin too.

- Follow the C++ Core Guidelines NL.16 order: types and aliases, special members, functions, then data.
- Within a class, put `public` before `protected` before `private`.
  Put constructors, the destructor, and copy and move operations first, then static methods.
- Keep ordinary methods separate from overrides, grouping overrides by their JUCE interface.
  Within a block, group by return type and alphabetize names, keeping overloads together.
- Put member functions before data members in each access section, and group fields by type.
- Correctness, lifetime dependencies, and real-time contracts take priority over cosmetic sorting.
  Match constructor initializer lists to declaration order,
  and explain a dependency-sensitive declaration order with a short comment.
- Do not define structs, classes, or enums inside classes.
  Declare them at namespace level, or in the anonymous namespace of the one `.cpp` that uses them.
- Give each `.cpp` file a single anonymous namespace right after the project namespace opens,
  for its file-local types, constants, and helpers, instead of `static`.
- Make enums `enum class`.
- Put all plugin code in `namespace esgrove::image_control`, with nested namespaces such as `test` and `benchmark`.
  Only `createPluginFilter`, `main`, and forward declarations of third-party types stay outside.
- Do not write classes with only static members, and do not use `using namespace` or namespace aliases.
- Keep headers self-contained, with no reliance on transitive includes and no cyclic dependencies.
- Split a file that reaches about 1000 lines into focused parts by topic,
  as `SpatialSpectrogramViewInput.cpp` is split from `SpatialSpectrogramView.cpp`, but avoid very small files.
- Use `constexpr` and `consteval` whenever possible.
- clang-format sorts the includes, so do not sort them by hand.

## Build And Verification

The build needs CMake 4.2 or newer, a C++23 compiler, and the JUCE submodule,
initialized from the repository root with `git submodule update --init cpp/JUCE`.
macOS builds use the Xcode toolchain and Ninja when available, with macOS 15 as the minimum deployment target.
Windows builds use Visual Studio by default, or Ninja with `--ninja`.

```shell
# Build the Release VST3, and on macOS the signed Audio Unit, into cmake-build-release/
./build.sh
# Also build and run the unit tests, in Release or Debug
./build.sh --test
./build.sh --test --debug
# Validate the built VST3 with pluginval, without installing it
./build.sh --validate
```

`build.sh` also accepts `--build-type`, `--reuse`, `--clean`, `--print`, and `--validation-level <5-10>`.
`--reuse --test` requires a workspace already configured with `IMAGE_CONTROL_BUILD_TESTS=ON`.
Debug builds go into `cmake-build-debug/`, and Windows Ninja builds into `cmake-build-ninja-<type>/`.
pluginval comes from `PATH`, or is built at a pinned version in the repository's `.deps/` directory.
For the pinned validator explicitly, configure with `-DAUDIOBATCH_PLUGINVAL=ON`
and build the `ImageControl_pluginval` target.

`./install.sh` builds and copies the plugin into the user's plugin folders,
and `./install.sh --validate` also validates the installed VST3 and, on macOS, the registered Audio Unit with auval.
Both change the installed plugins and the Audio Unit registry,
so run them only when the user asks for an installation or installed-format validation.

The sanitizer and coverage scripts are macOS only, and each uses its own build directory:

- `./sanitize.sh` runs the unit tests under AddressSanitizer and UndefinedBehaviorSanitizer in `cmake-build-mac-asan/`.
- `./sanitize.sh --thread` runs them under ThreadSanitizer in `cmake-build-mac-tsan/`.
- `./sanitize.sh --realtime` checks the callbacks with Homebrew LLVM RealtimeSanitizer in `cmake-build-mac-rtsan/`.
  It builds `ImageControlRealtimeTests`
  and runs one CTest case per callback scenario through `../tests/realtime_sanitizer.py`.
  It checks detector controls, clean direct-processing cases,
  and the exact build-tree VST3 with its known JUCE host and wrapper findings as explicit `BLOCKED` audits.
  Diagnostics are kept under `cmake-build-mac-rtsan/realtime-artifacts/`.
  The checks detect exercised unsafe calls, not audio deadlines.
- `./coverage.sh --lcov` measures source coverage of `src/`, and `--open` opens the HTML report.
  The reports are in `cmake-build-mac-coverage/coverage/`.

Run the sanitizers after changing ownership, threading, buffer handling, or callback code.

Benchmarks are not tests, and `--test` does not build them:

```shell
cmake --build cmake-build-release --config Release --target ImageControlBenchmark
./cmake-build-release/ImageControlBenchmark --quick
```

Use a Release build for measurements, and `--help` for the workload options such as `--focused`.
`IMAGE_CONTROL_HARDENING=OFF` gives a separate build for comparing the cost of standard library hardening.
The results describe CPU callback work, not GPU drawing or device deadlines,
and are evidence of an improvement only against a matched baseline.
On macOS, `benchmarks/compare_formats.py` compares the installed VST3 and Audio Unit through the AudioBatch CLI.
It requires both installed formats, so it is not a routine check.

## Testing

- `ImageControlTests` holds plain GoogleTest suites, one topic per file, registered with CTest under the `unit` label.
  Run one suite with `ctest --test-dir cmake-build-debug -R <Suite>`,
  or the executable with `--gtest_filter=<Suite>.*`.
- Tests exercise the real processor, imager, and components, not a separate model of them.
  Prefer independent expectations, such as analytical signals, direct convolution, and exact neutral output,
  over values recorded from the current implementation.
- Keep unit tests focused and fast, with bounded waits for worker threads.
  Do not use installed plugins, physical audio devices, screenshots, or pixel assertions.
  Accessibility checks use native component peers.
- Add tests for new DSP behaviour, parameters, state formats, and host lifecycle contracts.
  A new parameter needs tests of its host index, its default for older states,
  and its automation in real-time and offline processing.
- `RealtimeTests.cpp` builds only in the `realtime` sanitizer configuration,
  where it fails to compile without RealtimeSanitizer.
  Mark only the callback under test as nonblocking, and keep setup and assertions outside the marked region.
- `tests/test_build_scripts.py` and `benchmarks/test_compare_formats.py` check the scripts through uv:
  `uv run --script tests/test_build_scripts.py` and `uv run --script benchmarks/test_compare_formats.py`.
- `tests/.clang-tidy` disables `readability-magic-numbers`, since tests compare against literal expected values.
- CI runs `../.github/workflows/image-control.yml`:
  Debug and Release builds and tests on macOS and Windows, pinned pluginval, Audio Unit validation on macOS,
  clang-tidy, AddressSanitizer, ThreadSanitizer, RealtimeSanitizer, coverage, and the script contract tests.

## Formatting

- After changing any C++ source or header, run `clang-format -i --style=file` on each changed file.
- `./format.sh` formats the plugin's C++ sources and headers, its CMake files, its shell scripts,
  its Python scripts with ruff, and its prose with slb when installed.
  `./format.sh --check` only verifies them.
- The root `../format.sh image-control` runs the same script,
  and the root `../format.sh --check` that CI runs includes it.
- The pre-commit hooks in `../.pre-commit-config.yaml` also format the plugin's C++ and CMake files.

## Linting

- After any C++ change, run `./lint.sh` to check the plugin with clang-tidy, and fix what it reports.
  It uses the C++ project's `.clang-tidy`, and `tests/.clang-tidy` for the tests.
- `./lint.sh --check` exits with code 1 when clang-tidy reports any warning.
  `./lint.sh --changed` lints only the files changed since the last commit, including untracked ones,
  and `./lint.sh --staged` only the staged ones.
  A changed header also lints the source file of the same name next to it.
- Lint prepares a Debug build with the tests and benchmark enabled to get its compilation database.
  It lints `RealtimeTests.cpp` only when the RealtimeSanitizer workspace exists,
  so run `./sanitize.sh --realtime` first on macOS to include it.
- If a clang-tidy check is genuinely a poor fit, disable it in the shared `.clang-tidy` with a comment explaining why,
  since the C++ application uses the same configuration.
  Do not silence individual warnings with inline comments.
- The root `../lint.sh image-control` runs this script, and the root `../lint.sh` runs it after both applications.

## Prose Style (Comments, Docstrings, Markdown)

Use [semantic line breaks](https://sembr.org/) for code comments, docstrings, commit messages, and Markdown files.

- Target a soft line length of about 120 characters.
  Break a line only when it would exceed the limit or when a break aids readability.
- Break at semantic boundaries, after a sentence or at a logical clause boundary, never mid-clause.
- Prefer simple sentences separated by periods and commas. Avoid semicolons and em dashes.
- Never use trailing comments on the same line as code. Place the comment on its own line above the code.
- Comments describe what the code does now and why, not the change that introduced it.
  Avoid phrases such as "previously", "no longer", or "this PR".
  Comments on state migrations describe what each step does and why it exists.

Example:

```cpp
/// Designs the replacement kernels for the crossovers of the pending request into the inactive slot.
/// Runs on the kernel design thread, or on the processing thread when the host renders offline.
/// The audio thread switches to the new slot at the next partition boundary.
```

## Notes

- Shell scripts live in this directory.
  Their `common.sh` sets `REPO` to `image-control-plugin/` and sources `../common.sh`,
  which holds the helpers shared with the AudioBatch projects.
- `CLAUDE.md` is a symlink to this file. Only edit `AGENTS.md` directly.
