# Rust ImageControl evaluation

Source inspection date: **12 November 2026**.
This is a feasibility and dependency evaluation for macOS and Windows, not a production rewrite approval.
The C++ plugin remains the supported implementation.
The Rust experiment includes four-band FIR processing and a native GPU editor, but not C++ project-state compatibility.
C++ is the feature, close user-experience, and audio reference, not an exact internal implementation blueprint.
Compatibility with older RS state or normalized automation is not required.
No framework, graphics backend, or permissive license by itself establishes production readiness.

The dated parity-completion and benchmark results retained below describe earlier reviewed artifacts.
The Rust-native cleanup checks are recorded separately and do not imply a performance improvement.

### Windows Native Probe: 12 November 2026

The Windows `editor_reentry` driver supports `--case removal-retry`, `--case nested-dispatch`,
and `--case synchronous-removal`.
All cases load an explicit build-tree VST3 bundle and fail after a 30-second watchdog deadline.
The inspected baseline module SHA-256 is `001c7b5db6d0069f4ed50bd3516ee39ab3a4c8c86b0bd18c2ec59a69543dc050`.
This is a preserved as-found binary, not a verified rebuild of the recorded source revision.

With the host's Win32 message drain bounded by its timeout, the Release removal/retry probe passed three cycles.
Each cycle delivered native pointer input, read back a changed Output value, balanced begin/end gestures,
rejected removal twice per parameter callback without losing the child,
then successfully removed it outside the callback and verified native child retirement.
The host-core regression suite passed 13 tests, including a queued-message budget check.
This does not establish a bound on an individual native window procedure.

Nested dispatch aborted in the former `iced_baseview` revision `c7b4663`,
because `poll_runtime` mutably borrowed an already-borrowed runtime.
The published fork repair `f9d3384c50e95d78fbfe0a1b8e002c1f1a314e2d` defers nested polling,
preserves queued events, and avoids draining native commands inside an active runtime borrow.
The workspace now pins that revision.
Its eight library tests pass, including recursive polling followed by later progress.
The repaired packaged VST3 passed three nested-dispatch cycles and subsequently three synchronous-removal cycles.
Each synchronous cycle retired the HWND inside the parameter callback and balanced its gesture.
The strengthened driver subsequently passed three cycles of each case against both Debug and Release packages.
It retains the original child HWND for retirement assertions and stops pointer delivery after child detachment.
The focused callback-removal and geometry-flush ownership regressions also pass on Windows.

Windows CLAP advertises its custom GUI with the editor feature enabled.
The Windows `clap_editor` driver passed three ordinary create/show/hide/resize/destroy/reopen cycles,
checking actual native geometry, pending-work cancellation, and immediate HWND destruction.
Three subsequent active `request_flush` callback-destruction cycles passed after allowing native input to settle.
Each verified native child retirement and independently flushed balanced begin/end gestures.
Later repetitions passed ordinary lifecycle checks but stopped before pointer delivery:
the strict native hit-test found a covering `MozillaWindowClass`, despite visible and enabled plugin windows.
Fixed placement and per-monitor DPI awareness did not remove that obstruction.
Those stopped runs are not successful callback tests or established teardown failures.
The native driver does not send input through a foreign window or suppress the failure.
Subsequent unobstructed runs passed all three ordinary
and all three callback-destruction cycles against each of the Debug and Release packages,
with strict hit-testing unchanged.
Their raw logs are retained in `rust/target/benchmark/windows-host-integration/`, relative to the plugin directory.
This establishes repeatability for the exercised native cases, not the broader input and lifecycle matrix.

Both Debug and Release packaged builds passed 302 plugin library tests and format contracts.
The full native test script passed, including its editor lifecycle case.
Pluginval passed strictness 5 for both profiles and strictness 10 for Release.
Independent C++/Rust packaged parity passed all 264 cases without changing tolerances.
Shared VST3 hosting passed 76 unit tests and its integration test.
The Windows callback-final-release regression verifies successful synchronous removal without retained UI owners
after the host callback returns.
These are bounded package and ownership results, not complete native GUI qualification.

The subsequent editor benchmark smoke exposed plugin-dependent DPI awareness:
C++ opened at 1250 by 955 physical pixels and 125% scale, while RS opened at 1000 by 764 and 100%.
The benchmark now establishes a per-monitor-aware thread context before loading either plugin.
That exposed a separate RS initialization defect: its child remained 1000 by 764 physical pixels at 125% scale,
despite a correct pre-attachment VST3 size of 1250 by 955.
The shared Windows attachment layer now resolves initial logical size against the live parent DPI
before native child creation, without scaling explicit physical host sizes again.
The corrected Release VST3 smoke observed exactly 1000 by 764 logical and 1250 by 955 physical pixels at 125%.
A native-parent regression verifies authoritative DPI and unchanged explicit physical sizes.
Both profiles were subsequently rebuilt with `--test --validate`, passing package contracts,
all 303 plugin library tests, and pluginval strictness 5 after this repair.
Strict lint and formatting also passed.
This verifies the exercised initial geometry, not genuine multi-monitor DPI transitions.
Initial callback repeats stopped before pointer delivery after the desktop locked:
CLAP identified the covering `LockScreenBackstopFrame` window, and no input bypass was attempted.
Subsequent unobstructed post-repair repetitions passed in both Debug and Release:
three VST3 synchronous-removal cycles, three nested-dispatch cycles,
three ordinary CLAP lifecycle cycles, and three active callback-destruction cycles per profile.
The original native child retired and gesture starts and ends balanced in every exercised callback case.
CLAP observed its 1000 by 764 physical child at DPI 120.
These probes do not establish another DPI configuration.
Logs and tested-module SHA-256 manifests are retained in `rust/target/benchmark/windows-post-dpi-U2YZjUl4/`,
relative to the plugin directory.
The earlier unmatched editor smokes remain diagnostics, not performance comparisons.
The rebuilt Release `editor_compare` host also completed separate five-second runs with two RS editors
and with C++ and RS together, processing both instances through concurrent paced callbacks.
Their exit-success logs and input hashes are retained beside the post-DPI logs.
This is a basic two-editor lifecycle smoke, not input, physical playback, or arbitrary-unload qualification.

### Windows Application Hosts: 12 November 2026

Actual C++ AudioBatch
and AudioBatch-rs CLIs processed only temporary copies with separate absolute `AUDIOBATCH_DATA_ROOT` directories
and explicit build-tree plugin paths.
JUCE captured native XML presets.
AudioBatch-rs captured native JSON through its production host and preset store.
No opaque state was converted between host schemas or plugin implementations.
Both applications loaded C++ ImageControl and ImageControl RS VST3.
AudioBatch-rs also loaded RS CLAP.

Twenty renders covered neutral and active states with two repetitions on each of these five routes.
Neutral decoded PCM16 and repeated outputs were bit-exact.
The C++ application's matched crossover/Mid/Side/Pan presets produced matching 546-frame trailing output
and at most one PCM16-step cross-implementation residual.
The Rust application's separate -6 dB output-trim presets matched independently quantized expectations within one PCM16 step.
RS VST3 and CLAP outputs were bit-identical.
Five additional tagged-FLAC renders preserved title, artist, and album.
Processed Serato fixtures correctly discarded stale Overview tags.
These exported-file checks do not replace the independent floating-point parity or latency/tail tests.

Reports, captured native presets, decoded PCM hashes, executable/plugin/fixture SHA-256 inventory,
and machine metadata are retained locally under
`rust/target/benchmark/windows-host-integration/` relative to the plugin directory.
The application executables are identified as-found builds, not a fresh rebuild of every application source.
No CLI processing duration is presented as DSP-only time.
Actual GUI chain manipulation, preview/playback removal, multi-instance editing,
and genuine DPI transitions remain unqualified.

### Windows Matched Measurements: 12 November 2026

Hardware metadata identifies Windows 11 Pro build 26300, Ryzen 9 7950X,
and GeForce RTX 4090 with driver `32.0.16.2101`.
The High performance power scheme was recorded after the captures, not monitored throughout each sample.
Renderer reports identify DX12 for Rust and OpenGL 4.1/NVIDIA 621.01 for C++.

The same optimized native VST3 host measured C++, preserved RS baseline, and candidate RS plugins.
Ten processing scenarios passed bounded smoke at 44.1/48/96/192 kHz for both implementations.
The matched capture contains 300 runs:
five counterbalanced repetitions of neutral/recover/delta/bypass at 48 kHz,
960000 measured frames, and fixed 64/128/256/512 or irregular 1/17/257/511 blocks.
Each run used the host's built-in audio warmup, not an additional whole-process warmup.
The recorded helper metadata value `warmup=1` must not be interpreted as a separate process invocation.
Every five-sample group has one deterministic output checksum.
Checksums alone do not prove correctness.

At 256-frame blocks, total warmed callback milliseconds were:

| Case | C++ Median (Range) | RS Baseline Median (Range) | RS Candidate Median (Range) |
| --- | --- | --- | --- |
| Neutral | 37.005 (36.747-37.369) | 19.547 (19.097-20.503) | 19.145 (19.110-19.335) |
| Recover | 547.671 (542.730-552.014) | 89.719 (88.936-92.006) | 90.862 (90.187-96.793) |
| Delta | 547.663 (543.907-554.648) | 90.736 (90.393-92.426) | 92.080 (89.790-97.065) |
| Bypass | 37.022 (36.765-37.649) | 19.152 (19.083-19.634) | 19.111 (19.093-19.225) |

Local artifacts: `rust/target/benchmark/windows-processing-matched-5UivmfjK/`, relative to the plugin directory.
The directory retains raw samples, exact bundle/host hashes, and all block schedules.
These observations do not establish an RS before/after improvement: sample ranges overlap.
The preserved baseline is an as-found artifact, not a proven rebuild of its recorded source revision.

The shared native host now also separates module loading from instance preparation and warmed callbacks.
Sixty fresh-process captures cover C++, preserved RS baseline, and current RS at all four rates,
with five counterbalanced repetitions, 256-frame blocks, and 4096 measured neutral frames.
Every run passed its independent delayed-input bit check, with deterministic outputs in each five-run group.
Instance preparation includes construction, bus configuration, processing setup, and activation.
It excludes module loading, sound configuration, input generation, warmup, and teardown.
These are complete host-preparation boundaries, not exclusive kernel-design or analysis timings.
Filesystem caches were not controlled, and there was no additional process warmup.

| Rate (kHz) | C++ Preparation Median (ms) | RS Baseline (ms) | RS Current (ms) |
| --- | --- | --- | --- |
| 44.1 | 8.019 | 3.306 | 3.353 |
| 48 | 8.002 | 3.381 | 3.328 |
| 96 | 12.755 | 6.082 | 6.121 |
| 192 | 21.553 | 11.572 | 11.656 |

