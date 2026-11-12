# ImageControl RS current status

ImageControl RS is an independent Rust implementation of the ImageControl stereo-imaging effect.
It builds as VST3 and CLAP on macOS and Windows, with experimental AUv2 delivery on macOS.
The C++ plugin remains the supported implementation and the reference for features and audio behavior.

The Rust implementation has functional DSP, a native Iced/wgpu editor, independent state and preset formats,
and bounded packaged-plugin verification on both platforms. It is not yet qualified as a complete replacement.
The open platform, editor, renderer, and lifecycle gates are listed below.

## Implementation and selected stack

The implementation has three production crates and one macOS AU build:

- [`image-control-dsp`](../crates/dsp/src/lib.rs) implements the processing, kernel design, correlation,
  spatial analysis, particle projection, and bounded audio-to-editor transport.
- [`image-control-plugin-rs`](../crates/plugin/src/lib.rs) owns the VST3 and CLAP adapters,
  shared instance state, Iced editor, native window bridge, and wgpu rendering.
- [`image-control-xtask`](../crates/xtask/src/main.rs) packages VST3 and CLAP bundles
  and invokes the AU build on macOS.
- [`au/`](../au/CMakeLists.txt) builds the experimental AUv2 wrapper and its isolated native host.

The packages use the [repository Cargo workspace](../../../Cargo.toml) and root lockfile.
The plugin does not import AudioBatch application services or its GPUI application GUI.
The Rust package installs alongside the C++ plugin and keeps its own format identities and persistence.

### Plugin formats and host boundary

The VST3 uses the `vst3` Rust bindings and a narrow first-party adapter.
The binding exposes the VST3 ABI without imposing a higher-level parameter, state, or editor framework.
This keeps the plugin's class identity, parameter IDs, state streams, bus negotiation, and editor lifetime explicit.
The generated bindings use Steinberg VST SDK 3.8.0, whose SDK license is MIT.

