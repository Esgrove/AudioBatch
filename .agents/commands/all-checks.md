---
description: Run the complete applicable AudioBatch verification suite, fix issues, and report remaining coverage gaps.
argument-hint: "[all|audiobatch|cpp|rust|imagecontrol] [check-only] [additional instructions]"
---

# Run all checks

Run the project's checks, not just a plan or a list of commands.
Arguments: $ARGUMENTS
Treat arguments or instructions appended to this command as the requested scope, mode, and constraints.
Default to the whole repository, fixing issues found by the checks and rerunning affected validation.
`check-only` runs the checks and reports issues without editing source, configuration, or expectations.
Do not use autofixing commands in check-only mode. State the selected scope and mode before starting.

## Scope and preparation

- `all`, or no scope: the whole repository, including both applications, ImageControl, and shared tooling.
- `audiobatch`: both `cpp/` and `rust/`, plus their shared fixtures, test plugin, assets, tests, and tooling.
  Exclude ImageControl-specific work.
- `cpp`: the C++ application and shared support that affects it.
- `rust`: the Rust application and shared support that affects it, including the JUCE test plugin.
- `imagecontrol`, `image-control`, `image-control-plugin`, or `imagecontrol plugin`: `image-control-plugin/`
  and the shared C++ configuration, JUCE, assets, helpers, and tests it depends on.

Read root `AGENTS.md`, the selected projects' instructions and architecture documents,
and their verification scripts and relevant `.github/workflows/` files before running commands.
For ImageControl, read its `README.md` and `ARCHITECTURE.md` and the applicable `.agents/rules/` files.
The current scripts and workflows are authoritative if this checklist becomes stale.
Support only macOS and Windows. Run each project's commands from its own directory.

1. Record `git status --short`, the host, compiler and toolchain versions, and the selected scope.
   Preserve existing work. Do not stage, commit, clean build directories, or run destructive Git commands.
2. Check prerequisites before expensive builds.
   These include the JUCE submodule, CMake, the native C++ toolchain, uv, and the Rust toolchain as applicable.
   Full macOS checks also need Homebrew LLVM and Ninja, nextest, cargo-llvm-cov, cargo-fuzz,
   nightly Rust with Miri and rust-src, and the matching Rust/LLVM runtime required by `rust/rtsan.sh`.
   Inspect script help for exact requirements. Do not silently install global tools or alter toolchain defaults.
   Initialize missing submodules or download LFS objects only when permitted, without overwriting local changes.
3. For application checks, ensure `fixtures/normalization/` contains materialized LFS audio, not pointer files.
   Use the repository's Git LFS setup and `git lfs pull` when downloads are permitted.
   Otherwise report the full-corpus checks as blocked, not passed.
4. Make a checklist of every applicable tier below.
   Run independent checks separately so one failure does not hide later checks.
   Do not run competing builds in the same workspace or desktop GUI suites concurrently.
   Avoid concurrent full-core builds and benchmarks. Preserve exit codes when capturing logs.
5. Use the existing isolated test runners. Never normalize committed fixtures in place,
   scan installed plugins, open physical audio devices, or use production application storage.
   Do not invoke `install.sh` as part of routine checks.

## Formatting and shared tooling

For `all`, run `./format.sh --check` from the repository root.
It covers both projects, ImageControl, shared C++ sources, shell scripts, Python, prose, and JSON.
For application-only scopes, run each selected project's `./format.sh --check` from its directory.
Note that `cpp/format.sh` also checks the test plugin and ImageControl.
For ImageControl-only checks, use that C++ formatter in check mode and identify its broader coverage in the report.
Do not invent an `image-control` selector for the root formatter.

The format scripts can skip optional tools such as shfmt and slb when absent.
Record those omissions explicitly. Check `.agents/commands/` and `.agents/rules/` with slb when available.
From the repository root, run `uv run --frozen ruff format --check` and `uv run --frozen ruff check` for Python.
Check shared JSON with the root formatter's jq two-space formatting contract.
Do not use autofixing pre-commit hooks in a check-only run.

For `all` or application scopes, also verify independent expectations from the repository root:

- `uv run --script fixtures/spectrogram_expected.py --check`.
- `uv run --script fixtures/metrics_expected.py --check`.
- Compare test-plugin expectations with `fixtures/plugin_expected.py` without rewriting committed expectations.
  This generator has no `--check` or output-directory option.
  Copy the generator and its required source WAV files into a matching scratch directory,
  run the copied script through uv, and compare the generated file set and bytes with `fixtures/plugin_expected/`.
  Do not delete or regenerate the working tree's expected-output directory.

Run shared Python script contracts with `uv run --script <path>` from the repository root:

- C++ scope: `tests/test_cpp_lint.py`, `tests/test_cpp_test.py`, and `tests/test_callback_benchmark.py`.
- Rust scope: `tests/test_rust_scripts.py`, plus `rust/scripts/test_distribution_licenses.py` on macOS.
  The Rust tooling tier below covers parity and benchmark regression tests.
- Either application on macOS: `tests/test_mac_signing.py`.
- Any selected project: `tests/test_realtime_sanitizer.py` and `tests/test_realtime_scripts.py`.
- ImageControl scope: `image-control-plugin/benchmarks/test_compare_formats.py`
  and `image-control-plugin/tests/test_build_scripts.py`.

Run each applicable script once, not once per project that uses it.
These test tooling behavior, not benchmark performance.

## C++ application

From `cpp/`, run the following for both `Debug` and `Release`, replacing `<type>` with the build type:

1. `./build.sh --build-type <type>` to build and package both GUI and CLI without installing.
2. `./test.sh --build-type <type> --build-only` to build every test target and the unit target's test-plugin dependency.
3. `./test.sh --build-type <type> --unit --no-build`.
4. `./test.sh --build-type <type> --integration --no-build` for executable CLI and driven GUI contracts.

Do not start with an integration-only build in a fresh workspace. It does not ensure the test plugin exists.
The driven GUI tier needs a usable desktop session and owns pointer and focus while running.
Do not enable visible stepping or automate the desktop outside the test process.
If no desktop is available, report GUI coverage as blocked. Run the executable CLI contracts separately:
`uv run --script ../tests/cli_integration.py --cli <absolute-built-cli-path>`.

Run `./lint.sh --prepare --build-type Debug --check` for the ordinary sources' full clang-tidy pass.
Do not replace a full check with `--changed` or `--staged`.
On Windows, select a supported Ninja compilation database as documented by `./lint.sh --help`.

Run `./sanitize.sh` on either platform.
On Windows, also run `./sanitize.sh --clang` when the LLVM toolchain is available,
reporting a missing toolchain rather than pretending this variant ran.
On macOS, also run:

- `./sanitize.sh --thread`.
- `./sanitize.sh --realtime`, then `./lint.sh --check --build-dir cmake-build-mac-rtsan`.
  This includes the real-time-only source omitted from the ordinary compilation database.
- `./fuzz.sh --build-only`, then `./lint.sh --check --fuzz`.
- `./fuzz.sh --no-build --replay`, then `./fuzz.sh --no-build --fuzz --seconds 60`.
- `./coverage.sh --lcov`, without opening a browser.

Sanitizer unit runs do not replace normal CLI and GUI integration tests.
Keep sanitizer configurations separate, and retain fuzz artifacts without changing committed seeds.

## Rust application

First run `./build.sh` from `test-plugin/` so real JUCE VST3 tests cannot silently skip.
Then, from `rust/`:

1. `cargo build --workspace --bins` and `./build.sh` for Debug binaries and Release packaging without installation.
2. On macOS, `cargo build -p audiobatch-gui --features snapshot` for the feature omitted by ordinary builds.
3. `./lint.sh --check`, covering both the workspace and the separately excluded fuzz crate.
4. `./test.sh --unit`.
5. `./test.sh --gui-integration`.
6. `./test.sh --optimized`.
7. `./test.sh --cli-integration`.
8. `./test.sh --tooling`.
9. `cargo test --workspace --doc` for documentation tests not run by nextest.
10. `cargo test --manifest-path fuzz/Cargo.toml --test decoder_regressions`.

Set `AUDIOBATCH_REQUIRE_NORMALIZATION_FIXTURES=1` on test invocations when the complete corpus is available.
A full run must not claim normalization coverage from a locally skipped corpus.
Inspect test output for skipped JUCE plugin and cross-language shared-cache checks too.

On macOS, also run:

- `./miri.sh`.
- `./fuzz.sh --build-only`, then `./fuzz.sh --no-build --replay`.
- `./fuzz.sh --no-build --fuzz --targets parsers --seconds 60` for required mutation coverage.
- `./fuzz.sh --no-build --fuzz --targets decoder --seconds 60` as a separately reported diagnostic campaign.
  Known decoder dependency defects do not turn a failed campaign into a clean result.
- `./rtsan.sh --skip-native --artifacts target/rtsan/artifacts/first-party`.
- `./rtsan.sh --diagnostic` for native and ownership audits with their documented blockers.
- `./coverage.sh --lcov`, without opening a browser.

