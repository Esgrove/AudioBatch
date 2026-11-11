# ImageControl RS

ImageControl RS is an independent Rust multiband stereo-imaging VST3 with a native Iced/wgpu editor.
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
- GPU particles, persistent energy, bloom, and history-preserving resize, with frequency or phase coloring.
- Functional Pre/Post analysis, persistence and contrast settings, and processed-band correlation meters.
- Thirty-one stable automatable sound parameters and independent versioned host state containing display preferences.

At 48 kHz, processing latency is 4352 samples and the conservative tail beyond compensation is 4096 samples.
Long bass-analysis windows affect display responsiveness, not the reported audio latency.
The spectrogram uses relative adaptive brightness and is not an absolute level meter.

Recover adds a finite quadrature-rotated contribution from removed Side amplitude when Width is below one.
It is not automatic loudness compensation or a power-preserving width law.

## Build and test

Run scripts from this directory.
Use the Rust toolchain declared by the [root workspace](../../Cargo.toml), a native platform build toolchain,
and a GPU supported by wgpu's Metal or Direct3D backend.
The Rust plugin does not require JUCE to compile.
Build-tree validation can build the repository-pinned pluginval when it is not on `PATH`.

```shell
# Release VST3, without installation.
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
```

The default bundle is `rust/target/bundled/ImageControl RS.vst3` relative to the repository root,
not this directory's ignored `target/`.
`CARGO_TARGET_DIR` overrides the artifact location.
Debug and Release packaging use the same bundle path, so the last build selects its contents.
`./test.sh` requires an existing build-tree bundle.
Every script provides `-h` and `--help`.

The packaged realtime checks require macOS, nightly Rust, uv, jq, and a matching Homebrew LLVM runtime.
They verify allocation and mutex detector controls separately from required-clean callback cases.
Passing these exercised scenarios does not establish audio deadlines or arbitrary host compatibility.

## Install

Close hosts using ImageControl RS before replacing its installed bundle.

```shell
./install.sh
./install.sh --no-build --validate
```

The default per-user destinations are `~/Library/Audio/Plug-Ins/VST3` on macOS
and `%LOCALAPPDATA%/Programs/Common/VST3` on Windows.
Use `--destination <directory>` for another VST3 folder and `--validation-level <5-10>` for installed validation.
Rescan the host and select **ImageControl RS**.
The installer neither replaces C++ `ImageControl.vst3` nor installs an Audio Unit.
Local macOS signing is ad-hoc, not Developer ID signing or notarization.

## Scope and limitations

- The plugin has its own class identity and `com.esgrove.imagecontrol-rs-own` bundle identity.
  Its state is not compatible with JUCE ImageControl projects or the removed Nice experiment.
- Only matching mono-to-mono and stereo-to-stereo main buses and 32-bit floating-point processing are supported.
  Automation is sampled per host block, not sample-accurate within a block.
- Audio preparation supports actual rates from 1 through 2,822,400 Hz.
  Spatial analysis supports 8 through 768 kHz, reporting unavailable outside that range without disabling audio.
- Undo/Redo, A/B, and presets remain visibly unavailable.
  Link and layout choices are local editor state, not part of the persisted display-preference snapshot.
- Full Iced device-loss recovery, native accessibility qualification, safe arbitrary module unloading,
  and complete upstream widget-cache reclamation are not established.
  Windows reentrant removal may be rejected until the host retries outside an active callback.

See [ARCHITECTURE.md](ARCHITECTURE.md) for ownership, DSP, analysis, rendering, state, and verification contracts.
[The evaluation](docs/rust-evaluation.md) records dependency research, historical experiments, and production gates.
[Plugin agent instructions](../AGENTS.md) and [repository instructions](../../AGENTS.md) govern development.