The CLAP adapter uses [Clack](https://github.com/prokopyl/clack) for the plugin ABI.
VST3 and CLAP share the DSP, parameter registry, editor, and Rust state codec.
They have independent host-facing identity and format-specific lifecycle handling.

The direct wrapper avoids adding the GPLv3 `vst3-sys` dependency seen in the inspected NIH-plug route.
It also keeps the plugin's host identity and lifecycle under first-party control.
Nice-plug offers more integrated parameter and packaging services,
but its framework runtime is not needed for the chosen adapters and lifecycle control.
Neither framework's state format would provide C++ JUCE project compatibility automatically.

### Editor and rendering

The embedded editor uses Iced with `iced_baseview` and wgpu.
Baseview attaches the editor to the host-owned native parent instead of starting a separate application window.
Iced supplies retained controls and event handling, while plugin code owns parameter gestures and state updates.
The production shader is a separate WGSL source embedded at build time.

wgpu supplies one rendering path over Metal on macOS and Direct3D 12 on Windows.
This avoids maintaining separate first-party Metal and Direct3D renderers.
The GPU path handles the six-tier spatial spectrogram, persistence, particles, bloom, and solid contours.
Audio processing and ordinary controls do not depend on successful rendering.

The editor uses a plugin-owned executor and cancellable workers.
Native removal joins editor work before releasing native children where the host lifecycle permits it.
This ownership does not by itself prove reclamation of every upstream widget resource or safe module unloading.

GPUI was evaluated as an editor option but is not part of the implementation.
Its application-oriented startup and window APIs do not provide the host-parent editor boundary needed here.

### DSP and realtime ownership

The DSP uses RealFFT for real convolution transforms and RustFFT for complex analysis transforms.
Plans, scratch, FIR history, and queues are prepared outside realtime callbacks.
The `rtrb` single-producer/single-consumer queue carries bounded analyzer input and drops display work on overflow.

Four complementary linear-phase bands use 256-sample overlap-save partitions.
Each band has Mid, Side, Width, Recover, Pan, and Solo controls.
Output trim, latency-aligned Delta, bypass, and final Mono processing are separate stages.
At settled neutral settings, output is the delayed input copied bit-for-bit.

Crossover kernels are designed on a worker and exchanged at partition boundaries.
Old kernels are retired away from the processing callback.
Offline processing uses synchronous kernel design for deterministic rendering and is explicitly non-realtime.
At 48 kHz, reported latency is 4352 samples and the conservative tail beyond compensation is 4096 samples.

The callback admission boundary uses safe APIs from `AtomicRefCell` and `SyncWrapper`.
Overlapping access is rejected rather than waiting for the processing callback.
The callback does not allocate, lock, log, perform GPU work, or destroy retired kernel allocations.
See the [architecture](../ARCHITECTURE.md) for signal flow, ownership, and detailed contracts.

### State and user data

Rust host state and presets use independent version-1 JSON schemas.
They do not import C++ XML or promise compatibility with prototype Rust state or normalized automation.
The detached state codec validates the full document before publishing values.
Invalid or unsupported state is rejected without partially changing the live instance.

Sound snapshots, A/B comparisons, preset origins, editor layout,
display preferences, and logical window size are distinct persisted data.
Solo, Delta, Bypass, and local undo history are not sound-preset data.
The per-user Rust preset library is separate from the C++ preset library and host project state.

Parameter IDs and current plain-unit behavior are stable within the Rust implementation.
Crossovers use a logarithmic `f64` mapping.
Older RS normalized automation is not a compatibility promise.

### Licensing and distribution

The root workspace declares GPLv3 for repository code.
Permissively licensed framework or binding crates do not change
that license or grant proprietary distribution rights for first-party code.
The selected VST3, Clack, baseview, Iced, and wgpu dependencies have permissive licenses,
but the resolved dependency graph and enabled features still require auditing.

The CI license audit uses `cargo-deny` against the current graph.
That audit is a dependency-policy check, not legal advice, copyright clearance, or a relicensing decision.
Commercial distribution must meet the applicable source, notice, ownership, signing, and platform requirements.
Local macOS ad-hoc signing is not Developer ID signing or notarization.

## Platform and format status

### macOS

The VST3 and CLAP packages build and pass their scoped package tests.
The tested VST3 passes the repository-pinned pluginval at strictness 10.
Packaged C++/Rust parity runs against independent plain-unit expectations.
Native CLAP editor and AU editor checks run in macOS CI.
VST3 and CLAP have required-clean RealtimeSanitizer scenarios, with detector controls checked separately.
Miri covers bounded controller, state, and callback-admission contracts.

AUv2 is experimental and does not inherit VST3 or direct CLAP qualification.
It statically hosts the Rust CLAP implementation through the pinned upstream clap-wrapper.
Its separate component identity is `aufx / Egir / Esgr`.
Strict AU validation is performed on macOS, but several wrapper and SDK limitations remain open:

- The pinned wrapper does not forward host offline mode to `clap.render`.
- State save and restore require initialized wrapper state, and restore does not propagate the CLAP loader result.
- Render admission can return silence under contention, while parameter ingress and reset can spin.
- AU RealtimeSanitizer diagnostics report first-use allocation in clap-wrapper and reset-entry locking in AudioUnitSDK.

The known AU findings are reported as `BLOCKED` diagnostics, not clean callback evidence.
AU offline behavior, contention, and broader host compatibility remain unqualified.

Native macOS accessibility qualification, complete upstream widget-cache reclamation,
renderer device-loss recovery, and arbitrary module unloading also remain open.

### Windows

Debug and Release VST3 and CLAP packages pass their scoped tests and the pinned pluginval checks.
Packaged parity passes against the C++ implementation under the existing independent residual limits.
Bounded native VST3 and CLAP editor lifecycle cases have passed in both build profiles.
These cases exercise creation, resizing, close/reopen, callback-time removal, and balanced parameter gestures.
Initial editor geometry has been checked against the active parent DPI.

These results do not establish broad Windows GUI qualification:

- Full keyboard, text, focus, and control-input coverage is incomplete.
- Genuine transitions between monitors with different DPI settings have not been qualified.
- Actual AudioBatch and AudioBatch-rs GUI workflows and physical playback remain unqualified.
- Dedicated GPU resource tests do not establish complete device-loss recovery or physical presentation behavior.

The current surface handling can reconfigure an outdated surface and recreate a lost surface.
It does not recreate the GPU device, and out-of-memory handling is not a recoverable plugin-owned path.
Windows CLAP custom-editor destruction and broader native interaction remain subject to their host callback limits.

### Open lifecycle and qualification gates

Repeated editor close/reopen and bounded callback-removal tests are not proof of complete resource reclamation.
Upstream Iced widget-cache cleanup remains incomplete.
Invalid or reentrant final release retains UI resources as leak containment.
The plugin has not established format-compliant unload and reload safety for its native module.

Device-loss recovery must be handled at the owner of the compositor and wgpu device.
Testing plugin-owned GPU history alone cannot establish recovery at that layer.
Accessibility, GUI workflows, and physical audio continuity need native host checks beyond unit tests and CLI parity.
Production AudioBatch GUI workflows and physical playback have not been qualified on either platform.

The C++ plugin remains the supported release implementation until these independent gates are closed.
This status does not imply that each open gate blocks ordinary development builds or bounded tests.

## Verification approach

The ImageControl CI workflow runs C++ Debug and Release builds and tests on macOS and Windows.
It also validates the pinned VST3 with pluginval, validates the Audio Unit on macOS,
and runs clang-tidy, AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer,
RealtimeSanitizer, and coverage for the C++ implementation.

The Rust CI jobs format, lint, build, and test Debug and Release packages on macOS and Windows.
They validate VST3 with pinned pluginval strictness 10, exercise CLAP host scenarios,
check packaged C++/Rust parity, run Miri and RealtimeSanitizer, and audit licenses with cargo-deny.
macOS CI also exercises native CLAP and AU editors and validates the installed Audio Unit.
CI runs no benchmarks. They are available for local measurement only.

Run the Rust scripts from `image-control-plugin/rust/`:

```shell
./format.sh --check
./lint.sh --check
./build.sh --debug --test --validate
./build.sh --test --validate
./test.sh
./test.sh --miri
./test.sh --licenses
./rtsan.sh
```

The build scripts package the supported native formats without installation.
The test script requires the corresponding build-tree bundle.
RealtimeSanitizer runs require macOS, nightly Rust, uv, jq, and a matching Homebrew LLVM runtime.
They check exercised callback paths, not audio deadlines or arbitrary host behavior.
AU diagnostic mode verifies known dependency reports as `BLOCKED`, not clean callbacks.

### Cross-implementation and performance evidence

The separate `ImageControlParity` host loads explicit C++ and Rust build-tree VST3 bundles.
Its deterministic 264-case matrix covers formats, sample rates, block sizes, controls, automation,
latency, and full tail drains. The test compares outputs against independent expectations.
Peak residual is bounded by `1e-5` full scale and RMS residual by `1e-6` full scale.
Exact neutral and silent-Delta expectations remain exact.
These limits apply to the tested matrix, not every signal or host.

DSP Criterion targets, the packaged-processing runner, and GPU rendering benchmarks remain available locally.
Use Release builds and matched workloads when comparing performance.
DSP timing, offscreen GPU completion, and complete-process timing measure different work.
None establishes a realtime deadline, physical display cadence, or performance improvement without a baseline.

## Primary sources

Project behavior and verification:

- [Rust architecture](../ARCHITECTURE.md)
- [Rust user guide](../README.md)
- [Plugin agent instructions](../../AGENTS.md)
- [ImageControl CI workflow](../../../.github/workflows/image-control.yml)
- [Root Cargo workspace and dependency versions](../../../Cargo.toml)
- [AU wrapper build](../au/CMakeLists.txt)
- [Packaged parity runner](../parity.sh)

Core upstream projects and licenses:

- [VST3 Rust bindings](https://github.com/coupler-rs/vst3-rs)
- [Steinberg VST SDK 3.8.0 license](https://github.com/steinbergmedia/vst3sdk/blob/v3.8.0_build_66/LICENSE.txt)
- [Clack](https://github.com/prokopyl/clack)
- [iced_baseview](https://github.com/Esgrove/iced_baseview)
- [Iced](https://github.com/iced-rs/iced)
- [wgpu](https://github.com/gfx-rs/wgpu)
- [RealFFT](https://github.com/HEnquist/realfft)
- [RustFFT](https://github.com/ejmahler/RustFFT)
- [rtrb](https://github.com/mgeier/rtrb)
- [clap-wrapper](https://github.com/free-audio/clap-wrapper)
- [Apple AudioUnitSDK](https://github.com/apple/AudioUnitSDK)