At 48 kHz, preparation ranges were 7.855-8.453, 3.271-3.595, and 3.295-3.459 ms respectively.
Module-load medians were separately 5.924, 3.365, and 3.329 ms.
The overlapping RS preparation ranges establish no before/after improvement.
Raw samples, exact bundle/host identities,
and min/median/max summaries are retained in `rust/target/benchmark/windows-preparation-matched-zBRCMkVL/`,
relative to the plugin directory.
The seven focused evidence contracts pass, including finite preparation timings and unchanged non-timing determinism.
The complete 16-case benchmark-script suite passes on Windows.
Its shell fixtures use valid POSIX shebangs, LF newlines, and wrappers around Git's original executables.
Native path conversion preserves invocation-relative targets and quoted paths.
Production argument validation precedes Windows path-tool queries, with failure-propagation assertions retained.

Separate candidate-only Criterion runs used 100 samples after three seconds of warmup at each rate.
At 44.1/48/96/192 kHz, preparation point estimates were 2.671/2.548/5.098/10.332 milliseconds.
Warmed analysis of 4096 frames measured 0.903/0.879/0.880/0.921 milliseconds at those rates.
Preparation includes allocation and kernel worker creation, with destruction excluded.
The saved `windows-20261112` Criterion data is in the root workspace's `rust/target/criterion/`.
The invocation log is `rust/target/benchmark/windows-setup-analysis-20261112.log`, relative to the plugin directory.
These are not matched C++ or baseline measurements and establish no before/after improvement.

The paired renderer capture contains 40 validated runs, five repetitions per implementation and case,
using identical workload bytes at 1280 by 720 physical pixels, scale 1, particles/solid, and 4096/16384 particles.
Each run has 30 warmup and 120 measured frames.
For 16384 particles, median mean CPU encoding was 129.309 microseconds/frame for C++ and 234.984 for Rust.
Median completed-batch time was 49.871 milliseconds for C++ and 29.469 for Rust.
For 4096 particles, the corresponding batch medians were 22.775 and 22.773 milliseconds.
C++ uses RGBA32F accumulation on Windows.
Rust uses Rgba16Float, with different output formats as well.
C++ GPU timestamps were unavailable and remain null, so these batch/CPU values are not GPU pass-time comparisons.
No physical presentation or display FPS is inferred from this headless measurement.
Raw reports and validated summaries are in `rust/target/benchmark/windows-render-matched-20261112T091626Z/`.
The C++ benchmark retires pending warmup query bookkeeping before resetting measured frame identities.
The exact duplicate-index regression and subsequent paired matrix passed strict report validation.

The native editor capture contains 60 reports: five counterbalanced repetitions of C++, preserved RS baseline,
and current RS for default, solid, continuous persistence/contrast, and maximized layouts.
Each fresh process has two seconds of warmup and five measured seconds with concurrent paced stereo callbacks.
The host explicitly requests constrained physical size after attachment, without changing the preserved baseline.
All native observations were visible, not minimized, and exactly 1250 by 955 physical pixels at 125% scale,
or 1000 by 764 logical pixels.
RS baseline/current renderer plots matched at 1190 by 339 pixels, or 1226 by 371 in maximized layout.
The C++ telemetry does not expose equivalent plot dimensions or native encoding endpoints.

| Case | C++ Counter Observations/s Median (Range) | RS Baseline Encodings/s Median (Range) | RS Current Encodings/s Median (Range) |
| --- | --- | --- | --- |
| Default | 139.24 (138.87-140.84) | 39.6 (39.2-40.2) | 40.0 (39.4-40.2) |
| Solid | 139.60 (139.03-141.00) | 39.6 (39.2-39.6) | 39.6 (39.2-40.0) |
| Persistence/contrast | 139.24 (139.01-141.21) | 39.2 (38.6-40.0) | 39.4 (39.2-40.0) |
| Maximized | 139.85 (139.03-141.23) | 39.2 (38.8-40.0) | 39.4 (38.6-40.0) |

These are different timestamp boundaries, not measured display FPS or directly equivalent encoding rates.
Windows occlusion, GPU completion, and physical scanout remain unknown.
The median of per-run RS encoding p50 values was 8.0-9.1 microseconds across these cases.
The median callback p50 was 55.7-60.4 microseconds for RS and 209.3-299.3 for C++.
Callback-lateness p95 medians were separately 1.69-1.85 milliseconds.
These concurrent editor/processing observations are not isolated DSP costs or audio-device deadline guarantees.
All RS before/after rate ranges overlap.
No improvement is established.
Raw reports, exact module and host hashes,
the uncommitted host-source patch, protocol, and spreads are retained in
`rust/target/benchmark/windows-editor-matched-L7j2PKUm/`, relative to the plugin directory.
Input hashes remained unchanged through capture, and the complete artifact checksum inventory verifies.

The workflow includes bounded packaged-processing workloads
and an opt-in interactive Windows native job selected with `windows_native_runner`.
That job validates four native GPU reports with the reproduced 30-warmup/120-measured-frame query workload
before running Debug and Release editor probes.
The exact GPU invocation passed locally after rebuilding the touched C++ targets.
Native failures retain their raw logs.
The workflow schema validates locally, but the new jobs have not been qualified remotely.
Remaining gates include broader native input and DPI, actual application GUI workflows,
isolated C++/baseline analysis measurements, renderer failure recovery,
upstream widget-cache reclamation, and arbitrary module unloading.

## Recommendation

- The primary experiment uses **our narrow VST3 layer** on `vst3 0.3.0` and a Clack `0.2.0` CLAP adapter,
  with direct `iced_baseview 0.6.0` native attachment and Iced's wgpu renderer.
  macOS AUv2 delivery uses a separately identified, self-contained upstream clap-wrapper shell.
  The own-wrapper VST3 remains unchanged in identity and does not run through CLAP.
  Its name is **ImageControl RS**, with its own class and bundle identities.
  Iced keeps application/widget state and routes messages. It is not egui's immediate-mode control model.
  **egui is excluded from the implementation choice**, as requested.
- The GPU spectrogram uses custom wgpu shader primitives for instanced particles, half-float persistence,
  bloom, and composition. The selected Iced graph has no `iced_tiny_skia` software backend and no egui dependency.
  macOS selects Metal.
  Bounded Windows DX12 native/headless runs are recorded above,
  without complete device-loss or display-presentation qualification.
- The own VST3 and Clack CLAP adapters share one Rust DSP/state/Iced/wgpu implementation.
  The raw VST3/patched-GPUI reference, its vendor patches, and its separate workspace have been removed.
  The primary artifact links no nice-plug, nice-plug-iced, or patched GPUI crate.
  No C++ plugin is replaced, and the Rust plugin does not claim JUCE state compatibility.
  The GPUI evidence below is historical, not a second build or installation route.
- Nice-plug was replaced after its packaged cold callback exposed GUI wakeup locks and a state-channel mutex/allocation.
  The replacement samples parameter atomics, transfers kernel ownership through bounded SPSC queues,
  and polls display work from the UI without audio-thread GUI notifications.
  Historical reviewed packaged artifacts passed RTSan before the current cleanup.
  Coverage included cold automation, reset, concurrent state restore, and two-editor lifecycle,
  with positive allocation and mutex detector controls.
  This is bounded evidence, not proof of every callback or deadline.
- Preserve the intended features, close user experience, audio behavior, and realtime contracts,
  while choosing idiomatic Rust ownership and state rather than mirroring C++ internals.
  Older RS persistence and normalized automation are outside the compatibility scope.
  Begin with RustFFT/RealFFT and preallocated scratch, then compare against native FFT performance before optimizing.
  Exact neutral transparency must remain bit-exact. General processed-output equality requires numerical tests,
  not an assumption that a language or FFT substitution produces identical bits.
- Retain an explicit macOS Audio Unit route.
  Rust AU delivery exists through wrappers, including `clap-wrapper`, and through a native Apple SDK bridge.
  None is a drop-in replacement for the present JUCE AU wrapper or its saved-project identity.

## Scope, evidence, and version baseline

Repository sources inspected include [the architecture](../ARCHITECTURE.md),
[parameter definitions](../../src/Parameters.h), [processing state](../../src/ProcessingState.cpp),
[preset storage](../../src/UserPresets.cpp), and [Rust workspace dependencies](../../../Cargo.toml).
The application workspace currently declares Rust 1.99, GPUI Kit 0.9.1, GPUI snapshots 0.3.12,
`vst3` 0.3.0, `realfft` 3.5.1, and `rtrb` 0.4.1.
Its lockfile resolves RustFFT 6.4.1 and wgpu 30.0.1.
These are inspected repository versions, not a request to change the application dependencies.

The following are reproducible research baselines, not assertions that all combinations have been built:

- `vst3` **0.3.0**, MIT OR Apache-2.0, with pre-generated bindings from VST SDK **3.8.0**.
  The inspected binding repository snapshot is `b2a45f2d40e602eff5a224b41613735388868590`.
  Its pluginterfaces pin `31d6eeba6daaa3e2a8bfbe3e7a90ca0b7fbfbc1c` identifies SDK 3.8.0.
  See [binding source and licenses][vst3] and [SDK pin][sdk-pin].
- nice-plug **0.4.2**, ISC, with `nice-plug-egui` **0.5.1**.
  That adapter uses egui **0.36.1**, `egui-baseview` **0.7.1**, and baseview **0.3.4**.
  Its wgpu path accepts wgpu **30**, while OpenGL is its default feature.
  A GPU experiment must select the intended features deliberately.
  See [release manifest][nice-manifest], [editor manifest][nice-egui], and [adapter dependency metadata][egui-baseview].
- baseview **0.4.2** is a separate current candidate,
  tagged at `4fe59b368f50573127fa1285767208781bc03cdf`, MIT OR Apache-2.0.
  Do not combine its API with the 0.3.4 editor adapter by assuming source compatibility.
- wgpu **30.0.1**, egui **0.36.1** for the above adapter, RealFFT **3.5.1**, RustFFT **6.4.1**,
  and rtrb **0.4.1** are suitable pinned comparison points.
  Upstream egui **0.37.0** is newer, but is not the inspected nice-plug adapter's dependency generation.
- NIH-plug snapshot `de421011f41a6d10fc8c7a6084e4f4dee0143683` is in maintenance mode.
  Its VST3 dependency resolves from a branch,
  inspected at `b3ff4d775940f5b476b9d1cca02a90e07e1922a2`, and declares GPLv3.
  Branch names are not sufficient production pins.
