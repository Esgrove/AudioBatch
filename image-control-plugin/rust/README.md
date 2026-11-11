# ImageControl RS

ImageControl RS is an independent Rust multiband stereo-imaging plugin with a native Iced/wgpu editor.
It provides VST3 and CLAP on macOS and Windows, and an experimental self-contained AUv2 on macOS.
It shares the repository's Cargo workspace, but does not use AudioBatch's application services or GUI.
It installs alongside the C++ ImageControl plugin rather than replacing it.

The implementation targets macOS and Windows.
Native macOS processing, editor, and GPU tests are exercised locally.
Windows MSVC cross-checks pass, but native Windows execution and graphics remain unverified.
The C++ plugin remains the supported implementation.

## Features

- Four complementary linear-phase FIR bands with independent Mid, Side, Width, Recover, Pan, and Solo controls.
- Output trim, latency-aligned Delta, latency-preserving bypass, and a final Mono fold, with 20 ms control ramps.
- Bit-exact delayed input at settled neutral settings, with warm convolution history for processing changes.
- Background crossover-kernel design for realtime use and synchronous deterministic design for offline rendering.
- Six-tier spatial spectral analysis with frequency reassignment, stereo position, and phase coherence.
- GPU particles, persistent energy, five-level bloom, and history-preserving resize, with frequency or phase coloring.
- Consistent Iced controls and embedded font weights,
  editable numeric values, compact bands, and modal display settings.
- An embedded resize grip with host-approved logical window sizes retained across close/reopen and saved projects.
- Functional Pre/Post analysis, persistence and contrast settings, and processed-band correlation meters.
- Thirty-one stable automatable sound parameters and independent versioned state for sound, display, and window geometry.

At 48 kHz, processing latency is 4352 samples and the conservative tail beyond compensation is 4096 samples.
Long bass-analysis windows affect display responsiveness, not the reported audio latency.
The spectrogram uses relative adaptive brightness and is not an absolute level meter.

Recover ranges from 0–200%, matching C++.
It adds a finite quadrature-rotated contribution from removed Side amplitude when Width is below one.
At 200%, the recovered contribution is twice its 100% amplitude, not twice the overall output level.
It is not automatic loudness compensation or a power-preserving width law.

## Build and test

Run scripts from this directory.
Use the Rust toolchain declared by the [root workspace](../../Cargo.toml), a native platform build toolchain,
and a GPU supported by wgpu's Metal or Direct3D backend.
The Rust plugin does not require JUCE to compile.
The macOS AU build additionally requires CMake and an Apple C++/Objective-C++ toolchain.
It fetches immutable upstream clap-wrapper, CLAP SDK, and AudioUnitSDK revisions into the selected build tree.
The component statically links the Rust CLAP implementation and does not search for an installed CLAP.
Build-tree validation can build the repository-pinned pluginval when it is not on `PATH`.

```shell
# Release VST3 and CLAP, plus AUv2 on macOS, without installation.
./build.sh

# Debug build, complete scoped tests, and build-tree pluginval validation.
./build.sh --debug --test --validate

# Release build and tests.
./build.sh --test

# Full native contracts, including packaged editor open/close/reopen.
./test.sh

# Static checks and additional verification.
./format.sh --check
./lint.sh --check
./test.sh --miri
./test.sh --licenses
./rtsan.sh
./rtsan.sh --format clap
# Verify known AU dependency reports as BLOCKED, not clean callback evidence.
./rtsan.sh --format au --diagnostic
```

The default artifacts are `rust/target/bundled/ImageControl RS.vst3`, `ImageControl RS.clap`,
and on macOS `ImageControl RS.component`, relative to the repository root, not this directory's ignored `target/`.
The Windows CLAP artifact is a single file. The macOS CLAP and AU artifacts are bundles.
`CARGO_TARGET_DIR` overrides the artifact location, resolved from the invocation directory before scripts change directories.
Explicit external target roots are allowed, with canonical path checks rejecting symlink escapes.
Debug and Release packaging use the same bundle path, so the last build selects its contents.
`./test.sh` requires an existing build-tree bundle.
Every script provides `-h` and `--help`.

The packaged realtime checks require macOS, nightly Rust, uv, jq, and a matching Homebrew LLVM runtime.
They verify allocation and mutex detector controls separately from required-clean callback cases.
Passing these exercised scenarios does not establish audio deadlines or arbitrary host compatibility.

## Cross-implementation tests and benchmarks

The packaged parity host compares both actual VST3s with deterministic signals and independent neutral,
gain, pan, mono, latency, and tail expectations.
It uses plain-unit parameter mappings, not shared normalized IDs or project-state migration.

