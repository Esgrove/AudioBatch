# Rust ImageControl evaluation

Source inspection date: **10 November 2026**.
This is a feasibility and dependency evaluation for macOS and Windows, not a production rewrite approval.
The C++ plugin remains the supported implementation.
The Rust experiment includes four-band FIR processing and a native GPU editor, but not C++ project-state compatibility.
No framework, graphics backend, or permissive license by itself establishes production readiness.

## Recommendation

- The primary experiment uses **our narrow VST3 layer** on `vst3 0.3.0`,
  with direct `iced_baseview 0.6.0` native attachment and Iced's wgpu renderer.
  Its name is **ImageControl RS**, with its own class and bundle identities.
  Iced keeps application/widget state and routes messages. It is not egui's immediate-mode control model.
  **egui is excluded from the implementation choice**, as requested.
- The GPU spectrogram uses custom wgpu shader primitives for instanced particles, half-float persistence,
  bloom, and composition. The selected Iced graph has no `iced_tiny_skia` software backend and no egui dependency.
  macOS selects Metal, while native Windows graphics remain unverified.
- The own VST3/Iced/wgpu implementation is the single Rust plugin path.
  The raw VST3/patched-GPUI reference, its vendor patches, and its separate workspace have been removed.
  The primary artifact links no nice-plug, nice-plug-iced, or patched GPUI crate.
  No C++ plugin is replaced, and the Rust plugin does not claim JUCE state compatibility.
  The GPUI evidence below is historical, not a second build or installation route.
- Nice-plug was replaced after its packaged cold callback exposed GUI wakeup locks and a state-channel mutex/allocation.
  The replacement samples parameter atomics, transfers kernel ownership through bounded SPSC queues,
  and polls display work from the UI without audio-thread GUI notifications.
  Actual packaged cold automation, reset, concurrent state restore, and two-editor lifecycle scenarios now pass RTSan,
  with positive allocation and mutex detector controls.
  This is bounded evidence, not proof of every callback or deadline.
- Preserve the existing DSP, parameter, state, and realtime contracts explicitly.
  Begin with RustFFT/RealFFT and preallocated scratch, then compare against native FFT performance before optimizing.
  Exact neutral transparency must remain bit-exact. General processed-output equality requires numerical tests,
  not an assumption that a language or FFT substitution produces identical bits.
- Retain an explicit macOS Audio Unit route.
  Rust AU delivery exists through wrappers, including `clap-wrapper`, and through a native Apple SDK bridge.
  None is a drop-in replacement for the present JUCE AU wrapper or its saved-project identity.

## Scope, evidence, and version baseline

Repository sources inspected include [the architecture](../ARCHITECTURE.md),
[parameter definitions](../src/Parameters.h), [processing state](../src/ProcessingState.cpp),
[preset storage](../src/UserPresets.cpp), and [Rust workspace dependencies](../../Cargo.toml).
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

**Recommendation:** use this for the bounded spike, and retain it as the compatibility-first production fallback.
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
Do not call the VST3-only editor spike a complete replacement.
If removing JUCE is mandatory, investigate clap-wrapper AUv2 before commissioning a custom AU wrapper.
Preserve the existing `aufx` type, `Esgr` manufacturer, `Egic` subtype,
and version-hint ordering only after compatibility tests demonstrate a valid replacement.
The spike must use different identities.

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

A framework substitution must preserve more than parameter labels:

- Preserve stable string IDs, host ordering, version hints, ranges, defaults, step counts, normalization/skew,
  display/text parsing, units, finite-value rejection, and balanced begin/perform/end gestures.
  VST3 numeric parameter IDs and component/controller class IDs must match the shipped JUCE mapping,
  not a new framework's default string hash.
  AU addresses and ordering need a separate compatibility check.
- Keep current block-sampled automation and partition-boundary smoothing semantics.
  Opting into sample-accurate framework automation changes behavior and workload and is not a neutral improvement.
  Test controller and processor state synchronization with the editor closed.
- Keep sound settings distinct from audition controls and editor preferences.
  Presets include crossovers, Mid/Side, Width, Recover, Pan, output gain, and Mono.
  Solo, Delta, and Bypass remain live audition controls.
  Link remains editor gesture behavior, not host-automation coupling.
- Preserve chronological local history, nested gestures, A/B slot capture, provenance, and stale-preset handling.
  Undo history remains transient and host automation does not enter it.
  Host restores invalidate pending local transactions and do not synchronously wait for message dispatch.

There are two sensible codec choices:

- **Explicit XML compatibility codec, recommended.**
  Use [quick-xml 0.42.0][quick-xml], MIT, for bounded reading/writing,
  or [roxmltree 0.21.2][roxmltree], MIT OR Apache-2.0, for strict tree inspection plus an explicit writer.
  These are parsing tools, not schema compatibility implementations.
  Preserve JUCE's binary-XML framing around the `ImageControl` tree as well as its XML content.
  Preserve `EditorState`, `SoundState` v1, `ComparisonState` v1, and `PresetProvenanceState` v1.
  Portable `.xml` presets use `ImageControlPreset` v1, plugin identity, UUID, revision, name, and sound data.
  Preserve round-trippable float values, duplicate rejection, complete sound-set validation, range checks,
  deterministic missing-property defaults, and existing malformed-provenance fallback behavior.
- **New native schema with an importer.**
  Serde JSON or a framework's native state representation can simplify a genuinely new plugin product.
  It does not make old DAW projects load automatically and does not preserve old-plugin readback without an exporter.
  This requires a deliberate new identity or explicit migration decision, not an incidental rewrite consequence.