- Clack snapshot `27ca28391b57a61bad4bb5099288a7bf9247350d` declares `clack-plugin` **0.2.0**.
  `clap-wrapper` **v0.17.0** is commit `806bba0a5f792e3966b99c1561ab9952d3316d7c`.
  Conformal snapshot `5bea4ca9433455bddeeb426ba0679e0f63dd59b8` declares its VST wrapper **0.6.6**.
- GPUI Kit **0.9.1** uses exact `gpui-pre` and platform snapshots **0.3.12**.
  The published GPUI snapshot identifies Zed commit `d2a4fb6`.
  Vizia's inspected current snapshot is `12aa7a980663baab783051b98ec39ff3d001bc06`, declaring **0.4.0**.
  A production selection needs a committed lockfile, checksums, enabled-feature inventory, and license review.

## 1. Plugin framework and format boundary

### Direct VST3 bindings

[`vst3`][vst3] is a thin, unsafe API binding with COM construction and ownership helpers,
not a complete plugin framework.
Its crate license is MIT OR Apache-2.0.
SDK 3.8's [pinned license file][sdk-license] is MIT,
and its pre-generated bindings need neither SDK code generation nor libclang at build time.
Use the actual SDK revision's license when older online licensing pages describe superseded terms.
This avoids the historical VST3 SDK licensing trap, but does not eliminate ABI or host-lifecycle work.

Advantages are exact control of class IDs, numerical parameter IDs, state streams, processing mode, and editor lifetime.
AudioBatch already has a raw-binding test plugin and host that can inform the spike.
Costs include implementing processor/controller communication, refcounting, stream bounds, gestures,
latency/tail notification, bus negotiation, and exception-free FFI failure paths.
Do not let Rust panics unwind through a COM or C ABI boundary.
A panic strategy must be deliberate. Aborting terminates the host, while catching a panic is not DSP recovery.

**Recommendation:** use this for the bounded spike, retaining explicit control of RS identity and host lifecycle.
Do not grow the spike into a second framework before measuring whether editor attachment works.

### nice-plug and NIH-plug

[nice-plug][nice] provides parameters, smoothing, state migrations, background tasks, export macros,
and plugin bundling with considerably less wrapper code.
The **0.4.2** manifest uses the optional **`vst3` 0.3.0** crate, not NIH-plug's GPL `vst3-sys` dependency.
The framework and inspected egui adapter are ISC-licensed.
Its experimental status and limited documented macOS testing are material maintenance risks.
Built-in JSON or compressed state does not establish compatibility with JUCE binary XML.

[NIH-plug][nih] itself is ISC-licensed, but that is not the license of every selected dependency.
The inspected default `vst3` feature enables `vst3-sys` from the author's fork,
whose [manifest declares GPLv3][nih-vst3-sys].
Its own README explicitly warns that exported VST3 plugins must comply with GPLv3.
Steinberg relicensing SDK 3.8 to MIT does **not** automatically relicense that independent Rust binding.
A CLAP-only build can omit this VST3 dependency, but its complete resolved dependency graph still needs review.

**Recommendation:** evaluate nice-plug ahead of original NIH-plug for a new implementation.
Original NIH-plug remains technically viable for GPL-compliant distribution,
but it is not a verified permissive, proprietary-distribution VST3 route as inspected.

### CLAP-first and Conformal alternatives

[Clack][clack] provides low-level, safe CLAP wrappers under MIT OR Apache-2.0.
Together with the MIT-licensed [`clap-wrapper`][clap-wrapper],
it offers a credible CLAP-core route to VST3 and macOS AUv2, with AUv3 support also documented.
The wrapper introduces C++/CMake and a second lifecycle/parameter/state mapping layer.
It is therefore not an all-Rust delivery stack, and the additional format is not a requirement of this evaluation.
Use it if AU delivery is a stronger driver than minimizing wrapper layers.

[Conformal][conformal] is ISC-licensed, and its inspected VST3 wrapper uses `vst3` 0.3.0.
It offers Rust DSP with a TypeScript/webview editor, using `wry` in its UI crate.
That can be productive for web-oriented teams, but changes the editor architecture,
introduces webview deployment and IPC considerations, and does not demonstrate a GPUI or native GPU editor path.
The inspected workspace contains a VST3 wrapper, not an AU export implementation.
Do not generalize that observation into a claim that Rust cannot ship Audio Units.

**Recommendation:** keep Clack plus clap-wrapper as the format-expansion alternative.
Conformal is a third-party framework option, not the best match for this native GPU spike.

## 2. macOS Audio Unit delivery

There are three credible choices, each needing separate compatibility and host validation:

- **Keep a small JUCE AU shell around a Rust DSP library.**
  This preserves a known host wrapper and can reduce migration risk while DSP is evaluated separately.
  It retains C++ integration and JUCE licensing obligations, so it does not meet a complete JUCE-removal goal.
  A Rust static library behind a narrow C ABI needs explicit buffer ownership, initialization, and panic containment.
- **Use CLAP plus clap-wrapper.**
  Its pinned README documents AUv2 and AUv3 on macOS.
  AUv2 uses Apple's [AudioUnitSDK][au-sdk], licensed Apache-2.0.
  AUv3 has distinct extension packaging and signing requirements and is not interchangeable with the current AUv2.
  Verify latency, bypass, parameter address mapping, editor parenting, and saved-state translation in the wrapper.
- **Write a native Apple AU bridge.**
  Apple's SDK and system frameworks make a Rust DSP with a C++/Objective-C boundary possible.
  This offers control but has the highest AU-specific implementation and maintenance burden.
  AudioToolbox bindings alone do not implement a production AU component or its editor.

**Recommendation:** retain AUv2 as an explicit production requirement.
Experimental AU delivery does not establish complete replacement qualification.
If removing JUCE is mandatory, investigate clap-wrapper AUv2 before commissioning a custom AU wrapper.
Retain the independent RS `aufx / Egir / Esgr` identity, not C++ `Egic` or its project-state contract.

## 3. Native editor embedding and controls

### baseview with Iced

[baseview][baseview] is specifically designed for audio-plugin UI windowing,
with native-parent attachment rather than an application-owned event loop as the central use case.
It is MIT OR Apache-2.0 and exposes window parenting, sizing, visibility, and lifecycle operations.
It does not supply a renderer, control toolkit, or parameter attachment policy by itself.