```shell
# Build C++ once with tests, then its independent packaged-plugin comparison host.
../build.sh --test
cmake --build ../cmake-build-release --config Release --target ImageControlParity
./build.sh
./parity.sh --cpp "$PWD/../cmake-build-release/ImageControl_artefacts/Release/VST3/ImageControl.vst3" \
    --executable "$PWD/../cmake-build-release/ImageControlParity_artefacts/Release/ImageControlParity"

# Preallocated production DSP/analysis benchmarks, with preparation measured separately.
cargo bench -p image-control-dsp --bench dsp -- --quick

# Complete-process hyperfine runs with separate warmed-callback diagnostics.
./benchmark.sh --case recover --frames 960000 --runs 5 --warmup 1
```

Use the `.exe` parity host on Windows.
`parity.sh` rejects incomplete setup rather than skipping a missing counterpart.
The Rust integration test is optional without counterpart selection, and CI supplies both build-tree bundles.
Peak residual must stay below `1e-5` full scale and RMS residual below `1e-6`, with exact neutral checks retained.
Reports live under the shared target's `image-control-parity/` directory.
Benchmark artifacts include exact binary hashes, compiler/source identity, workload, checksums, and machine metadata.
Run timings with other builds idle. There is no implied speedup without a matched baseline.
See [the evaluation](docs/rust-evaluation.md#cross-implementation-and-performance-evidence) for measured scope
and limitations.

## Install

Close hosts using ImageControl RS before replacing its installed bundle.

```shell
./install.sh
./install.sh --no-build --validate
```

The default per-user VST3 destinations are `~/Library/Audio/Plug-Ins/VST3` on macOS
and `%LOCALAPPDATA%/Programs/Common/VST3` on Windows.
CLAP uses the sibling `CLAP` folder. The macOS Audio Unit uses `~/Library/Audio/Plug-Ins/Components`.
Use `--destination <directory>`, `--clap-destination <directory>`, and `--au-destination <directory>`
to select format-specific folders, and `--validation-level <5-10>` for installed validation.
Rescan the host and select **ImageControl RS**.
The installer never replaces the C++ ImageControl bundles.
Installed AU validation registers and checks the distinct `aufx / Egir / Esgr` component with strict auval.
Build-tree tests load the exact artifacts without installation or registrar changes.
Local macOS signing is ad-hoc, not Developer ID signing or notarization.

## Scope and limitations

- The VST3 retains its own class identity and `com.esgrove.imagecontrol-rs-own` bundle identity.
  CLAP uses descriptor ID `com.esgrove.imagecontrol-rs-own`, while the AU has a separate component identity.
  Formats share the Rust state codec, not JUCE ImageControl project compatibility or the removed Nice experiment.
- AUv2 is an experimental wrapper route, not equivalent qualification to the own-wrapper VST3.
  Upstream offline-mode forwarding, automation contention, and lifecycle/state behavior have explicit diagnostics.
  Nonblocking render admission can still produce silence under wrapper contention, which RTSan alone cannot qualify.
- Only matching mono-to-mono and stereo-to-stereo main buses and 32-bit floating-point processing are supported.
  Automation is sampled per host block, not sample-accurate within a block.
- Audio preparation supports actual rates from 1 through 2,822,400 Hz.
  Spatial analysis supports 8 through 768 kHz, reporting unavailable outside that range without disabling audio.
- Undo/Redo, A/B, and presets remain visibly unavailable.
  Link and layout choices are local editor state, not part of the persisted display-preference snapshot.
- Windows CLAP currently exposes host-provided parameter controls, not the custom Iced editor.
  CLAP destruction cannot use VST3's rejected-removal retry protocol during a reentrant Win32 callback.
  The custom CLAP GUI is not advertised there until safe synchronous teardown is implemented and tested.
- Full Iced device-loss recovery, native accessibility qualification, safe arbitrary module unloading,
  and complete upstream widget-cache reclamation are not established.
  Windows reentrant removal may be rejected until the host retries outside an active callback.

See [ARCHITECTURE.md](ARCHITECTURE.md) for ownership, DSP, analysis, rendering, state, and verification contracts.
The bounded `editor_compare` example hosts explicit C++ and Rust build-tree bundles with the same stereo comb.
Use it for manual layout and spectrum comparison without audio hardware.
[The evaluation](docs/rust-evaluation.md) records dependency research, historical experiments, and production gates.
[Plugin agent instructions](../AGENTS.md) and [repository instructions](../../AGENTS.md) govern development.