**Recommendation:** treat state compatibility as an independent acceptance gate.
Test C++ write/Rust read, Rust write/C++ read, old states restored onto already-edited instances,
missing Width/Recover defaults, malformed streams, bounds/length overflows, truncated input, and unknown versions.
Keep host state and user preset files as separate contracts.
Do not assume whole `.vstpreset` files and inner processor state are the same format.
Capture actual class and parameter IDs from the shipped wrapper and compare host-visible metadata
before attempting a drop-in identity.
The isolated spike deliberately avoids that compatibility promise.

## 8. What AudioBatch can reuse

There are three different meanings of reuse:

- **Existing crates as development/test dependencies.**
  [`vst3-host`](../../rust/crates/vst3-host/Cargo.toml) can exercise processing and native editor callbacks.
  [`audiobatch-test-plugin`](../../rust/crates/test-plugin/Cargo.toml) demonstrates raw COM factories and test bundling.
  [`test-fixtures`](../../rust/crates/test-fixtures/Cargo.toml) provides fixture discovery and null-test support.
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

The repository's [top-level LICENSE](../../LICENSE) is **GPL version 3**,
and the inspected Rust crate manifests inherit `license-file = "../LICENSE"` through workspace metadata.
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
For AU, run auval and real macOS-host tests once the AU artifact exists.
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

Highest risks are host-owned GUI lifecycle, compatibility of framework-generated IDs and state,
last-owner destruction on realtime threads, macOS AU delivery, and matching FFT-based performance.
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
- Full editor interactions, history/A-B/presets, XML compatibility, and host integration:
  **5 to 10 weeks**, depending on reuse and accessibility requirements.
- AUv2 wrapper feasibility and validation: **1 to 3 weeks** using an existing wrapper,
  or **4 to 8 weeks** for a custom native bridge before broad host hardening.
- Release packaging, signing, CI, sanitizers, and representative host hardening:
  **2 to 4 weeks**, with certificate procurement and external review excluded.

A production replacement is plausibly **3 to 6 engineer-months** after a successful bounded spike,
with longer schedules credible if exact legacy-project compatibility or custom GUI embedding requires upstream work.
The spike is valuable precisely because it can reject an unsuitable embedding path before that investment.

## Current four-band implementation and Iced frontend

The primary crate is [`image-control-plugin-rs`](../rust/crates/plugin/src/lib.rs), using our own VST3 layer and Iced.
The DSP, plugin frontend, and bundler are members of the [repository-root Cargo workspace](../../Cargo.toml).
They reuse shared dependency versions, lints, formatting, and the root lockfile.
Iced/baseview and the independent bundler remain plugin-local dependencies in their own package manifests.
Nice-plug and its adapter/bundler are absent from the root lockfile and primary dependency graph.
`./build.sh --debug --validate` bundles **ImageControl RS** through a small framework-independent packaging task.
The output is repository-root `rust/target/bundled/ImageControl RS.vst3`. The single Rust build is VST3 only.
For a real host, run `./install.sh` from `image-control-plugin/rust/` to build Release and install its VST3.
On macOS it uses `~/Library/Audio/Plug-Ins/VST3`, without replacing the C++ `ImageControl.vst3`.
Select `--no-build` for the current build, `--debug` for Debug, `--destination <dir>` for a custom host folder,
and `--validate` for installed pluginval validation. Every script supports `-h` and `--help`.
The installer supplies a per-user Windows destination and does not install an Audio Unit.
Close hosts using the Rust plugin before replacing it, then rescan and select **ImageControl RS**.
It uses a new class ID and bundle identifier `com.esgrove.imagecontrol-rs-own`.
Previous Nice projects are not migrated automatically,
and its installer does not replace `ImageControl RS Nice.vst3`.
No dependencies or GUI code are imported from AudioBatch's application services into the plugin.

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
  The independent, bounded versioned JSON state validates a complete detached snapshot before publication.
  Controller/state callbacks touch atomics, not mutable convolution history.
- The retained editor follows C++ geometry: compact header with Output, optional processing toolbar,
  dominant GPU plot with draggable crossover markers and axes, resizable divider, correlation row, and four band strips.
  Mid, Side, Width, Recover, Pan, Solo, Link, and their host gestures are functional.
  Settings, Pre/Post selection, Undo/Redo, A/B, and preset commands remain visibly disabled rather than simulating support.
  Correlation displays unavailable placeholders, not invented readings.

### Evidence and boundaries

Native tests cover complementary direct-convolution expectations, FFT convolution, exact delayed neutral samples,
frequency isolation, gains/Width/Pan/Solo, quadrature response/recovery, finite tails, reset, chunk invariance,
warm history, real-time handoff, and offline crossover replacement.
Actual packaged own-wrapper tests verify a bit-exact delayed neutral impulse, reported latency/tail,
band automation, all-band mute recall, missing-state defaults, and transactional malformed-state rejection.
The editor-enabled own VST3 passes pluginval strictness 5 in Debug and Release, strictness 10 in Release,
and repeated native two-editor open/close/reopen.
An x86-64 Windows MSVC `cargo check --all-targets` also passes.
Windows execution and graphics remain unverified.
Production Iced slider and crossover tests drive pointer/key events
and verify balanced host gestures and marker constraints.
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
The primary wrapper passes bounded Miri parameter/controller, atomic-publication, and admission-unwind tests.
Historical Miri coverage of the removed raw adapter is not substituted for those checks.
Full FIR and native-library scenarios are native tests, not interpreted Miri coverage.

This establishes a functional multiband Rust plugin, not full output parity with the C++ plugin,
AU delivery, native Windows qualification, complete native accessibility, or preserved JUCE projects.
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
