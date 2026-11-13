# AGENTS.md

AudioBatch analyzes and batch processes audio files.
This repository holds two versions of it side by side, each with its own agent instructions:

- `cpp/`: the C++23 and JUCE application, the reference implementation.
  Read [`cpp/AGENTS.md`](cpp/AGENTS.md) before changing anything under `cpp/`.
- `rust/`: the Rust port. Read [`rust/AGENTS.md`](rust/AGENTS.md) before changing anything under `rust/`.

Each version also has an `ARCHITECTURE.md`, [`cpp/ARCHITECTURE.md`](cpp/ARCHITECTURE.md) and [`rust/ARCHITECTURE.md`](rust/ARCHITECTURE.md),
which explains how its parts work together.
The ImageControl plugin in `image-control-plugin/` is a separate C++ project with its own instructions.
Read [`image-control-plugin/AGENTS.md`](image-control-plugin/AGENTS.md) before changing anything under it.

## Agent Commands

Reusable command prompts live in [`.agents/commands/`](.agents/commands/).
`.cursor/commands` and `.claude/commands` link to that directory, so edit the canonical files only.

- `/all-checks [scope] [check-only]` runs the complete applicable verification suite and fixes found issues by default.
  Add `check-only` to report without applying fixes.
- `/code-review [scope]` reviews current project code without editing.
  Add `changes` for local changes or `base=<ref>` for branch changes plus local work.
- `/guidelines-review [scope]` applies small, behavior-preserving organization and style fixes.
  Add `check-only` to report without editing, or `changes` to limit cleanup to local changes.

All commands default to the whole repository.
Scopes are `all`, `audiobatch` for both applications and shared support, `cpp`, `rust`, and `imagecontrol` for the plugin.
Each command documents its scope, safety boundaries, and reporting requirements.

## Platform Scope

AudioBatch supports only macOS and Windows, as the README states.
Linux is not a target, and VST3 plugin hosting does not exist there for this project.
Do not add Linux builds, CI jobs, code paths, dependencies, tests, or documentation,
and do not discuss Linux support or Linux tooling in plans, reviews, or answers.

## Shared Files

- `assets/` holds the icons and fonts that both applications bundle.
- `common.sh` holds the shell helpers shared by the scripts of all three projects,
  such as platform detection and colored output.
  `cpp/common.sh`, `rust/common.sh`, and `image-control-plugin/common.sh` source it
  and add their project specific parts.
- `build.sh`, `install.sh`, `format.sh`, and `lint.sh` run the scripts of the same name in both applications,
  or in the one named by their first argument, `cpp` or `rust`, through `run_project_scripts` in `common.sh`.
 Unqualified `install.sh`, `format.sh`, and `lint.sh` also run the ImageControl script after both applications.
 `install.sh` then runs `image-control-plugin/rust/install.sh`, including with `--cli-only`.
 An `image-control` first argument runs only the plugin scripts, both C++ and Rust for `install.sh`.
 `install.sh image-control` does not forward root installer options,
 so run `image-control-plugin/install.sh` or `image-control-plugin/rust/install.sh` directly for plugin-specific options.
  `format.sh` also formats the root shell scripts and the prose outside the projects when shfmt and slb are installed,
  runs ruff on the Python scripts through uv,
  and formats tracked and non-ignored new JSON files with jq using two-space indentation.
  `lint.sh --check` reports warnings without applying fixes.
- `package-macos.sh` builds non-relocatable Apple Silicon `.pkg` installers (`arm64`, macOS 15+, `apple-m1` baseline)
  into `dist/macos/` through `cpp/package-macos.sh`, `rust/package-macos.sh`, and `image-control-plugin/package-macos.sh`.
  Each installer exposes customizable App/CLI or VST3/Audio Unit choices,
  with optional Developer ID `--sign` and `--notarize` modes, while local `build.sh` and `install.sh` remain `native`.
- `bump-version.sh` bumps the versions of the C++ application, the Rust workspace, and the test plugin together,
  a patch by default, or `--major` or `--minor`.
  `--commit` commits only the version files and adds an annotated `vX.Y.Z` tag.
  ImageControl keeps its own version in `image-control-plugin/CMakeLists.txt`.
- `fixtures/` holds the committed audio fixtures that both test suites read in place,
  and `fixtures/README.md` lists them with what each one tests.
  `fixtures/generate.sh` regenerates the ordinary audio with ffmpeg and the plugin expected outputs,
  not every fixture collection or independent expectation dataset.
  It calls `fixtures/serato_overview.py` to add the rainbow Serato Overview tag that ffmpeg cannot write.
  When an input changes intentionally, review dependent plugin outputs, independent metric expectations,
  C++ test values, Rust `test-fixtures` values, and affected golden snapshots.
  Follow the [scoped capture procedure](fixtures/README.md#regeneration-provenance).
  It covers input identities and `fixtures/checksums.sha256`.
  Do not refresh unrelated expectations or identities to make a failure pass.
  `fixtures/metrics_expected.py` generates independent source-metric expectations and requires jq.
  The real recordings under `fixtures/metadata/` are immutable inputs, with independently inspected expectations.
- `test-plugin/` holds `AudioBatch Test MS`, the JUCE VST3 effect that both test suites load,
  described in `test-plugin/README.md`,
  with a gain, a mid/side width, and a latency parameter that the hosts must compensate.
  The C++ build adds it with its tests,
  and `test-plugin/build.sh` builds it into `test-plugin/cmake-build-release` for the Rust tests.
  It builds with the JUCE submodule in `cpp/JUCE`, and `cpp/format.sh` formats it.
  `uv run fixtures/plugin_expected.py` computes its expected outputs with numpy into `fixtures/plugin_expected/`.
  When its processing changes, update the script, regenerate the files,
  and update the expectation lists in `cpp/tests/PluginPresetTests.cpp` and the Rust `test-fixtures` crate.
  Review and update only the affected checksum entries using the fixture identity-capture procedure.
- `image-control-plugin/` holds ImageControl, a multiband mid/side stereo imaging effect,
  as a VST3 and, on macOS, an Audio Unit, described in `image-control-plugin/README.md`.
  It is a real plugin for processing audio, mainly from AudioBatch's plugin chain, and not part of either application.
  Its C++ implementation and independent Rust VST3 experiment are outside the application feature scope
  and behavioral parity rules.
  Its names and ids never carry AudioBatch branding.
  `image-control-plugin/build.sh` builds it with the JUCE submodule in `cpp/JUCE`,
  and runs its GoogleTest unit tests with `--test`.
  `image-control-plugin/install.sh` builds it with `build.sh` and installs it into the plugin folders.
  `image-control-plugin/rust/` contains one own-wrapper VST3/Iced/wgpu implementation in `crates/plugin`,
  with shared processing in `crates/dsp` and packaging in `crates/xtask`, all in the root Cargo workspace.
  Its scripts run from that directory and produce `ImageControl RS`, without replacing C++ `ImageControl`.
  `image-control-plugin/format.sh` formats it,
  and `image-control-plugin/lint.sh` checks it with the C++ clang-tidy configuration.
  It includes `cpp/cmake/Optimizations.cmake`, so it targets the same CPUs and optimizations as AudioBatch.
  `image-control-plugin/benchmarks/compare_formats.py` renders files through its VST3
  and Audio Unit with the AudioBatch CLI, and compares their speed and output.
  `image-control-plugin/test-signals/generate.py` writes stereo sines, sine combs,
  and noise at set level differences into its ignored `output/` directory, for checking the spatial spectrogram by eye.
- `fixtures/spectrogram_expected/` holds short PCM16 inputs and sparse NumPy power expectations for the spectrogram,
  which both test suites read in place.
  `uv run --script fixtures/spectrogram_expected.py` regenerates them independently of either application's DSP,
  and `format.yml` runs it with `--check`.
  Both applications share the bounded `spectrogram_cache` table and its `SPEC` codec.
  Their grids may differ by one intensity step in at most one cell per thousand,
  because `juce::dsp::FFT` and `realfft` round their last bits differently.
  The Rust shared-cache tests measure that documented exception against the C++ test executable.
- `fixtures/limiter_model.py` fits the empirical limiter model of the gain-adjusted integrated loudness estimate.
  Its observations come from the Rust `limiter_probe` example, and `fixtures/README.md` describes both.
  When the fit changes, update the `LIMITER_LOSS_*` constants and the expected estimates in both applications.
- `fixtures/normalization/` holds a larger collection for comparing the normalized output of both applications.
  It has music-like audio in representative supported formats, sample rates, and bit depths,
  from a fifth of a second to 30 seconds long, with a wide range of tags and edge cases.
  The audio is stored with Git LFS, so run `git lfs install` once per clone.
  `uv run fixtures/normalization/generate.py` regenerates it with ffmpeg and mutagen.
  `rust/scripts/parity.sh` and the parity workflow read it, and the Rust golden tests do when the LFS files are present.
- `fixtures/benchmark/generate.py` generates the long audio inputs for the Rust Criterion benchmarks.
  Its `output/` directory is ignored by git, and the benchmarks skip files that are absent.
  Run it through uv before measuring decoding or file analysis throughput.
- `cpp/tests/fuzz/corpus/` holds the seeds of the C++ libFuzzer harnesses, run by `cpp/fuzz.sh`.
  The Rust cargo-fuzz targets `serato_tags` and `cli_arguments` in `rust/fuzz/` share their input layouts,
  and `rust/fuzz.sh` replays that corpus in place next to its own seeds in `rust/fuzz/corpus/`.
  Change a harness input layout in both projects together.
  The Rust-only `cache_blobs` and `decode_audio` targets have only their own seeds.
  `rust/miri.sh` checks shared host lifecycle, VST3 and in-process CLAP hosting, both dev test plugins,
  the Serato parsers, playback callback admission, and cache blob readers under Miri.
- `.agents/rules/` holds the rules for the C++ sources of the C++ application, the test plugin, and ImageControl.
  `.claude/rules` is a symlink to it, and the `.mdc` files in `.cursor/rules/` are symlinks to its files,
  because Cursor reads only `.mdc` rules there.
  Edit the canonical files in `.agents/rules/` only.
- RealtimeSanitizer checks the audio callbacks of all three C++ and Rust projects on macOS with Homebrew LLVM:
  `cpp/sanitize.sh --realtime`, `rust/rtsan.sh`, and `image-control-plugin/sanitize.sh --realtime`.
  `tests/realtime_sanitizer.py` is their shared bounded runner, which runs each scenario in its own process.
  It separates detector controls and clean callback cases from verified `BLOCKED` audits of known dependency findings.
  A `BLOCKED` audit is not a clean result, and these checks detect exercised unsafe calls, not audio deadlines.
- `tests/callback_benchmark.py` compares paired, hardware-free playback callback runs of a baseline and a candidate
  Release executable of either application, for changes to callback synchronization.
  [`docs/performance.md`](docs/performance.md) records the measured CLI, VST3, and callback comparisons
  and how to reproduce them.
  Benchmarks run locally only, never in CI.
- `tests/cli_integration.py` runs the same executable-level CLI contracts against either built CLI through uv.
  Pass `--cli <absolute-executable-path>` and optionally `--artifacts <directory>` for failure diagnostics.
  It isolates application storage with `AUDIOBATCH_DATA_ROOT`
  and normalizes only temporary fixture copies with `--delete`.
  CTest runs it with the `integration` label, and the Rust workflow runs it after building the CLI.
- `tests/gui_controls.json` lists the main-window controls that both GUIs expose with the same stable id,
  accessibility role, and label, and maps each shared role to the native GPUI and JUCE roles.
  The C++ `ManifestControlsExistWithRoles` integration case
  and the Rust `integration_manifest_controls_exist_with_roles` test both read it,
  so a control renamed, removed, or given another role in one application fails both suites.
  Add a control there when both applications gain it, and drive GUI workflows through these ids.
  Both GUI integration tiers send real pointer, key, wheel, and file-drop events to the production view
  and assert application results, never screenshots or pixels.
  The C++ tier drives a transparent desktop window through `cpp/tests/GuiDriver.*`,
  and the Rust tier uses gpui-kit's test window helpers.
- `pyproject.toml` configures ruff for the Python scripts in `fixtures/`, `tests/`, `rust/scripts/`,
  and `image-control-plugin/`, and pins it in `uv.lock`.
  Python scripts generate fixtures, compare or drive the real applications and plugins, and run local benchmarks.
  Do not add Python tests of the shell scripts, Python tooling, or CI configuration.
  Run Python only through [uv](https://docs.astral.sh/uv/), never a bare `python3`.
  Each script declares its Python version and packages in an inline `# /// script` block
  and runs in its own environment with `uv run --script`, so add a script's packages there, not to `pyproject.toml`.
  Check the scripts with `uv run ruff format` and `uv run ruff check --fix` from the repository root.
- `.pre-commit-config.yaml` runs clang-format and cmake-format on `cpp/`, `test-plugin/`, and `image-control-plugin/`,
  cargo fmt on `rust/`, ruff on the Python scripts, shfmt and shellcheck on the shell scripts,
  and basic file checks through [prek](https://prek.j178.dev).
- CI builds, tests, lints, format-checks, sanitizes, fuzzes, and measures coverage of the applications and plugins.
  It runs no benchmarks and no tests of the scripts themselves.
  `.github/workflows/cpp.yml` builds and checks the C++ project, and `rust.yml` the Rust port.
  Their path filters include their own project, fixtures, the test plugin, assets, shared shell helpers,
  the root `tests/` scripts, and their workflow files, and exclude Markdown-only changes.
  The C++ workflow excludes the normalization collection.
  `format.yml` runs the root `./format.sh --check` on every change,
  checks that `fixtures/plugin_expected/` matches what its script writes,
  and verifies the independent spectrogram and source-metric expectations with their generators' `--check` modes.
  `cpp.yml` builds and tests Debug and Release on macOS and Windows,
  and runs coverage, clang-tidy, the sanitizers, RealtimeSanitizer, and fuzzing.
  `rust.yml` checks out only the JUCE submodule, to build the test plugin.
  Its `miri`, `fuzz`, and `realtime` jobs run `rust/miri.sh`, `rust/fuzz.sh`, and `rust/rtsan.sh` on macOS with nightly Rust.
  The realtime job requires clean first-party callbacks with `--skip-native`,
  then verifies known blockers with `--diagnostic`.
  A separate non-blocking job fuzzes the decoder for diagnostics.
  Its path filters also include the C++ fuzz corpus that the Rust fuzz job replays.
  Its Windows build job downloads the LFS fixtures and requires the full normalization corpus.
  `parity.yml` compares the levels both CLIs report and the files they write when normalizing,
  downloading the LFS fixtures for the macOS CLI comparison.
  It also runs the shared-cache tests in both directions.
  `image-control.yml` builds and tests ImageControl in Debug and Release on macOS and Windows,
  validates the VST3 with the pinned pluginval, and validates its Audio Unit on macOS.
  It builds, tests, and validates the Rust implementation's formats on both platforms,
  and compares both packaged implementations.
  It also runs clang-tidy, AddressSanitizer, ThreadSanitizer, RealtimeSanitizer, and coverage on macOS,
  and Miri, RealtimeSanitizer, and a license audit for the Rust implementation.
  Its path filters cover the plugin, JUCE submodule, shared C++ style, optimization, and pluginval configuration,
  native CPU detection helper, root `common.sh`, the shared realtime runner,
  the Python configuration, and the workflow itself, excluding Markdown-only changes.

## Isolated Application Storage

`AUDIOBATCH_DATA_ROOT` selects an absolute storage root for integration tests and isolated runs.
Under it, both applications share `AudioBatch/analysis.db`,
while Rust-specific settings, logs, and plugin data stay in `AudioBatch-rs/`
and C++-specific data in `AudioBatch/`.
An unset variable preserves the native production paths.
A supplied invalid root must never fall back to normal application storage.
In-process tests inject their paths instead of changing environment variables while workers or other tests run.

## Feature Scope

Feature requests apply to both the C++ and Rust applications by default.
Implement the feature in both versions, including matching behavior, presentation, settings, and tests where applicable.
Work on only one version when the user explicitly specifies C++ or Rust.
Do not infer single-version scope from the currently open file or the implementation discussed most recently.

CLAP hosting is a Rust-only capability on macOS and Windows.
The bundled JUCE has no CLAP hosting support, so the C++ application's supported plugin formats remain unchanged.
This format capability difference does not relax parity for operations both applications support.

## Behavioral Parity

For the same input and supported operation, the C++ and Rust applications must produce the same meaningful results.
This includes parsed metadata, names, positions, colors, numerical values, analysis results, and processed audio.
Different results are bugs to investigate and fix, not features of the implementations.
Small loudness and normalization differences are allowed within the existing documented test tolerances.
These allowances do not extend to parsed metadata or the bit-exact true peak contract.
The C++ implementation is a reference,
not an oracle that overrides the file format or independently verified expectations.

Use shared, independent expectations and cross-implementation tests where possible.
Do not add implementation-specific expected values or widen tolerances merely to make divergent results pass.
A passing test with a tolerance establishes only its bounded contract, not exact equality.
Keep presentation fallbacks separate from parsed source values,
and apply the same presentation rules in both applications.

An exception requires a strong, documented reason,
such as a platform capability or a dependency behavior that cannot reasonably be reconciled in project code
without unsafe or impractical third-party patches.
Using different libraries does not by itself justify different results.
Document each exception beside the affected behavior and tests, including its cause, scope, and measurable bounds.
Keep exceptions narrow, and revisit them when their technical constraints change.

## Working Rules

### Numeric calculations and performance

- Group decimal integer parts only when they have five or more digits.
  Leave four or fewer integer digits ungrouped, such as `4000` and `4000.0`.
  Use groups of three from the right for larger integer parts, such as Rust/Python `40_000` and C++ `40'000`.
  Fractional groups remain allowed independently, such as `1681.974_450_955_533`.
  Preserve radix literals, fractional precision, identifiers, and serialized or user-facing formats.
- Prefer multiplication by one half over division by two for floating-point calculations,
  such as `value * 0.5f` for C++ `float` and `value * 0.5` for `double` or inferred Rust floating-point values.
  Leave integer division unchanged.
  Preserve overflow-safe midpoint helpers and expressions whose numerical contract requires another evaluation.
- Evaluate fixed calculations at compile time when doing so makes the code clearer or removes runtime work.
  Prefer existing named mathematical constants, such as Rust's `FRAC_PI_2` instead of `PI * 0.5`.
  Otherwise reuse invariant results at the narrowest appropriate scope, outside repeated sample or comparison loops.
  Keep mutable processing state separate from shared immutable calculation data.
- Prefer named constants over unexplained numeric literals when a value represents a domain concept,
  protocol field, policy limit, or reused value. Keep self-evident arithmetic and conventional values inline.
- Preserve operation order, precision, rounding, and the behavioral parity contracts when optimizing numeric code.
  Do not substitute approximate constants, arbitrary reciprocals, or fast math without verification.
  Validate DSP tables against the native reference on every supported architecture before replacing native calculations.
- Fused multiply-add operations, such as Rust's `mul_add` and C++ `std::fma`, are allowed.
  They round once instead of twice, so they change the last bits of a result.
  Check them where tests compare exact values,
  such as the bit-exact true peak contract, the goldens, and peak normalization output.
  Elsewhere results only need to be close enough that no difference is visible or audible.
- Measure runtime changes with the existing benchmarks.
  [`docs/performance.md`](docs/performance.md) describes the benchmark procedures and the recorded results.
  Compilers often already optimize division by two and common subexpressions,
  so distinguish clearer source code from demonstrated performance improvements.

### Shader sources

- Keep first-party shader programs in dedicated files with their native language and stage extensions,
  such as `.vert`, `.frag`, `.glsl`, `.wgsl`, or `.metal`, rather than inline strings in host-language code.
  Keep them beside the owning renderer or in its shader directory so editors and shader tools can parse them directly.
- Embed shader files at build time, using generated resources in C++ and `include_str!` in Rust.
  Packaged applications and plugins must not depend on source-tree paths or runtime shader file reads.
  Register every shader as a build input so edits rebuild its embedded resource.
- Include shader sources in the applicable formatting and validation checks.
  Preserve shader arithmetic, precision, interfaces, and rendering behavior during extraction or formatting.
  Generated embedding code is a build artifact, not a second maintained copy of the shader.

### Repository work

- The repository-root `Cargo.toml` and `Cargo.lock` define the shared Rust workspace for AudioBatch
  and primary ImageControl.
  Shared dependencies and Rust lints belong there. ImageControl-only dependencies stay in its own crate manifests.
  Cargo commands work from the root. Scope application or plugin work with explicit packages rather than `--workspace`.
  ImageControl has one Rust implementation: `crates/dsp`, `crates/plugin`, and `crates/xtask` under its Rust directory.
- Run each project's build, format, lint, and test scripts from its own directory.
  The root `lint.sh` lints the C++ project, the Rust project, and ImageControl in one go,
  and `lint.sh --check` reports warnings without applying fixes.
- First-party scripts provide both short and long forms for optional arguments whenever possible.
  Reuse the same spelling and short character for the same concept across scripts, and document both in help.
  Reserve `-h` for help and `-v` for verbosity. Do not reuse them for another option in a script without that feature.
  Preserve existing spellings as compatibility aliases when standardizing names.
  Keep positional arguments and `--` passthrough intact.
  Common pairs include `-h/--help`, `-v/--verbose`, `-d/--debug`, `-c/--check` in checkers,
  `-c/--clean` in builders, `-j/--jobs`, `-t/--test`, `-V/--validate`, `-L/--validation-level`,
  `-N/--no-build`, `-B/--build-only`, `-A/--artifacts`, and `-R/--realtime` in sanitizer scripts.
  Parameter-taking shell entry points print useful help and exit successfully
  before builds, installations, required-tool checks, or other side effects.
  Use `REPO_ROOT` from root `common.sh` for repository-level paths.
  Do not reconstruct the root through project `..` paths.
- Stage with explicit paths, such as `git add cpp`, `git add rust`, or `git add image-control-plugin`.
- Keep README updates concise and targeted, preserving existing structure and useful content.
  Do not fully rewrite or broadly condense an existing README unless explicitly requested.
  Focus additions on purpose, key capabilities, essential setup and commands, and important caveats.
  Link to architecture, agent instructions, or focused reference documentation instead of duplicating implementation details.
  Do not turn READMEs into detailed UI walkthroughs.
  Keep rolling requirements such as the Rust toolchain version in their authoritative configuration, not duplicated prose.
- Commit messages and all prose follow the semantic line break style described in the project files.
- Never use `feat:` or `chore:` commit prefixes, including scoped forms such as `feat(area):` or `chore(area):`.
  Prefer a descriptive project or topic prefix, such as `rust:`, `image-control:`, `build:`, or `docs:`.
- **NEVER run destructive git commands**, such as `git checkout -- <file>`, `git restore`, `git reset --hard`,
  `git clean`, or `git stash drop`.
  Ask the user to do it manually if something needs undoing.
- `CLAUDE.md` is a symlink to this file. Only edit `AGENTS.md` directly.