Do not substitute Miri for native integration tests or invent a Rust `sanitize.sh`.
RTSan requires matching nightly Rust and Homebrew LLVM versions.
An unavailable or mismatched runtime is blocked validation, not a successful sanitizer run.

## Cross-implementation contracts

Run this tier for `all` and `audiobatch` after both applications are built.
For a single-implementation scope, run it when compatible counterpart binaries are available,
without expanding code edits outside the requested scope. Otherwise report the missing coverage.

From `rust/`, set `AUDIOBATCH_CPP_TESTS` to the absolute built C++ `AudioBatchTests` executable
and run `cargo test --release -p audiobatch-core --test shared_cache -- --nocapture`.
Set `AUDIOBATCH_CPP_CLI` to the absolute built Release C++ CLI for parity commands.
Resolve actual platform-specific artifact paths rather than assuming the macOS path on Windows.

- `./scripts/parity.sh --release ../fixtures`.
- `./scripts/parity.sh --release ../fixtures/normalization`.
- `./scripts/parity.sh --release --normalize --peak 0 ../fixtures/normalization`.
- `./scripts/parity.sh --release --normalize --peak -1 ../fixtures/normalization`.
- `./scripts/parity.sh --release --normalize --lufs -14 ../fixtures/normalization`.
- `./scripts/parity.sh --release --normalize --lufs -23 ../fixtures/normalization`.

On macOS, also run the independent AAC reconstruction contracts.
They require FFmpeg, FFprobe, and `afconvert`, with both Release CLIs available.
From `rust/`, run `uv run --script scripts/test_aac_reconstruction.py` with both required CLI arguments:
`--cpp-cli <absolute-cpp-cli-path>` and `--rust-cli <absolute-rust-cli-path>`.
Use the current binaries rather than stale packaged copies.
This checks Core Audio reconstruction, edit-list alignment, and PNS-disabled controls beyond ordinary parity.
Report missing prerequisites as blocked coverage.

The parity runner copies normalization inputs, isolates storage, and uses `--delete` only on temporary copies.
Do not use `scripts/snapshot.sh` as a substitute. Its normalization mode has system-trash side effects.
Do not relax tolerances or update goldens, checksums, or input identities to make failures pass.

## ImageControl plugin

From `image-control-plugin/`, run for both `Debug` and `Release`:

1. `./build.sh --test --build-type <type>`.
2. `cmake -S . -B cmake-build-<type-lowercase> -DAUDIOBATCH_PLUGINVAL=ON`.
3. `cmake --build cmake-build-<type-lowercase> --config <type> --target ImageControl_pluginval`.

Use the actual Ninja build directory on Windows if selected, rather than mixing CMake generators.
The explicit pluginval target uses the pinned validator, unlike a potentially different executable on `PATH`.
Missing validator dependencies are failures or blocked setup, never silent validation skips.

On macOS, also run `./sanitize.sh`, `./sanitize.sh --thread`, `./sanitize.sh --realtime`,
and `./coverage.sh --lcov`.
Then run `./lint.sh --check` on either platform.
Run lint after the macOS real-time build so it can include RTSan-only sources.

Audio Unit validation through `./install.sh --reuse --validate` installs plugins and changes the native registry.
Do not run it without explicit permission for those side effects.
Report it as not run unless authorized, and follow the current workflow for each build type when authorized.
The installed-format comparison benchmark also requires installation and is not a routine check.

## Completion and reporting

Wait for every started check and record its exit status.
Keep passing, failing, blocked, skipped, and diagnostic outcomes distinct, including exact commands and directories.
A successful RTSan wrapper can include verified `BLOCKED` dependency findings. Report those separately,
not as proof that every production callback is clean or that audio deadlines were met.

Unless `check-only` was requested, diagnose and fix issues found by the checks with minimal scoped changes.
Use the applicable formatters and lint autofixes where appropriate, and inspect every autofix for portability.
Rerun failed checks and validation affected by the changes until they pass or a concrete blocker remains.
In check-only mode, report findings and suggested fixes without applying them.
Never weaken tests, sanitizer controls, warning policies, or independent expectations to obtain a pass.

Finish with a concise summary of failures first, diagnostics and artifact paths, validation gaps,
and a final `git status --short` comparison identifying any unexpected modifications.
State which host and architecture were actually checked.
Do not claim Windows validation from a macOS run, or another architecture from the native run.
Performance benchmarks and profiles are additional work only when requested or needed to verify a performance change.