[nice-plug-iced 0.5.0](https://crates.io/crates/nice-plug-iced/0.5.0) is ISC-licensed,
with [iced_baseview 0.6.0](https://crates.io/crates/iced_baseview/0.6.0), Iced 0.14 components,
and the wgpu renderer. Iced's application state, message routing, and retained widget state fit the requested controls.
The selected features disable `tiny-skia`, and explicitly enable `iced_widget/wgpu` for custom shader primitives.
The plugin supplies sliders, buttons, checkboxes, and a custom GPU spectrogram without a standalone application loop.

**Recommendation:** retain Iced/wgpu, but embed `iced_baseview` directly through the narrow VST3 view.
The nice-plug adapter is historical reference only.
Its runtime and bundler are absent from the primary graph.
Keyboard focus, text entry, accessibility, popup ownership, IME behavior, and host shortcuts still need native testing.
The MIT OR Apache-2.0 [egui][egui] adapter was researched but is excluded because its immediate-mode model is unwanted.
Do not substitute it as a fallback, and do not assume a standalone winit application can take over the host's UI thread.

### GPUI Kit and GPUI

[GPUI Kit 0.9.1][gpui-kit-license] and the [inspected GPUI snapshot][gpui-license] declare Apache-2.0 licenses.
The Zed application's repository license is not a substitute for checking GPUI's separately licensed crates.
Likewise, do not infer that an Apache-licensed UI toolkit relicenses AudioBatch application code.

The inspected GPUI [`WindowOptions` and platform API][gpui-platform] describe application windows,
and do not expose a direct host-owned `NSView`/`HWND` parent constructor in that API.
GPUI Kit's normal application bootstrap runs an application event loop.
Obtaining a raw window handle from GPUI is not equivalent to importing a host's native parent as an embedded editor.
Its native rendering and useful AudioBatch controls make it interesting,
but do not establish that host event loops, multiple plugin instances, or module unloading are supported.

Risks include application-global initialization, host menu/delegate interactions, executor shutdown,
window-thread affinity, hidden-window redraw, native focus, and callbacks surviving module unload.
A top-level GPUI window visually placed over the host editor is not successful embedding.
A custom platform adapter or upstream embedding API could make this possible, but neither is assumed here.

**Historical conclusion:** the isolated macOS embedding experiment succeeded,
but did not establish Windows readiness or eliminate long-term patch-maintenance costs.
The patched GPUI route was rejected in favor of direct Iced/baseview attachment and has been removed.
Its results remain useful evidence, not an active implementation recommendation.

### Vizia

The inspected current [Vizia source][vizia] and the NIH-plug-pinned Vizia fork declare **MIT**, not LGPL.
The current Vizia manifest includes a baseview backend and uses Skia for rendering.
That does not imply every rendering dependency, bundled font, asset, or historical revision shares Vizia's license.
Audit the actual selected versions rather than describing either the entire stack as LGPL or entirely MIT.

The inspected [`vizia_plug` adapter][vizia-plug] is MIT,
but depends on `nice-plug-core` **0.1.4** and a pinned Vizia revision.
That is not the nice-plug **0.4.2** core generation evaluated above.
Its source also documents mutex use during parameter registry updates from host/audio paths,
which requires realtime review rather than accepting the adapter as automatically callback-safe.

**Recommendation:** keep Vizia as a retained/declarative-control alternative,
but resolve adapter generation and realtime behavior before comparing implementation effort with egui.

## 4. GPU backend and rendering architecture

- **wgpu, preferred first.**
  [wgpu 30.0.1][wgpu] is MIT OR Apache-2.0, with native Metal and Direct3D 12 backends.
  Use one WGSL rendering design across both platforms, with explicit capability checks.
  Preserve the current pipeline's persistence accumulation, Gaussian instances, bloom, and final composition.
  Verify float texture renderability and blending rather than assuming present OpenGL texture choices transfer directly.
  Query surface format, alpha mode, present mode, and usable limits for each adapter.
  Recreate surfaces on resize/loss and handle zero-size or hidden views without processing GPU frames.
  Keep surface handles valid until all work and resources using them are released.
- **Native Metal and Direct3D, targeted fallback.**
  [Metal][metal] and [Direct3D 12][d3d] give direct control of child-view layers, swap chains, pacing, and diagnostics.
  Rust binding choices include the MIT-licensed [`objc2` 0.6.5][objc2] ecosystem
  and Microsoft's MIT OR Apache-2.0 [`windows` bindings][windows-rs].
  Audit each selected framework binding crate separately.
  The bindings' open-source licenses do not replace Apple or Microsoft SDK terms.
  Two shader/backend implementations increase testing and maintenance,
  but may simplify a specific host-lifetime problem.
  Choose this route only for an integration blocker or a measured benefit, not an assumed zero-overhead advantage.
- **Retain OpenGL as a comparison baseline, not the preferred new dependency.**
  The current renderer is valuable for behavior and performance comparison.
  Reusing its conceptual passes is easier than mechanically translating context management or global GL state.
  A native/wgpu renderer removes dependence on JUCE's GL context, not the need for synchronization and lifecycle tests.

**Recommendation:** separate the renderer from controls and analysis, regardless of backend.
The audio callback publishes bounded data only.
GPU creation, shader compilation, uploads, waits, present, and destruction belong off the audio thread.
Renderer failure must leave audio processing and ordinary controls usable.
Measure UI-thread stalls and GPU frame times separately from DSP callback time.
Do not use queue completion waits in normal rendering solely to obtain timing numbers.

## 5. DSP and FFT

Three implementation choices deserve measurement:

- **RealFFT plus RustFFT, preferred portability baseline.**
  [RealFFT 3.5.1][realfft] is MIT and wraps RustFFT for real transforms.
  [RustFFT][rustfft] is MIT OR Apache-2.0 and provides SIMD complex transforms.
  Use RealFFT for real convolution transforms and RustFFT for the analyzer's packed stereo complex transform.
  Construct plans and all scratch/storage during preparation or on the kernel worker.
  RealFFT's ordinary `process` may allocate scratch.
  Its [`process_with_scratch`][realfft-scratch] exists specifically to reuse caller-provided scratch.
  Apply the same scrutiny to inverse and complex-transform calls.
- **Apple Accelerate/vDSP on macOS, measured alternative.**
  [vDSP][accelerate] is a native system framework accessed through an audited FFI wrapper.
  Its FFT packing, scaling, normalization, setup objects, and first-use behavior must be reconciled explicitly.
  Native availability does not make it a Windows implementation or prove allocation-free first callbacks.
  Keep the RustFFT path for Windows and compare actual convolution/analyzer sizes, not generic FFT headlines.
- **Keep the present C++ DSP behind a narrow ABI during evaluation.**
  This can isolate editor and plugin-wrapper risks from DSP translation.
  It retains JUCE and the corresponding licensing/build obligations.
  It is a migration technique, not evidence that a Rust-only DSP port is complete.

Preserve the four-band complementary FIR construction, quadrature recovery sign and antisymmetry,
20 ms ramps, partition-boundary kernel handoff, warm history, and arbitrary host-block adaptation.
The partition is 256 samples and convolution FFT is 512 points.
The half-length is 4096 samples through 48 kHz and scales with the existing rate rule.
At 48 kHz latency is 4352 samples, with the existing conservative tail contract.
Recovery is a finite quadrature waveform contribution, not power recovery or an automatic loudness law.

**Recommendation:** reproduce independent direct-convolution and analytical tests before optimizing.
Keep the exact delayed-input neutral shortcut and exact neutral Delta zeroing.
Do not demand bit equality for every non-neutral FFT result, but do not silently widen current tolerances either.
Record measured peak/RMS/null-error bounds for each changed numerical path and supported CPU architecture.
Check crossover and quadrature coefficients, DC/Nyquist endpoints, low-frequency response, ramps, tails,
phase-adversarial signals, offline determinism, and reset/startup behavior separately.
Benchmark matched release builds with the existing `ImageControlBenchmark` workloads as the baseline.

## 6. Realtime communication and ownership

Use two complementary primitives rather than one universal concurrent-state abstraction:

- **rtrb for bounded SPSC transport.**
  [rtrb 0.4.1][rtrb] is MIT OR Apache-2.0 and allocates fixed capacity at construction.
  Its push/pop operations are wait-free, provided the payload itself does no unexpected work.
  Use copyable stereo frames with source epochs for analyzer transport.
  Full queues drop visualization frames, never block audio or spin until space appears.
  Bound worker drain work so offline producers cannot starve shutdown.
- **Atomic scalar targets and preallocated kernel slots.**
  Represent float targets by audited atomic bit storage or an equivalently reviewed atomic-float wrapper.
  Publish coherent crossover requests and kernel-slot ownership with explicit acquire/release synchronization.
  Retain the existing two-slot handoff and delayed reuse acknowledgement.
  A sequence counter requires atomic payloads or another valid Rust memory-safety design,
  and callback-side retries must remain bounded under contention.
- **Deferred destruction rather than callback-owned smart-pointer replacement.**
  `Arc`, `Box`, `Vec`, channels, error payloads, and ring endpoints can allocate or deallocate during drop.
  A final `Arc` decrement may destroy an FFT plan or kernel on the callback even when the pointer swap is lock-free.
  Prefer slot indices or return retired ownership to a non-realtime consumer.
  A failed queue push returns its payload. Dropping that returned payload on audio can still free memory.
  Predefine an allocation-free overflow policy and capacity sufficient for the ownership protocol.

**Recommendation:** preallocate processing, history, FFT scratch, queue storage, and kernel slots.
Keep locks, logging, filesystem access, blocking wakeups, thread joins, and GPU operations off realtime callbacks.
Treat ordinary offline kernel design as an explicit non-realtime exception, not a suppression of realtime findings.
A Rust implementation must separately verify dependencies, host wrappers, first-use TLS,
and callbacks on fresh threads. Safe Rust alone provides no realtime guarantee.
RTSan clean scenarios and verified dependency blockers must remain distinct, and neither proves audio deadlines.

## 7. Parameters, saved state, editing, and presets

The C++ reference defines intended features and audio behavior, not an internal schema or normalization blueprint:

- Retain the RS plugin identities and stable parameter IDs, useful ranges/defaults, units, finite-value rejection,
  and balanced begin/perform/end gestures. Matching JUCE's class IDs or float rounding is not required.
  Crossovers use a true logarithmic `f64` mapping. Stepped controls snap in plain units.
  Older RS normalized automation is not a compatibility promise.
- Keep block-sampled automation and partition-boundary smoothing semantics.
  Test controller and processor synchronization with the editor closed.
- Keep sound settings distinct from audition controls and editor preferences.
  Presets include crossovers, Mid/Side, Width, Recover, Pan, output gain, and Mono.
  Solo, Delta, and Bypass remain live audition controls.
  Link remains editor gesture behavior, not host-automation coupling.
- Preserve chronological local history, nested gestures, A/B slot capture, provenance, and stale-preset handling.
  Undo history remains transient and host automation does not enter it.
  Host restores invalidate pending local transactions and do not synchronously wait for message dispatch.

**Recommendation:** use the independent RS JSON codec with schema 1 only.
Serialize `Snapshot`, `AnalysisTap`, and `DisplayPreferences` directly instead of duplicate mirrors or migration layers.
Older and unknown schemas reject transactionally, leaving all live values and restore identity untouched.
Use fixed defaults for absent current-schema fields and validate the complete detached document before publication.
Test malformed streams, length bounds, truncation, invalid values, and rejected schemas.
Test precise current-schema round trips independently.
Host state and user preset files remain separate contracts.
Neither a C++ XML importer nor compatibility with earlier RS projects is required.

## 8. What AudioBatch can reuse

There are three different meanings of reuse:

- **Existing crates as development/test dependencies.**
  [`vst3-host`](../../../rust/crates/vst3-host/Cargo.toml) can exercise processing and native editor callbacks.
  [`audiobatch-test-plugin`](../../../rust/crates/test-plugin/Cargo.toml) demonstrates raw COM factories
  and test bundling.
  [`test-fixtures`](../../../rust/crates/test-fixtures/Cargo.toml) provides fixture discovery and null-test support.
  These are useful test infrastructure, not a complete production plugin SDK.
  Do not link a VST3 host or AudioBatch's application GUI into the production effect merely to reuse helpers.
- **Concepts and narrow algorithms.**
  Playback's bounded callback admission, deferred disposal, fresh-thread RTSan cases,
  spectrogram scratch reuse, epoch-based transport, and native-window thread tokens are valuable references.
  The application's cached mel spectrogram is not ImageControl's six-tier spatial analyzer.
  Whole `audiobatch-core` reuse brings unrelated decoding, storage, cache, and filesystem dependencies.
  Prefer extracting a coherent shared capability only when actual second ownership justifies it.
- **Dependencies and assets.**
  Reuse established external crate versions and relevant compiler/test conventions where appropriate.
  Review embedded font and image licenses independently.
  Dependency familiarity is useful, but it does not prove a host-embedded lifecycle or suitable license for copied code.

**Recommendation:** reuse test infrastructure directly when appropriate,
and reuse realtime/analysis architecture as explicit designs rather than importing entire application crates.
Keep plugin identities and package naming independent of AudioBatch.

## 9. Licensing and commercial distribution

The repository's [top-level LICENSE](../../../LICENSE) is **GPL version 3**,
and the Rust crate manifests inherit `license-file = "LICENSE"` from the repository-root workspace metadata.
Moving or translating existing repository code into a Rust effect does not automatically make that code permissive.
Nor does putting it in a separate workspace remove copyright or distribution obligations.
The existing plugin also remains subject to its actual JUCE and third-party licensing.

- GPL allows commercial use and selling software. It is not a non-commercial license.
  Conveying covered binaries generally requires GPL-compliant licensing and corresponding source availability.
  Choosing permissively licensed dependencies alone does not create a proprietary distribution right for GPL code.
- A copyright holder can separately license code they wholly own, including previously GPL-released code.
  Existing recipients retain their GPL rights.
  Before a proprietary release, establish ownership and permissions for every copied contribution,
  translated implementation, vendored fragment, asset, and dependency.
  Do not assume repository ownership proves ownership of every contribution.
- MIT, ISC, and Apache-2.0 permit commercial proprietary use under their conditions.
  That does not mean public-domain, no notices, no trademark restrictions, or no patent/SDK concerns.
  Preserve license and copyright notices, Apache notices where required, and relevant modified-file notices.
  Audit the resolved graph and selected features, not just the top-level framework badge.
- SDK 3.8's MIT terms require no Steinberg fee, membership, or signed agreement.
  [Steinberg's VST trademark and logo rules][vst-license] remain separate if the branding is used.
  This change does not grant MIT rights to NIH-plug's independently GPL-licensed VST3 binding.
- The historical GPUI spike's resolved `cargo-deny` audit found **MPL-2.0** in `option-ext` and `dwrote`,
  and in build-only `cbindgen`. GPUI Kit's Apache-2.0 declaration was not a whole-graph license summary.
  MPL can permit a larger proprietary work, but covered files retain their source and notice obligations.
  That audit used narrow named exceptions and did not modify those MPL crates.
  It describes the removed GPUI graph, not the current plugin's license inventory or legal clearance.
- A no-framework-license-fee stack is not a no-cost release process.
  Apple Developer Program membership is [USD 99 per year or local equivalent][apple-membership],
  relevant to Developer ID signing and notarized distribution.
  Ad-hoc local signing is not Developer ID distribution signing.
  Windows signing certificates/services and release infrastructure can also involve costs.

**Recommendation:** the repository can remain GPL-compliant without rejecting GPL dependencies as inherently unusable.
If a future proprietary product is a goal, make ownership, relicensing, and full dependency review an explicit gate.
This technical evaluation is not legal advice or a license change.

## 10. Build, packaging, signing, and validation

- **Cargo plus a small explicit bundler** fits the raw-binding spike.
  A `cdylib` is only the binary, not a distributable VST3 bundle.
  Emit the correct macOS bundle metadata and executable layout,
  and the correct Windows architecture-specific `.vst3` binary layout.
  Use ImageControl-independent IDs and build-tree paths for the spike.
  The existing test bundler is a useful reference, not a complete signing or release pipeline.
- **nice-plug's bundler** is a useful production candidate if the framework is selected.
  It reduces format-layout boilerplate, but still requires review of metadata,
  embedded resources, symbols, versioning, signing order, architecture, and installer behavior.
  Freeze the selected framework/adapter versions and commit the independent experiment's lockfile.
- **CMake plus Rust artifacts** is necessary for a JUCE/Apple SDK or clap-wrapper bridge.
  This retains two toolchains and a cross-language ABI.
  Pin SDKs/submodules and avoid fetching a moving branch during a release build.

For macOS, assemble resources and all requested architectures before signing the complete final bundle.
Verify signatures after the last bundle modification.
Use Developer ID signing and [Apple's notarization workflow][notarization] for production distribution,
with credentials kept outside source control.
Do not claim a local ad-hoc signature establishes Gatekeeper acceptance.
Preserve the current minimum deployment policy unless deliberately changed and tested.

For Windows, build with the supported MSVC target and verify native loading and editor behavior on Windows.
A cross-check from macOS can check some Rust code but cannot demonstrate Windows windowing, graphics,
DLL dependencies, installer permissions, or signing correctness.
Avoid unintentional local-CPU tuning in distributed binaries and match the existing supported CPU policy.

Run the repository-pinned pluginval against the exact build-tree VST3, with missing tooling reported as failure.
Also use Steinberg's validator where appropriate for interface diagnostics.
For the experimental AU artifact, retain independent auval and real macOS-host qualification.
Installed-format validation and registration are separate actions requiring explicit installation approval.
A successful Cargo build, factory scan, or headless validator does not establish GPU correctness or editor usability.

## 11. Acceptance tests and highest risks

The spike should answer narrow questions before a production implementation is justified:

- Does the host load the isolated module and obtain the correct factory, processor, controller, and view interfaces?
  Does mono/stereo negotiation work and does the intentionally simple audio path remain functional without an editor?
- Does `IPlugView` attach to a host-owned `NSView` or `HWND`, rather than create an unrelated top-level window?
  Are resizing, physical-pixel scale, mixed-DPI monitor movement, focus, keyboard input, and visibility correct?
- Can an editor open, close, reopen, and coexist with several instances without leaking threads or GPU resources?
  Does teardown cancel pending work before the host unloads the module?
  Do hidden views stop drawing, and do device/surface failures leave audio and controls usable?
- Do allocation checks include callback first use on fresh threads, automated parameter updates,
  full queues, state publication, kernel retirement, and teardown-adjacent handoffs?
  Record dependency `BLOCKED` findings separately from clean first-party cases.
- Are shader validation, native GPU diagnostics, and representative host sessions exercised on both platforms?
  Source-level lifecycle tests and ordinary validator success do not substitute for native editor tests.

A subsequent DSP implementation additionally needs the existing processor, crossover, recovery,
correlation, analyzer, state-threading, undo/A-B/preset, and interaction contracts represented in Rust tests.
Use independent analytical expectations and the C++ implementation as separate comparisons.
Preserve current tolerances unless a measured, explained numerical change is approved.

Highest risks are host-owned GUI lifecycle, correct independent IDs and transactional state,
last-owner destruction on realtime threads, macOS AU host qualification, and matching FFT-based performance.
A fast standalone renderer resolves none of the host-lifecycle risks by itself.

## 12. Approximate effort ranges

These are **engineering estimates**, not measured results, commitments, or additions to the approved scope.
They assume one engineer experienced in Rust, plugin APIs, and GPU rendering,
access to macOS and Windows test machines, and no major upstream framework changes.
Ranges overlap and are not additive without a separate implementation breakdown.

- Isolated raw VST3 factory, pass-through processing, one GPU child editor, and basic lifecycle tests:
  **3 to 8 engineering days** for initial macOS evidence and **3 to 8 more days** for native Windows evidence.
  Host-specific teardown or DPI failures can exceed that range.
- nice-plug/baseview/egui integration and compatibility feasibility:
  **1 to 3 weeks**, excluding a complete ImageControl UI and DSP port.
  GPUI embedding proof, if pursued separately: **1 to 3 weeks** to reach a go/no-go decision,
  with a custom reusable platform adapter potentially taking substantially longer.
- DSP, analyzer, realtime ownership, and independent numerical test translation:
  **4 to 8 weeks**, plus performance investigation if RustFFT does not meet the matched baseline.
  An Accelerate backend comparison and audited scratch/scale integration may add **1 to 2 weeks**.
- Full editor interactions, history/A-B/presets, independent state, and host integration:
  **5 to 10 weeks**, depending on reuse and accessibility requirements.
- AUv2 wrapper feasibility and validation: **1 to 3 weeks** using an existing wrapper,
  or **4 to 8 weeks** for a custom native bridge before broad host hardening.
- Release packaging, signing, CI, sanitizers, and representative host hardening:
  **2 to 4 weeks**, with certificate procurement and external review excluded.

A production replacement is plausibly **3 to 6 engineer-months** after a successful bounded spike,
with longer schedules credible if custom GUI embedding requires upstream work.
Exact legacy-project compatibility is outside the clarified RS scope.
The spike is valuable precisely because it can reject an unsuitable embedding path before that investment.

## Current four-band implementation and Iced frontend

The primary crate is [`image-control-plugin-rs`](../crates/plugin/src/lib.rs), using our own VST3 layer,
Clack CLAP, and one shared Iced editor.
The DSP, plugin frontend, and bundler are members of the [repository-root Cargo workspace](../../../Cargo.toml).
They reuse shared dependency versions, lints, formatting, and the root lockfile.
Iced/baseview and the independent bundler remain plugin-local dependencies in their own package manifests.
Nice-plug and its adapter/bundler are absent from the root lockfile and primary dependency graph.
`./build.sh --debug --validate` bundles **ImageControl RS** through a small framework-independent packaging task.
Outputs are repository-root `rust/target/bundled/ImageControl RS.vst3` and `ImageControl RS.clap`,
plus `ImageControl RS.component` on macOS.
The AU statically links the same CLAP implementation, not an installed CLAP dependency.
For a real host, run `./install.sh` from `image-control-plugin/rust/` to build Release and install the native formats.
On macOS it uses `~/Library/Audio/Plug-Ins/VST3`, without replacing the C++ `ImageControl.vst3`.
Select `--no-build` for the current build, `--debug` for Debug, `--destination <dir>` for a custom host folder,
and `--validate` for installed pluginval validation. Every script supports `-h` and `--help`.
The installer supplies per-user VST3 and CLAP destinations on both platforms and a macOS Audio Unit destination.
`--clap-destination` and `--au-destination` select their own folders without changing `--destination`'s VST3 meaning.
Close hosts using the Rust plugin before replacing it, then rescan and select **ImageControl RS**.
It uses a new class ID and bundle identifier `com.esgrove.imagecontrol-rs-own`.
Previous Nice projects are not migrated automatically,
and its installer does not replace `ImageControl RS Nice.vst3`.
No dependencies or GUI code are imported from AudioBatch's application services into the plugin.

### CLAP and AU delivery boundaries

The Clack adapter reuses the authoritative 31-parameter registry, format-independent instance ownership,
detached versioned JSON state, DSP, analysis, and native Iced/wgpu bridge.
CLAP descriptor ID is `com.esgrove.imagecontrol-rs-own`.
CLAP uses plain-unit values with explicit normalized conversion, preserving block-sampled automation.
The AU retains numeric parameter addresses and has independent `aufx / Egir / Esgr` registration,
not C++ `aufx / Egic / Esgr` or its XML state.

The AU integration pins upstream clap-wrapper commit `ddba261acdb421e9963959aa970a10876515aa6a`,
CLAP SDK commit `195b42a004144fab0b3cf95e9c067187d15365b7`,
and AudioUnitSDK commit `53a9a2008aae7fb1b0a9f093dd523b9b12f6c0d9`.
The wrapper pin contains post-v0.17.0 reset forwarding and main-thread native editor teardown fixes.
There are no local third-party source patches.
The exact-artifact native host avoids installation and registration.
Installed strict auval is a separately authorized installer/CI action, not an ordinary build-tree test.

AU support is experimental and carries independent qualification gates.
The inspected AUv2 wrapper does not establish forwarding of host offline mode to `clap.render`,
so synchronous deterministic crossover replacement is not assumed for AU offline bounces.
The pinned clap-wrapper rejects state save/restore before initialization and ignores the CLAP loader's Boolean result.
This affects lifecycle compatibility and restore failure reporting, not every initialized recall.
The wrapper uses render try-admission, spin-waiting parameter ingress,
and bounded preallocated queues with finite capacities.
Render admission failure can output silence.
A clean RTSan run would not prove bounded spin waiting or dropout-free contention.
These dependency limitations must remain explicit instead of borrowing the own-wrapper VST3's clean-callback evidence.
Windows CLAP does not advertise the custom Iced GUI.
Its void destroy callback cannot reject a reentrant Win32 teardown until a later UI turn as VST3 removal does.
Processing, state, automation, and host-provided parameter controls remain available.
Safe synchronous custom-editor destruction and native Windows runtime validation remain gates.
The existing upstream widget-cache,
renderer recovery, accessibility, and module-unload limitations apply to every editor format.
Historical packaged CLAP parameter/state/layout/neutral/reset contracts passed on arm64 macOS before this cleanup.
`./rtsan.sh --format clap` verifies separate positive allocation and mutex controls,
then requires clean cold automation, reset, and concurrent state callbacks on fresh processing threads.
The historical own-wrapper VST3 callback audit was also clean after shared-instance extraction.
These results do not qualify the current cleanup or establish every callback, audio deadlines, or host compatibility.
AU runtime diagnostics confirm initialized-state/neutral/reset contracts but retain the dependency blockers above.
With Rust and the native wrapper instrumented, the fresh-thread AU render reports `malloc`.
The allocation comes from clap-wrapper's `Clap::Plugin::AlwaysAudioThread` lazy thread-local setup.
The actual AU reset boundary reports `pthread_mutex_lock` in Apple's SDK `AUEntryGuard` dispatch.
`./rtsan.sh --format au` fails these required-clean cases.
`./rtsan.sh --format au --diagnostic` verifies the exact call/stack pairs as **BLOCKED**, not clean callbacks.
No dependency source or sanitizer suppression changes hide either finding.
These findings belong to clap-wrapper and Apple's AudioUnitSDK, not our Rust imaging algorithm.
The reset-entry mutex finding is separate from ordinary processing and its host threading contract.
We own the integration and qualification of the shipped AU component.
See [AU dependency limitations](../ARCHITECTURE.md#au-dependency-limitations)
for practical effects, remediation ownership,
and the distinction between passing bounded checks and clean qualification.

### Current cleanup ownership and publication

`AudioState` owns `AtomicRefCell<SyncWrapper<Imager>>` through the external crates' safe APIs.
A single-CAS `try_borrow_mut` rejects overlap without waiting, heap work, or final-owner destruction on the callback.
There is no custom `UnsafeCell` or `unsafe Sync` implementation for this admission boundary.
The host view uses safe `UiOwner<T>(Option<T>)` automatic ownership instead of unsafe `ManuallyDrop` cleanup.
Invalid off-thread or reentrant foreign final release deliberately leaks retained UI resources.
That containment does not qualify module unloading or resolve the separate upstream widget-cache limitation.

Full immutable `ArcSwap` snapshots carry display preferences, layout preferences, and logical geometry off audio.
They replace read-mostly mutexes, not every lock.
Compound editing transactions, worker joins and condition variables, completed-batch handoff,
and preset file locks retain justified non-audio synchronization.
The analysis worker retains the preference `Arc` identity as well as the source token and checks both
before applying settings, publishing completions, draining UI batches, and preparing or rendering GPU work.
The pending instances and retained GPU energy carry that identity too.
This covers the preference-swap/source-invalidation race and prevents stale mixed old/new frames.

The Rust-native cleanup was checked on arm64 macOS on 11 November 2026:

- 253 plugin unit tests, the complete DSP suite, strict all-target Clippy, formatting, and strict no-editor compilation pass.
- Miri passes the parameter/controller checks and safe atomic admission, including nested rejection and unwinding.
- Packaged VST3 and CLAP realtime scenarios pass with separate positive allocation and mutex detector controls.
- The natural logarithmic mapping passes all 264 packaged offline audio-comparison cases at the existing residual bounds.
- Pinned Release pluginval strictness 10 passes. The dependency license audit passes.

These are bounded functional and safety checks, not a speed comparison or complete platform/host qualification.
The final `./build.sh --test --validate` passes after the GPU snapshot-identity and stale-CLAP binding tests.
It checks all exact Release formats without installation, while AU functional diagnostics retain their explicit blockers.
AU wrapper, native Windows, accessibility, device-loss, widget-cache, and unload gates remain independent.

### Implemented band behavior

- Three frequency-sampled, Blackman-Harris-windowed lowpasses form four amplitude-complementary bands.
  The kernel half-length is 4096 through 48 kHz and doubles above that boundary with sample-rate doublings.
  Uniform partitioned overlap-save uses 256-sample partitions and preallocated RealFFT scratch.
  At 48 kHz the actual latency is **4352 samples** and conservative ringing beyond compensation is **4096 samples**.
- Every band has independent Mid and Side source trims, Width, finite quadrature Recover, signal-transfer Pan, and Solo.
  Recovery follows removed Side amplitude below Width 1. It is not a power-preserving compensation law.
  Mono input ignores stereo-only controls. Controls ramp over 20 ms.
- Exact neutral processing copies delayed input bits instead of cancelling filtered bands numerically.
  Neutral history still advances, allowing warm processing activation and crossover handoffs.
  Output trim, latency-aligned Delta, bypass, and the final Mono fold are implemented independently.
  Delta uses the same ordinary band histories, Solo ramps, crossover handoff, and partition timing as the heard signal.
  Fully settled Mono assigns one shared midpoint to both channels, including cancellation-sensitive inputs.
  Preparation designs at the actual requested rate through 2,822,400 Hz, rejecting out-of-range rates without clamping.
- Crossover requests are ordered with minimum one-third-octave spacing.
  Real-time replacement kernels are designed on a worker, crossfaded for one partition,
  and retired through bounded queues so the callback does not free them.
  Explicit offline rendering designs synchronously for deterministic results.
  Saved crossover targets are installed during preparation before the first audible FIR pre-ring.
- The own wrapper publishes exactly 31 stable numeric parameters: output trim, three crossovers,
  six controls per band, bypass, Delta, and Mono. There is no global Width parameter or slider.
  Crossovers use true logarithmic `f64` conversion, with useful plain-unit snapping for stepped controls,
  not JUCE float emulation. Parameter IDs stay stable, but older RS normalized automation is not promised compatibility.
- Independent JSON project state accepts schema 1 only, with a 16 KiB payload limit.
  It directly serializes `Snapshot`, `AnalysisTap`, and `DisplayPreferences`, without duplicate mirrors or migration.
  Sound, A/B sounds and preset origins, layout, display, and logical geometry validate before publication.
  Older and unknown schemas reject transactionally. Missing current-schema fields use fixed defaults.
  Controller/state callbacks never borrow mutable convolution history or wait for GUI dispatch.
  VST3 latency changes issue coalesced `restartComponent(kLatencyChanged)` notifications from SDK-defined
  UI-thread setup/handler callbacks, outside plugin locks and DSP admission, with bounded reentrant follow-up.
- The retained editor follows C++ geometry: compact header with Output, optional processing toolbar,
  dominant GPU plot, resizable divider, correlation row, and four band strips.
  Mid, Side, Width, Recover, Pan, Solo, Link, and their host gestures are functional.
  The plot supports crossover and proportional whole-band movement, modifier source-gain drags/wheel edits,
  reset gestures, and keyboard shortcuts.
  Expanded numeric fields and compact exact-value overlays use validated commits and balanced Link gestures.
  Right-click, focused Enter, and compact-value double-click open exact entry. Slider/crossover double-click resets.
  Modal focus exclusion and retained custom-chrome Tab/keyboard focus do not establish native accessibility support.
  Exact Regular, SemiBold, Bold, and Mono faces are embedded
  rather than relying on synthetic weights or system fallback.
- Display controls include Logarithmic/Mel axes, frequency/phase palettes, adjustable stereo-position curves,
  persistence/contrast, and particle or solid rendering.
  Solid contours sample a preserved quarter-size field before glow upsampling, with bicubic reconstruction,
  layered outlines, and separate glow/highlights.
  Correlation includes measured processed-band values, optional trend-smoothed numbers, recent extrema,
  and a fading range field, with explicit stale/silent unavailability.
  Hide spectrogram, stop analysis, FPS diagnostics, and shortcut help are functional non-sound preferences.
- Local chronological Undo/Redo, independent A/B selection/copy, and preset commands are functional.
  Sound snapshots exclude Solo, Delta, and Bypass. History is transient, and host automation never enters it.
  The per-user RS preset library uses independent JSON, not C++ XML imports or C++ project migration.
  A modal callout supports load, Save/Save As, rename, delete, refresh, reconnect, and absolute-path import/export.
  File work is asynchronous, and native close joins its worker independently of retained Iced widget state.
  Mutations/export validate both accepted revisions and exact bytes under fail-fast process/thread locking,
  with atomic file replacement and explicit stale-selection errors.
  Project origins retain identity, name, and a sound baseline, not filesystem revision permission.
  Reconnect accepts a current library revision without recalling sound or replacing the project baseline.

### Historical parity-completion verification

The 11 November 2026 macOS arm64 parity-completion verification predates the current cleanup.
Its reviewed artifacts passed:

- Debug and Release `./build.sh --test --validate`, including native VST3 and CLAP packaged contracts.
  AU functional contracts passed their bounded scope while reporting upstream state/offline/contention blockers.
- Pinned Release pluginval strictness 10, plus ordinary build-tree strictness 5.
- All 264 packaged C++/Rust offline audio-comparison cases,
  without changing their `1e-5` FS peak or `1e-6` FS RMS limits.
- 244 retained-editor, history, preset, state, native-GPU, and lifecycle unit tests,
  strict all-target Clippy, formatting, no-editor compilation, Miri scalar/admission checks, and the license audit.
- Native VST3 repeated-editor and reentrant-removal contracts, CLAP show/hide/resize/reopen,
  and the AU host's bounded editor open/close checks.
- Required-clean packaged VST3 and CLAP RealtimeSanitizer scenarios with independent positive detector controls.
  CLAP recall additionally fences callback progress inside actual state-stream reads
  and checks accepted parameter readback.
- AU diagnostic RealtimeSanitizer checks retain the verified allocation
  and reset-mutex findings as `BLOCKED`, not clean.

No installed plugin was replaced and no Audio Unit registration was changed for this verification.
These results do not establish native Windows or representative-DAW qualification, native accessibility,
upstream widget-cache reclamation, device-loss recovery, arbitrary module unloading, or clean AU callbacks.
Those artifacts do not verify the current schema-1-only codec, logarithmic mapping, or ownership cleanup.
No new passing cleanup evidence is claimed.

### Cross-implementation and performance evidence

`ImageControlParity` is a separate JUCE console host that loads two explicit build-tree VST3 bundles.
It neither links either implementation's DSP nor searches installed plugins.
Parameter titles are mapped to independently specified plain-unit domains, and readback validates accepted values.
The deterministic signal combines seeded broadband noise, multitone content, boundary impulses, silence,
anti-phase content, polarity inversion, and swapped channels.
The 264-case matrix covers mono/stereo, 44.1/48/96/192 kHz, 32/256/512-sample and irregular blocks,
static band controls, custom crossovers, automation, Solo/Delta, bypass, latency, and complete tail drains.
Independent exact delayed-neutral and silent-Delta expectations accompany analytical gain, pan, and Mono checks.

Acceptance is absolute, with digital full scale equal to one: peak residual at most `1e-5` FS (-100 dBFS),
and RMS residual at most `1e-6` FS (-120 dBFS).
These conservative low residuals are below ordinary audible differences for the bounded test signals.
They are not a universal psychoacoustic guarantee or permission to weaken exact-neutral contracts.
On arm64 macOS, the older reviewed Release artifacts passed all 264 cases on 11 November 2026.
Those artifacts predate the current cleanup, not a fresh packaged run of its parameter, state, or editor implementation.
The largest peak residual was `2.086162567138672e-7` FS, in 192 kHz anti-phase recovery at 200%.
The largest RMS residual was `2.987490796650603e-8` FS, in 44.1 kHz anti-phase recovery at 200%.
Reports retain input checksums, actual parameter readbacks, individual oracle errors, and the worst residual location.
Every active render also checks the last two post-support partitions independently against silence at `1e-6` FS.
A mutation control proves matching persistent ringing cannot pass merely because both outputs null together.
Native Windows measurements remain a separate CI/runtime gate.

Recover uses the same 0–200% plain-unit domain in RS and C++.
Normalized RS automation and saved values map 0–1 linearly onto a 0–2 recovery amplitude multiplier,
without compatibility migration.
Parity includes 100%, 150%, 200%, and automation to and from 200% without clamping requests.
State identities and formats remain independent.

Criterion benchmarks exercise the production Imager over fixed and irregular host blocks,
with preparation separated from preallocated warmed callback work, plus spatial analysis throughput.
The hyperfine runner measures complete packaged-plugin process wall time and separately reports warmed callback time,
finite output counts, energy, and deterministic checksums.
Complete-process time includes module loading, preparation, warmup, copies, checks, and teardown.
Neither a checksum nor a timing is a correctness oracle, and neither proves a realtime deadline.
Machine, compiler, source, and exact binary identities accompany every packaged benchmark artifact.
Only matched workload/build comparisons establish performance changes.

An idle-machine smoke measurement on Apple M3 Max, arm64 macOS 27.2.1, Rust 1.99.1/LLVM 23.1.1,
with the repository Release/native-CPU configuration on 11 November 2026 recorded:

- Criterion `--quick`, 4096 frames per iteration, irregular blocks: 48 kHz neutral `69.934 µs`,
  48 kHz Recover `679.26 µs`, and 192 kHz Recover `2.0590 ms` point estimates.
- Six-tier 48 kHz analyzer throughput: `1.3365 ms` per 4096 input frames.
- Hyperfine, 960000 frames at 48 kHz, irregular blocks, five runs after one warmup:
  Recover `214.2 ± 8.0 ms`, neutral `81.7 ± 10.5 ms`, and mono input `102.7 ± 3.9 ms` complete-process wall time.
  The Recover run's separate last-run warmed callback total was `160.26 ms` across 4888 process calls.

These reviewed-artifact smoke observations predate the current cleanup. They are not measurements of its implementation,
a full statistical baseline, or a C++ speed comparison.
Criterion quick-mode confidence and occasional process-startup outliers limit their precision.
The three retained packaged artifact sets are timestamped 03:00:14, 03:00:42, and 03:00:44
UTC under this directory's ignored `target/benchmark/`.
Criterion estimates remain in the shared `rust/target/criterion/` directory relative to the repository root.
The packaged library SHA-256 was `5b007706b68a4d723a689c00bb27af4fee93f26f95185dffebd4f79a793dcf0e`.
The benchmark host SHA-256 was `a12891288310240d3a79782774d5c54342f133e0303525fcaa309f82f090044c`.
Both binaries were built with Release commands before the `--no-build` measurements.
Artifacts explicitly record that they were reused rather than claiming the benchmark runner built them.

### Historical evidence and current boundaries

Passing results in this section describe reviewed artifacts that predate the current cleanup,
not a fresh qualification of the schema, mapping, admission, UI ownership, or snapshot changes.
Implemented source contracts and retained measurement evidence are distinct. No new measurements are claimed.

Native tests cover complementary direct-convolution expectations, FFT convolution, exact delayed neutral samples,
frequency isolation, gains/Width/Pan/Solo, quadrature response/recovery, finite tails, reset, chunk invariance,
warm history, real-time handoff, and offline crossover replacement.
Actual packaged own-wrapper tests verify a bit-exact delayed neutral impulse, reported latency/tail,
band automation, all-band mute recall, missing-state defaults, and transactional malformed-state rejection.
The historical editor-enabled own VST3 passed pluginval strictness 5 in Debug and Release, strictness 10 in Release,
and repeated native two-editor open/close/reopen before the current cleanup.
An x86-64 Windows MSVC production `--lib --bins` cross-check passed for those reviewed sources.
The expanded all-target check is blocked locally by Criterion's `alloca` C dependency needing Windows SDK headers.
Native Windows CI compiles those benchmark targets with its MSVC toolchain.
Windows execution and graphics remain unverified until that native job runs.
Production Iced slider, crossover, and numeric-input tests drive pointer, key, clipboard, focus, and traversal events.
They verify balanced host gestures, marker constraints, modal focus exclusion, draft cancellation,
unchanged-text automation protection, and reentrant-removal rejection before publication.
A native wgpu test executes particle, fade, bloom, resize, and independent-view GPU histories with actual worker observations.
The selected native graph contains wgpu 27.0.1 and no egui or Iced software renderer.

`./rtsan.sh` builds and audits the actual packaged VST3, with a fresh process per scenario:
clean detector control, positive allocation/mutex controls, cold automation, audio-thread reset,
concurrent same-component state save/restore, and editor open/close/reopen while two processors run.
The restore scenario queries processor/controller identity from the same native COM object
and fences callback progress inside the plugin's `IBStream` reads,
so a stopped or different-instance restore cannot satisfy it.
Every exercised own-wrapper callback is required clean.
There is no diagnostic suppression or BLOCKED allowance.
The marker covers native `process` and `setProcessing` boundaries, not editor construction or state serialization.
No third-party framework source is patched to hide the earlier Nice findings.

Audio-thread code has exclusive nonblocking admission to prepared DSP, bounded sample/automation traversal,
and no controller-handler lock, display notification, or lazy thread-ID lookup.
UI-side host callbacks clone their handler before calling it and release editor borrows before teardown.
Host size and scale changes are coalesced onto a cancellable native UI timer.
A callback-depth guard keeps nested host run loops from applying geometry while Iced still owns its update borrow.
The native `editor_reentry` example drives actual pointer events
and reenters `onSize` and scale callbacks from both `beginEdit` and `performEdit`,
then checks deferred child geometry, balanced gestures, and no late callbacks.
It also verifies grip and host-border resizing, host rejection, per-component reopen geometry,
and saved-state restoration into a fresh component.
It also removes the view from inside `performEdit` and a native frame-change notification,
requiring the child to detach before each callback returns and accepting recursive removal without another timer turn.
These native reentrant-removal results apply to macOS only.
On Windows, `removed()` rejects an active host callback or native geometry flush with `kResultFalse`,
leaving ownership and pending work unchanged so the host can retry after the callback returns.
Hiding or reparenting a focused Win32 child can synchronously deliver focus/DPI events into an already-borrowed Iced runtime.
A host must not destroy the parent or unload the module after rejected removal.
Final release without an accepted removal retains native ownership rather than attempting unsafe reentrant destruction.
Supporting successful reentrant Windows removal remains a production gate, not an established capability.
Native operations retain stable window storage separately from the explicit close capability.
Analysis has an independent shutdown handle,
so both analysis and executor workers join even while Iced state is borrowed.
Open host gestures are balanced by the binding on close, rather than relying on eventual program-state destruction.
Windows deferral owns a private message-only window with a window-local timer, destroyed before its class is unregistered.
It leaves neither a thread-timer procedure in queued messages nor a plugin TLS destructor on the host UI thread.

Each editor uses a private cancellable Iced executor,
whose owner outlives the native runtime and joins its worker on close.
Tests cover pending and never-waking subscription destruction, self-wake pressure, worker panics,
late submissions, independent editors, and scope restoration.
The upstream `thread-pool` feature remains enabled because `iced_baseview` requires an executor feature to compile,
but the editor selects the owned executor and never constructs the default detached pool.
Joined editor tasks do not establish that all graphics-driver or dependency callbacks are safe across module unloading.
The upstream `iced_baseview` owner-close path does not emit `WillClose` to release its `ManuallyDrop` widget cache.
Inspection of version 0.6.0 and the upstream `main` source on 11 November 2026 found the same manual-cleanup pattern.
Program state, workers, native children, and renderer are destroyed,
but complete widget-cache reclamation remains a production gate.
The retained interface includes its widget tree and callback closures, not just rendered pixels.
Manual clearing of our particle vector is mitigation, not evidence that the complete interface was destroyed.
The recommended upstream remedy is cancellation-safe ownership that drops the interface before its borrowed program state
and renderer on every runtime exit, including direct host close, initialization failure, and unwinding.
Merely sending `WillClose` from a destructor is insufficient if the Iced future is currently being polled.
An upstream regression should use destructor counters or weak references around actual widget-owned values,
then exercise ordinary close, host-forced close, repeated reopen, and reentrant removal without screenshot assertions.
Until a verified upstream release is available,
keep the limitation explicit and do not patch registry or vendored sources.
The pre-cleanup primary wrapper passed bounded Miri parameter/controller, atomic-publication, and admission-unwind tests.
Historical Miri coverage of the removed raw adapter is not substituted for those checks.
Full FIR and native-library scenarios are native tests, not interpreted Miri coverage.

This establishes a functional multiband Rust plugin with experimental AU delivery,
not full output parity with the C++ plugin, complete AU host qualification,
native Windows qualification, complete native accessibility, or preserved JUCE projects.
The older GPUI measurements below are historical reference measurements, not Iced performance measurements.
The Iced adapter carries the upstream `block 0.1.6` future-compatibility warning, which needs tracking before production.

## Historical initial GPUI spike and measured evidence

**Historical only:** this section records the removed raw VST3/GPUI implementation and its local measurements.
It is not a description of the current Iced plugin or an available build route.
GPUI required native embedding and compositor patches, retained application-global lifecycle risks,
and had no qualified Windows embedding path. Direct Iced/baseview attachment was selected instead.
The C++ plugin was not changed by this spike.

### Historical artifact and architecture

The separate workspace produced **ImageControl RS**, bundle identifier `com.esgrove.imagecontrol-rs`,
with an independent VST3 class ID.
Its first-party crates were `image-control-dsp`, `image-control-vst3`, `image-control-editor`, and `image-control-gpu`.
The raw wrapper, GPUI editor, Metal renderer, workspace, and vendored patch sources have been removed.
The framework-independent DSP remains in the single Rust implementation.

- **Sound:** mono/stereo gain and global Width, 20 ms ramps, bit-exact neutral shortcut, zero latency and zero tail.
  There were no FIR crossovers, per-band effects, Recover, Delta, or Mono processing in this initial spike.
  Reporting the production plugin's 4352-sample latency here would be incorrect.
- **VST3:** real factory/module exports, component/controller interfaces, finite normalized parameters,
  bounded versioned state codec, host automation, native child editor, and separate bundle generation.
  It implemented `IProcessContextRequirements` with no transport requirements, as the SDK validator requires.
- **Analysis:** bounded `rtrb` queue, one worker-owned Hann FFT tier and log-frequency/stereo-position projection.
  The worker built GPU-ready particle values. The UI only copied completed bounded instances.
  Dropped audio/display work never blocked processing. No band correlation was fabricated.
- **Graphics:** Metal instanced Gaussian particles, half-float ping-pong persistence, reduced-resolution bloom,
  and tone mapping into IOSurface-backed BGRA buffers.
  GPUI composited these buffers on the GPU, without CPU rasterization.
  Three bounded output slots required producer completion and released consumer leases before recycling.
  Both retained GPUI scenes and submitted Metal command completions held leases, preventing overwrite while sampling.
- **Editor:** gpui-kit controls, shared Roboto fonts and centralized ImageControl palette,
  header, disabled A/B/preset placeholders, dominant spatial plot, a draggable split, correlation placeholders,
  and four band columns. The VST3 view accepted host resizing within 760×600 through 4096×4096 logical units.
  Only output gain and global Width had implemented sound behavior. Other controls were visibly unavailable.
  The patched macOS platform returned to the host's run loop and did not replace its application delegate.
  The GPU child view was attached to the host parent. An invisible private NSWindow remained for upstream bookkeeping.

### Historical observed checks

Tests were run on **Apple M3 Max**, 16 CPU cores, 40 GPU cores, 64 GB memory, macOS, native `aarch64-apple-darwin`,
with stable Rust **1.99.1** and the installed nightly toolchain for Miri and RealtimeSanitizer.
These are local results, not a Windows or Intel macOS qualification.

- **Passed:** DSP/analyzer unit tests, including neutral sample bits, narrowed Side, malformed values,
  exact hop endpoints across different input chunking, bounded queue overflow, exclusive worker ownership, and shutdown.
- **Passed:** in-process host automation/state/layout tests and actual build-tree VST3 dynamic loading/processing.
- **Passed:** headless VST3 **Debug and Release pluginval strictness 5**, including Steinberg validator exit code 0.
  Initial failures exposed the missing process-context interface and overly narrow sample-rate rejection.
  Both were fixed.
- **Passed:** Miri on VST3 admission, concurrent atomic state, controller, and in-process host contracts.
  The native dynamic-library case was explicitly excluded under Miri. FFT-heavy cases were ordinary native tests.
- **Passed:** native RealtimeSanitizer allocation detector control and cold processing/automation/reset checks,
  using the shared process-isolated runner. These exercised callbacks had no detected realtime violation.
  This does not prove deadlines, every possible callback, or arbitrary third-party plugin safety.
- **Passed:** native GPU shader compilation, rendering/completion smoke, all-output-slots-leased backpressure,
  and weak-reference recycling/resize safety contracts on the actual Metal device.
- **Passed:** a real native host loaded the **editor-enabled VST3**, processed a stereo sine comb,
  opened and closed its child editor twice, and verified the host delegate was unchanged.
- **Passed:** `cargo-deny` license audit with documented first-party GPL and named MPL exceptions.
- **Passed:** all-feature workspace tests in Debug and Release, including production-view pointer and keyboard edits,
  balanced automation gestures, owner-to-slider synchronization without feedback, and disabled placeholder commands.
- **Passed:** native two-editor attach/resize/close/reopen smoke checks and host-parent child removal assertions.
- **Passed:** editor-enabled Debug and Release VST3 pluginval strictness 5,
  including editor opening and Steinberg validator exit 0.
- **Passed:** first-party `cargo fmt`, semantic prose formatting, and all-feature strict Clippy with warnings denied.
- **Passed:** patched compositor's native BGRA scene-replay/GPU-completion lease regression.
  One unrelated upstream Lilex test was compiled out because the published crate omitted its font fixture.
- **Captured:** a GPU-rendered screenshot of the actual hosted VST3, not a mockup.
  One diagnostic recorded 291,833 real analyzed particles and 86 GPU submissions during a bounded editor run.
  The screenshot and diagnostic were ignored local artifacts, not committed evidence or current reproduction targets.
  A fresh side-by-side C++ editor screenshot was not captured. Layout was checked against its source geometry.
- A multi-module test initially found duplicate Objective-C class registration
  when an editor-enabled test host also loaded the plugin cdylib.
  The spike's fix gave native class names a per-module identity suffix.
  The same module-loading test then passed without suppressing the failure.
- Native embedded accessibility was **unverified/unavailable** because upstream AccessKit attached at whole-window scope.
  The local patch did not install that adapter on the host window. Driven tests were not native accessibility proof.

### Historical offscreen Release GPU timings

The benchmark used **1960×640 physical pixels**, 120 measured frames after 12 warmups, on the M3 Max above.
It submitted the real Metal particle/fade/bloom/composite pipeline, but synthetic particle inputs.
These results **exclude gpui, window presentation, FFT, particle generation, and audio callback work**.
There was no matched C++ baseline, so they establish historical feasibility, not a Rust performance improvement.
They do not measure the current Iced/wgpu renderer.

- **8,192 comb particles:** GPU p50 **0.535 ms**, p95 **0.568 ms**, p99 **0.589 ms**.
  Submission-thread CPU p50 **0.072 ms**, p95 **0.083 ms**. No dropped submissions in this bounded run.
- **32,768 noise-like particles:** GPU p50 **0.583 ms**, p95 **1.047 ms**, p99 **1.058 ms**.
  Submission-thread CPU p50 **0.152 ms**, p95 **0.171 ms**. No dropped submissions in this bounded run.
- Texture readback existed only behind `diagnostic-readback` for explicit offline output inspection.
  The presented plot was never rasterized on the CPU. The GPU path was not an absolute level meter.

The removed crates have no current reproduction commands.
The workload dimensions, warmups, particle counts, and measured distributions above preserve the benchmark context.

### Historical standards and limits

First-party spike code followed the AudioBatch Rust edition, minimum toolchain, item organization,
documentation conventions, strict Clippy policy, rustfmt settings, and narrow documented unsafe modules.
Its per-function `manual_midpoint` expectation preserved the intended mid/side arithmetic order.
The separate workspace disabled Debug symbols and incremental compilation to bound its build-storage footprint.
Sanitizer builds explicitly restored debug information. Correctness linting was not relaxed.
Vendored GPUI retained its upstream style and license, separate from first-party lint policy.

The GPUI spike did not establish an AU or CLAP artifact, Windows child-window/GPU backend,
Intel macOS qualification, DAW compatibility matrix, old JUCE state migration, production DSP parity,
notarization, or safe module unloading.
AudioBatch's host keeps modules loaded for process lifetime. Editor close/reopen is not a module-unload test.
The spike did not implement the full six-tier analyzer or production spectrogram's complete perceptual behavior.
Current Iced implementation evidence and remaining production gates are documented above.

## Primary sources

GitHub source, manifest, license, tag, and commit checks were retrieved with the GitHub CLI/API.
Crates.io metadata, Codeberg release sources, and platform documentation were inspected separately.
Links below identify the upstream material behind the comparisons.
Source inspection is not a complete transitive license audit.

[vst3]: https://github.com/coupler-rs/vst3-rs/tree/b2a45f2d40e602eff5a224b41613735388868590
[sdk-pin]: https://github.com/steinbergmedia/vst3_pluginterfaces/commit/31d6eeba6daaa3e2a8bfbe3e7a90ca0b7fbfbc1c
[sdk-license]: https://github.com/steinbergmedia/vst3sdk/blob/v3.8.0_build_66/LICENSE.txt
[vst-license]: https://steinbergmedia.github.io/vst3_dev_portal/pages/VST%2B3%2BLicensing/VST3+License.html
[nice]: https://codeberg.org/RustAudio/nice-plug/src/tag/v0.4.2/README.md
[nice-manifest]: https://codeberg.org/RustAudio/nice-plug/src/tag/v0.4.2/crates/nice-plug/Cargo.toml
[nice-egui]: https://codeberg.org/RustAudio/nice-plug/src/tag/v0.4.2/crates/nice-plug-egui/Cargo.toml
[egui-baseview]: https://crates.io/api/v1/crates/egui-baseview/0.7.1/dependencies
[nih]: https://github.com/robbert-vdh/nih-plug/tree/de421011f41a6d10fc8c7a6084e4f4dee0143683
[nih-vst3-sys]: https://github.com/robbert-vdh/vst3-sys/blob/b3ff4d775940f5b476b9d1cca02a90e07e1922a2/Cargo.toml
[clack]: https://github.com/prokopyl/clack/tree/27ca28391b57a61bad4bb5099288a7bf9247350d
[clap-wrapper]: https://github.com/free-audio/clap-wrapper/tree/v0.17.0
[conformal]: https://github.com/russellmcc/conformal/tree/5bea4ca9433455bddeeb426ba0679e0f63dd59b8
[au-sdk]: https://github.com/apple/AudioUnitSDK/tree/bd98b31feff57a15989fcfab4cd86dc63382b1ac
[baseview]: https://github.com/RustAudio/baseview/tree/v0.4.2
[egui]: https://crates.io/crates/egui/0.36.1
[gpui-platform]: https://github.com/zed-industries/zed/blob/d2a4fb6/crates/gpui/src/platform.rs
[gpui-kit-license]: https://crates.io/api/v1/crates/gpui-kit/0.9.1
[gpui-license]: https://crates.io/api/v1/crates/gpui-pre/0.3.12
[vizia]: https://github.com/vizia/vizia/tree/12aa7a980663baab783051b98ec39ff3d001bc06
[vizia-plug]: https://github.com/vizia/vizia-plug/tree/34812c0ba14c5df39621bab956c74e660220636b
[wgpu]: https://crates.io/crates/wgpu/30.0.1
[metal]: https://developer.apple.com/documentation/metal
[d3d]: https://learn.microsoft.com/en-us/windows/win32/direct3d12/directx-12-programming-guide
[windows-rs]: https://github.com/microsoft/windows-rs
[objc2]: https://crates.io/crates/objc2/0.6.5
[realfft]: https://crates.io/crates/realfft/3.5.1
[rustfft]: https://github.com/ejmahler/RustFFT
[realfft-scratch]: https://docs.rs/realfft/3.5.1/realfft/trait.RealToComplex.html
[accelerate]: https://developer.apple.com/documentation/accelerate/vdsp
[rtrb]: https://docs.rs/rtrb/0.4.1/rtrb/
[quick-xml]: https://crates.io/crates/quick-xml/0.42.0
[roxmltree]: https://github.com/RazrFalcon/roxmltree/blob/master/Cargo.toml
[apple-membership]: https://developer.apple.com/programs/whats-included/
[notarization]: https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution
