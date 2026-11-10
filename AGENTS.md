# AGENTS.md

AudioBatch analyzes and batch processes audio files.
This repository holds two versions of it side by side, each with its own agent instructions:

- `cpp/`: the C++23 and JUCE application, the reference implementation.
  Read [`cpp/AGENTS.md`](cpp/AGENTS.md) before changing anything under `cpp/`.
- `rust/`: the Rust port. Read [`rust/AGENTS.md`](rust/AGENTS.md) before changing anything under `rust/`.

Each version also has an `ARCHITECTURE.md`, [`cpp/ARCHITECTURE.md`](cpp/ARCHITECTURE.md) and [`rust/ARCHITECTURE.md`](rust/ARCHITECTURE.md),
which explains how its parts work together.

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
- `common.sh` holds the shell helpers shared by the scripts of both projects,
  such as platform detection and colored output.
  `cpp/common.sh` and `rust/common.sh` source it and add their project specific parts.
- `build.sh`, `install.sh`, and `format.sh` run the scripts of the same name in both projects,
  or in the one named by their first argument, `cpp` or `rust`, through `run_project_scripts` in `common.sh`.
  Unqualified `install.sh` also installs ImageControl after both applications, even with `--cli-only`.
  `install.sh image-control` installs only the plugin, and root installer options are not forwarded to it.
  Run `image-control-plugin/install.sh` directly for plugin-specific options.
  `format.sh` also formats the root shell scripts and the prose outside the projects when shfmt and slb are installed,
  runs ruff on the Python scripts through uv,
  and formats tracked and non-ignored new JSON files with jq using two-space indentation.
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
- `image-control-plugin/` holds ImageControl, a multiband mid/side stereo imaging effect by esgrove,
  as a VST3 and, on macOS, an Audio Unit, described in `image-control-plugin/README.md`.
  It is a real plugin for processing audio, mainly from AudioBatch's plugin chain, and not part of either application.
  It is C++ only, so the feature scope and behavioral parity rules below do not apply to it,
  and its names and ids never carry AudioBatch branding.
  `image-control-plugin/build.sh` builds it with the JUCE submodule in `cpp/JUCE`,
  and runs its GoogleTest unit tests with `--test`.
  `image-control-plugin/install.sh` builds it with `build.sh` and installs it into the plugin folders.
  `cpp/format.sh` formats it, and `image-control-plugin/lint.sh` checks it with the C++ clang-tidy configuration.
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
 `rust/miri.sh` runs the unsafe VST3 hosting code, the Serato parsers, and the cache blob readers under Miri.
- `.agents/rules/` holds rules for the C++ sources, and `.claude/rules` is a symlink to it.
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
  `image-control-plugin/benchmarks/`, and `image-control-plugin/test-signals/`,
  and pins it in `uv.lock`.
  Run Python only through [uv](https://docs.astral.sh/uv/), never a bare `python3`.
  Each script declares its Python version and packages in an inline `# /// script` block
  and runs in its own environment with `uv run --script`, so add a script's packages there, not to `pyproject.toml`.
  Check the scripts with `uv run ruff format` and `uv run ruff check --fix` from the repository root.
- `.pre-commit-config.yaml` runs clang-format and cmake-format on `cpp/`, `test-plugin/`, and `image-control-plugin/`,
  cargo fmt on `rust/`, ruff on the Python scripts, and basic file checks through [prek](https://prek.j178.dev).
- `.github/workflows/cpp.yml` builds and checks the C++ project, and `rust.yml` the Rust port.
  Their path filters include their own project, fixtures, the test plugin, assets, and shared shell helpers.
  They also include their workflow files, and exclude Markdown-only changes.
  The C++ workflow excludes the normalization collection.
  `format.yml` runs the root `./format.sh --check` on every change,
  checks that `fixtures/plugin_expected/` matches what its script writes,
  and verifies the independent spectrogram and source-metric expectations with their generators' `--check` modes.
  `rust.yml` checks out only the JUCE submodule, to build the test plugin.
 Its macOS build job also runs `tests/test_mac_signing.py`, the packaging contracts of the shared signing helper.
 Its `miri` and `fuzz` jobs run `rust/miri.sh` and `rust/fuzz.sh` on macOS with nightly Rust,
 and its path filters also include the C++ fuzz corpus that the Rust fuzz job replays.
  Its Windows build job downloads the LFS fixtures and requires the full normalization corpus.
  `parity.yml` compares the levels both CLIs report and the files they write when normalizing,
  downloading the LFS fixtures for the macOS CLI comparison.
  `image-control.yml` builds and tests ImageControl on macOS and Windows,
  runs clang-tidy on macOS, and validates its Audio Unit on macOS.
  Its path filters cover the plugin, JUCE submodule, shared C++ style and optimization configuration,
  native CPU detection helper, root `common.sh`, and the workflow itself, excluding Markdown-only changes.

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
  Compilers often already optimize division by two and common subexpressions,
  so distinguish clearer source code from demonstrated performance improvements.

### Repository work

- Run each project's build, format, lint, and test commands from its own directory.
  The root `lint.sh` lints the C++ project, the Rust project, and ImageControl in one go,
  and `lint.sh --check` reports warnings without applying fixes.
- Stage with explicit paths, such as `git add cpp` or `git add rust`.
- Keep README updates concise and targeted, preserving existing structure and useful content.
  Do not fully rewrite or broadly condense an existing README unless explicitly requested.
  Focus additions on purpose, key capabilities, essential setup and commands, and important caveats.
  Link to architecture, agent instructions, or focused reference documentation instead of duplicating implementation details.
  Do not turn READMEs into detailed UI walkthroughs.
  Keep rolling requirements such as the Rust toolchain version in their authoritative configuration, not duplicated prose.
- Commit messages and all prose follow the semantic line break style described in both project files.
- **NEVER run destructive git commands**, such as `git checkout -- <file>`, `git restore`, `git reset --hard`,
  `git clean`, or `git stash drop`.
  Ask the user to do it manually if something needs undoing.
- `CLAUDE.md` is a symlink to this file. Only edit `AGENTS.md` directly.
