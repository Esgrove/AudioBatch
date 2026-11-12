# ImageControl RS architecture

ImageControl RS is a Rust VST3 and CLAP effect with framework-independent DSP and an embedded Iced/wgpu editor.
On macOS, a pinned upstream clap-wrapper AUv2 shell statically hosts the same CLAP implementation.
It is a separate plugin identity, not a binary-compatible replacement for the C++ ImageControl implementation.
C++ is the feature, close user-experience, and audio reference, not an exact internal implementation blueprint.
Older RS project-state and normalized-automation compatibility are not requirements.
[README.md](README.md) covers capabilities, build commands, installation, and current limitations.

## Workspace and boundaries

The [repository-root Cargo workspace](../../Cargo.toml) and lockfile own the toolchain metadata and shared dependencies.
There is one Rust plugin implementation, divided into three packages:

- [`image-control-dsp`](crates/dsp/src/lib.rs) owns processing, crossover design, correlation,
  spatial analysis, particle projection, and bounded audio-to-editor transport.
  It depends on neither VST3 nor a graphics or GUI framework.
- [`image-control-plugin-rs`](crates/plugin/src/lib.rs) owns the narrow VST3 and Clack CLAP adapters,
  shared instance ownership, sound-parameter registry, state codec,
  native editor attachment, Iced controls, and wgpu passes.
- [`image-control-xtask`](crates/xtask/src/main.rs) packages VST3 and CLAP and orchestrates the macOS AU CMake build.
  Packaging does not load the plugin or install it.
- [`au/CMakeLists.txt`](au/CMakeLists.txt) owns the pinned AU wrapper/SDK build and isolated native contract host.
  The AU component contains the Rust CLAP implementation, with no separately installed-plugin dependency.

The production plugin does not import AudioBatch's application core, playback engine, GUI, or storage.
The `vst3-host` package is development-only infrastructure for native host tests and examples.
There is no nice-plug runtime, GPUI editor, vendored embedding patch, or software spectrogram renderer.

## Ownership and execution

### Host processor and controller

[`runtime.rs`](crates/plugin/src/runtime.rs) owns the shared parameter atomics, stable `SharedState`,
logical geometry, and exclusively admitted DSP used by each host adapter.
Stopped preparation installs saved targets and allocates processing state before callbacks begin.
[`host/component.rs`](crates/plugin/src/host/component.rs) implements the VST3 interfaces on one COM object.
The 31 sound-parameter IDs, conversions, defaults, and metadata live in [`params.rs`](crates/plugin/src/params.rs).
Crossovers use a true logarithmic `f64` mapping, `20 * 1000^normalized`, without a frequency grid.
Stepped controls use useful plain-unit snapping, not JUCE float emulation.
Gain steps are 0.01 dB, Width steps are 0.01, and Recover/Pan steps are one percentage point.
Current-schema recalls preserve accepted off-grid normalized bits until explicitly edited.
Parameter IDs remain stable, but older RS normalized-automation compatibility is not promised.
Controller callbacks and host restores never borrow mutable DSP history.

[`AudioState`](crates/plugin/src/host/audio.rs) owns `AtomicRefCell<SyncWrapper<Imager>>`.
The external crates' safe APIs admit exclusive access through one CAS in `try_borrow_mut`,
without a custom `UnsafeCell` or `unsafe Sync` implementation.
An overlapping lifecycle or processing call is rejected rather than waiting.
The borrow guard releases admission on normal returns and Rust unwinding, with no heap work or final-owner destruction.
Raw sample-buffer access stays at the ABI boundary and supports legal in-place bus and channel-array aliasing.

The host serializes preparation and destruction against processing.
Preparation allocates FIR kernels, FFT plans, histories, scratch, and queues before realtime callbacks begin.
The audio thread owns ramps, partition adaptation, convolution history, and correlation moments.
It never acquires editor or host-handler locks, performs GPU work, signals the GUI, logs, or destroys retired kernels.

### Background work

Each prepared imager owns a [kernel design worker](crates/dsp/src/kernel_worker.rs).
It polls coherent crossover requests every 20 ms and returns complete ordinary/quadrature kernel sets.
Bounded ready and retirement queues transfer ownership without callback-side destruction.
Backpressure delays a replacement rather than dropping allocated kernels on the processing thread.
Stopped destruction joins the worker.

Each open editor owns one [analysis worker](crates/dsp/src/worker.rs).
It also owns one cancellable [Iced executor](crates/plugin/src/iced_executor.rs).
Both have independent shutdown ownership so native removal can join them even while retained editor state is borrowed.
Preset filesystem operations use a separate caller-owned worker with detached request/completion values.
Native close joins that worker independently of Iced program-state destruction.
No process-global detached executor is created for an editor.
The analysis worker is the sole consumer of the instance's audio queue.
Capture enablement and the exclusive consumer claim are separate controls.

### Native editor lifetime

[`iced_bridge.rs`](crates/plugin/src/iced_bridge.rs) attaches a baseview child to the host-owned `NSView` or `HWND`.
It does not create an unrelated standalone application window or replace the host's event loop.
Ordinary parameter gestures call the retained host handler only on the UI thread,
with handler locks released before invoking host code, including COM reference-count methods.
Handler slots retain an `Arc` around the COM reference, so cloning under the lock cannot reenter a host `AddRef`.
UI-only `IComponentHandler2::setDirty` notifications cover persistent non-sound choices and accepted size changes
without inventing sound-parameter gestures.
[`host/notifications.rs`](crates/plugin/src/host/notifications.rs) coalesces VST3 latency changes
into `restartComponent(kLatencyChanged)` calls from SDK-defined UI-thread `setupProcessing`
and `setComponentHandler` entries.
Delivery retains pending work without a handler, releases all plugin locks and DSP admission before calling the host,
and bounds reentrant follow-up delivery. It does not rely on an editor or the component construction thread.

[`host/view_deferred.rs`](crates/plugin/src/host/view_deferred.rs) coalesces host geometry changes.
Cancellable native UI work applies them outside active Iced update borrows.
Native child ownership remains valid through reentrant operations.
Closing balances host gestures, detaches the child, cancels pending geometry, and joins owned workers.
The embedded bottom-right grip sends constrained logical dimensions through the editor binding to `IPlugFrame::resizeView`.
The host's `onSize` acknowledgement is authoritative, so rejected requests do not optimistically resize the child.
macOS host dimensions remain points, while Windows dimensions convert once between logical units and physical pixels.
The default editor is 1000 by 764 logical units, bounded by 760 by 600 and 4096 by 4096.
Windows attachment resolves remembered logical size against the live parent's DPI before creating the child.
Explicit physical host sizes are preserved without another DPI multiplication.

Native reentrant removal is exercised on macOS and by the Windows VST3 qualification driver.
The pinned Iced bridge defers nested runtime polling while retaining queued events and active native dispatch storage.
Windows VST3 removal now synchronously retires its child and workers during host callbacks.
Windows CLAP advertises its custom GUI when the editor feature is enabled.
Its native driver exercises ordinary lifecycle and destruction inside an active host parameter callback,
checking synchronous child retirement and independently flushed balanced gestures.
The VST3 `callback-removal` case verifies immediate retirement without nested event dispatch,
while `synchronous-removal` also pumps nested events.
`removal-retry` is a compatibility spelling for `callback-removal`, not an assertion that valid removal is rejected.
Later repeatability checks can be blocked before input delivery by an obstructing desktop window.
The driver treats that as a failure, not a successful lifecycle result.
DSP/state remain independent of a native editor.
The view's safe `UiOwner<T>(Option<T>)` uses ordinary automatic drops rather than unsafe `ManuallyDrop` cleanup.
Invalid off-thread final release, or final release whose native retirement fails, deliberately forgets retained UI resources.
Valid owner-thread final release after callback removal drops the UI owners when the callback unwinds.
This is leak containment, not module-unload qualification.
Complete upstream widget-cache reclamation and arbitrary module unloading remain separate production gates.

## Audio signal flow

[`Imager`](crates/dsp/src/lib.rs) samples atomic targets at host-block boundaries and processes arbitrary block lengths.
[`MultibandImager`](crates/dsp/src/multiband.rs) adapts these frames to 256-sample convolution partitions.
For stereo input, Mid is the channel sum multiplied by one half and Side is the difference multiplied by one half.

Three frequency-sampled, Blackman-Harris-windowed lowpasses form four amplitude-complementary bands.
[`crossover.rs`](crates/dsp/src/crossover.rs) orders crossover targets with at least one-third-octave spacing
and designs ordinary and antisymmetric quadrature kernels.
[`convolver.rs`](crates/dsp/src/convolver.rs) uses 512-point RealFFT transforms with caller-owned scratch.

Each band's processing order is source Mid/Side trims, Width and Recover, Solo contribution, then signal-transfer Pan.
Recover contributes the quadrature-rotated Side multiplied by the source Side trim,
Recover amount, and the removed Width amplitude below unity.
Normalized Recover 0–1 maps linearly to 0–200%, or a DSP amplitude multiplier of 0–2, matching C++.
Host automation, numeric input, and normalized saved state use this same domain without migration.
Mid Off mutes source Mid but does not suppress recovered Side.
Side Off suppresses both ordinary and recovered Side.
Pan transfers signal into the receiving channel rather than merely attenuating the opposite channel.
Mono input uses the Mid path and ignores stereo-only controls.

The mixed result passes through output gain, latency-aligned Delta, and the final Mono fold.
Delta uses an ordinary dry reference with the same Solo selection and partition history as the heard signal.
Settled Mono assigns the same computed midpoint to both channels.
Bypass supplies neutral effective imaging controls and disables Delta through the normal ramps,
without changing saved controls, FIR latency, or the independent final Mono choice.

Gain, Width, Recover, Pan, Solo, output, Delta, and Mono transitions use 20 ms linear ramps.
Their length is the floor of the double-precision sample-rate/time product, matching the native reference.
Output trim and Delta advance while computing the next partition, alongside the band controls.
Mono remains a final fold on returned samples. Unity compatibility Width never adds a redundant Mid/Side round trip.
Preparation and reset settle current saved targets before the first audible FIR pre-ring.
Host automation is block-sampled, with band processing advancing at partition boundaries.
Bus arrangements may arrive before or after setup while inactive, and immediately update the prepared DSP channel mode.
Exact zero-dB Output accepts both the native double-normalized value and its exact float-normalized host representation,
without epsilon snapping nearby nonneutral automation.
The editor projects crossover boundaries through the same ordering helper as audio processing.

### Transparency, latency, and replacement

At settled neutral settings, output copies delayed original channel bits instead of cancelling filtered bands.
The raw convolution histories remain warm so processing activation and kernel replacement do not start from silence.
Unused output convolutions can be skipped when neither processing nor correlation needs them.
Optional monitoring must leave PCM bit-identical, including during ramps and replacements.

The FIR half-length is 4096 samples through 48 kHz and doubles above that rate with sample-rate doublings.
Reported latency is the half-length plus one 256-sample partition.
At 48 kHz this is 4352 samples, with a conservative 4096-sample tail beyond compensation.

Realtime replacements crossfade ordinary and quadrature outputs coherently over one partition,
using the same warm input spectra, before retiring the old kernel allocation to its worker.
Explicit offline mode designs synchronously for scheduling-independent output.
That non-realtime exception may allocate and lock, and must not be selected for realtime callbacks.

## Correlation monitoring

[`correlation.rs`](crates/dsp/src/correlation.rs) maintains double-precision exponentially weighted means,
second moments, and cross moments with a sample-rate-derived 150 ms time constant.
Centered covariance divided by the geometric mean of centered variances gives a bounded correlation value.
Insufficient power in either channel produces unavailable NaN, not a zero reading.

Measurement observes processed bands after trims, Width/Recover, Solo, and Pan,
before global output, Delta, and Mono controls.
It is independent of the spectrogram's Pre/Post tap.
The observation pass copies control ramps and reuses the existing convolved partition,
so live audio ramps advance exactly once.
Neutral measurement can require convolution without disabling the exact delayed-input output shortcut.

The audio thread publishes independent atomic readings at partition rate.
Lifecycle generations reject publications from old callbacks.
Measurement is optional and disabled offline, when its row is hidden, and on editor close.
The editor expires stale readings using observed publication progress and a UI-side clock,
with an allowance of at least 250 ms or two prepared maximum host blocks.
There is no audio-thread clock call.
[`correlation_display.rs`](crates/plugin/src/correlation_display.rs) owns transient display-only trend smoothing,
recent extrema, and a fading range field. Numeric smoothing is optional and does not change measurement or audio.
Correlation presentation history is not serialized.

## Analyzer transport and stream identity

[`monitoring.rs`](crates/dsp/src/monitoring.rs) separates sound controls from analysis configuration,
validated display preferences, availability, and lifecycle identities.
The audio queue holds 32768 copyable stereo frames with source-generation tokens.
A full queue drops display audio rather than blocking or changing output.

Pre selects untouched host input.
Post selects final output after gain, Delta, and Mono.
The producer captures its source token before choosing the samples and queues that same token.
Reset, preparation, tap changes, capture transitions, and monitoring invalidation advance generations.
Rate publication marks the source temporarily unstable, and the worker accepts only a stable rate/source snapshot.
No control thread resets FIFO indices or becomes a second consumer.

The worker examines at most one queue capacity per pass in 4096-frame chunks,
checking cancellation and source identity between chunks.
It retains a coherently published `DisplayConfiguration` with the source token.
A retained projection epoch changes for incompatible frequency-scale or palette edits,
including A-to-B-to-A transitions.
The worker and GPU recheck that epoch before accepting completed work, independently of stream generation publication.
Compatible Persistence, Contrast, stereo-position, Solid, and diagnostic edits retain the epoch and existing energy.
They apply at coherent worker boundaries without resetting FFT history, its clock, or the adaptive level reference.
Source discontinuities and incompatible projection changes discard old work and clear the appropriate history.
Completed batches carry generation and availability even when empty, allowing silent GPU clears.
Display overflow retains the newest bounded tail, with at most 65536 particles per publication/upload stage.

Audio preparation accepts actual rates from 1 through 2,822,400 Hz.
Analysis deliberately accepts only 8 through 768 kHz.
An unsupported rate reports analysis unavailable rather than silently exiting its worker or restricting audio.
The same worker recovers when the rate returns to its supported range.

## Six-tier spatial analysis

[`analyzer.rs`](crates/dsp/src/analyzer.rs) owns independent tier plans and scratch,
a common stereo history ring, and an audio-frame clock.
Named `TierLayout` records define base-rate window lengths, hop lengths, and reported frequency ranges.
Through 48 kHz the tiers are:

- 32768 samples with a 2048-sample hop below 80 Hz.
- 16384 samples with a 1024-sample hop from 80 to 160 Hz.
- 8192 samples with a 512-sample hop from 160 to 320 Hz.
- 4096 samples with a 512-sample hop from 320 to 640 Hz.
- 2048 samples with a 512-sample hop from 640 Hz to 4 kHz.
- 1024 samples with a 512-sample hop above 4 kHz.

Windows and hops scale by powers of two above 48 kHz.
Reported points stay within 20 Hz through 20 kHz and available bins below Nyquist.
Analysis starts at each tier's first hop using zero-filled preceding history.
Input spans stop at exact shared deadlines, making observations independent of host block and worker chunk sizes.
Non-finite channel samples become silence independently without restarting the clock.

One complex FFT packs Left and Right into real and imaginary components.
Its transform length is twice the window length, adding interpolation rather than true frequency resolution.
Conjugate symmetry separates channels.
Three-tap spectral Hann and derivative filters provide local power and bounded instantaneous-frequency reassignment.
A full-scale interior sine in one channel has calibrated power one.

Neighboring powers and cross powers over one-thirty-second octave determine stereo position and coherence.
The default position curve places a 12 dB level difference halfway from center to edge
and a 32 dB difference at 99% of that distance.
Spectral coherence distinguishes equal-level in-phase and opposite-phase content, which both remain centered by level.
It is not the centered time-domain estimator used by the correlation meters.

Each callback delivers reusable points plus tier identity, hop duration, absolute audio endpoint,
and a flag identifying the last tier due at that endpoint.
The analysis windows affect visualization response only, not audio latency.

## Particle projection and GPU rendering

[`particles.rs`](crates/dsp/src/particles.rs) projects observations without graphics dependencies.
Displayed level receives a 3 dB/octave tilt around 1 kHz and maps into a 36 dB range below an adaptive reference.
The reference attacks immediately and releases at 12 dB per audio second, with a -90 dB floor.
Simultaneous tiers share one reference update, retaining each tier's latest peak between deadlines.
Release is independent of tier count, arrival order, input chunking, and UI refresh cadence.

Projection supplies normalized position, bin-spacing-derived spread, linear-light weighted color, size, and emphasis.
Frequency colors or phase-coherence colors share duration weighting and persistence compensation.
The editor selects logarithmic or Mel frequency projection, shared by particles, axes, and crossover interaction.
Validated midpoint/edge level-difference preferences configure the stereo-position curve.

[`spectrogram.rs`](crates/plugin/src/spectrogram.rs) owns the Iced shader primitive and per-widget GPU history.
[`spectrogram.wgsl`](crates/plugin/src/spectrogram.wgsl) implements the rendering passes:

1. Clear accumulation and bloom when the stream generation changes, including empty batches.
2. On ordinary resize, resample the previous current accumulation into the replacement GPU target.
3. Fade retained linear energy exponentially using elapsed display time and the selected persistence.
4. Add instanced Gaussian cores and halos with spacing-aware horizontal spread and weighted energy.
5. Downsample through five reduced-resolution glow levels with a normalized 13-tap filter.
   Copy the actual quarter-size field before additive glow upsampling for solid density contours.
6. Tent-upsample glow additively and composite tone-mapped energy with the rounded plot background.
   Solid mode uses bicubic reconstruction of that preserved field for layered contours,
   with separate glow and sharp highlights rather than thresholding the accumulated bloom.

Accumulation uses half-float textures and ping-pong targets.
Zero-sized or invalid layouts allocate no history, and target dimensions are bounded by device limits and 4096 pixels.
Each widget has its own stable history identity.
Generation is checked again at rendering so a tap or reset change after upload cannot inject old observations.
Normal rendering neither reads pixels back nor waits synchronously for GPU completion.
The surrounding Iced controls and plot overlays use the same GPU renderer.
Complete renderer/device-loss fallback is not implemented.
The pinned `iced_baseview` shell owns the compositor, surface, and presentation-error handling.
`iced_wgpu::window::Compositor` owns the device through its engine and translates surface acquisition errors.
The shell reconfigures an outdated surface and recreates a lost surface, but this is not device recreation.
Its out-of-memory branch panics.
Plugin-owned `GpuHistory` cannot intercept that branch or qualify recovery at the upstream owner.
The existing surface branches remain unqualified by injected Windows faults.

## State and editor preferences

[`state.rs`](crates/plugin/src/state.rs) validates and serializes detached JSON independently of host transport.
[`host/state.rs`](crates/plugin/src/host/state.rs) provides VST3 stream transport,
with a four-byte little-endian payload length preceding at most 16 KiB.
CLAP streams use the same bounded codec. The AU wrapper stores the CLAP stream in its host property-list container.
Parameter and project-state persistence starts at schema 1 for the first supported iteration.
Its detached envelope uses `Snapshot` for normalized parameters,
`AnalysisTap` for Pre/Post, and `DisplayPreferences` directly, without duplicate mirrors or migration codecs.
It also stores A/B sounds and preset origins, complete layout preferences, and host-accepted logical window dimensions.
Layout includes Link, toolbar/correlation visibility, compact bands, retained band height, spectrogram hiding,
the independent stop-analysis choice, and maximized plot geometry.
Maximizing uses the two-unit panel inset with inside-axis labels,
without changing control visibility or window dimensions.
Display includes persistence, contrast, palette, Mel/solid modes, stereo-position curve,
correlation numeric smoothing, and frame-rate visibility.
Absent current-schema fields use fixed defaults rather than the current instance's choices.
Unsupported schemas, malformed fields, invalid values, and impossible stream progress are rejected.
The complete detached document, including comparisons and origins, is validated before publishing live values.
Rejected restores leave parameters, preferences, geometry, comparisons, and restore identity unchanged.

State save/restore belongs off the audio thread and does not dispatch to or wait for the GUI.
Publication does not mutate convolution history and is not sample-atomic across concurrent parameter automation.
Display preferences, layout preferences, and logical geometry use full immutable `ArcSwap` snapshots,
not read-mostly mutexes. Snapshot loads, replacement allocation, and retired-owner drops all stay off audio.
The records are individually coherent, not one aggregate transaction across every setting.
Component-owned geometry survives native view destruction and is captured without waiting for GUI work.
New views use that logical size for host negotiation and attachment, with explicit host dimensions taking precedence.
Rejected resize requests do not change it.
Restoring state affects subsequent views without synchronously resizing an open editor.
Unrelated new instances retain the fixed default size.
Live analyzer history, particles, GPU energy, correlation history, and Undo/Redo history are not serialized.
[`editing.rs`](crates/plugin/src/editing.rs) owns bounded chronological local history
and persistent A/B metadata behind a short non-audio mutex.
Host automation never enters history or takes that mutex.
The compound editing transaction mutex, worker join/condition-variable synchronization,
completed-batch handoff, and preset file locks remain justified off-audio ownership boundaries.
This is not a blanket replacement of mutexes with atomics.
Sound snapshots contain 25 controls, including Mono but excluding Solo, Delta, and Bypass audition settings.
A/B selection, copy, and preset recall are local history commands,
and active-slot capture samples authoritative atomics.
Each preset origin stores identity, name, and the recalled sound baseline, not a filesystem path or accepted revision.
Successful sound restores advance a generation observed by open editors and clear transient history/library bindings.
Pending numeric drafts and gesture participants are cancelled, with generation checks after every reentrant host callback.
A continuing UI operation cannot overwrite a newer recall, including a restore invoked by `beginEdit` or `performEdit`.
The independent state format does not imply C++ project migration.

[`user_presets.rs`](crates/plugin/src/user_presets.rs) owns the independent, bounded RS JSON preset codec and library.
Preset schema 1 stores the current logarithmic parameter mapping.
Prototype presets and host state are outside this baseline and are not migrated,
including prototype files that happen to carry the same version number.
Rust formats share per-user application-data storage under `esgrove/ImageControl RS/Presets`, not the C++ XML library.
The modal callout supports loading, Save/Save As, rename, delete, refresh, reconnect, and absolute-path import/export.
Operations run off the UI/audio threads, and completion checks reject changed project, slot, or sound identities.
Mutations and export revalidate the accepted revision and exact original bytes under a fail-fast process/thread lock.
Writes use temporary files and atomic replacement, with no silent overwrite of stale selections.
Import creates a fresh local identity without recalling sound. Export writes accepted saved bytes, not current edits,
and refuses an existing destination.
Project origins grant no mutation permission.
Reconnect explicitly accepts a current library revision without recalling sound or replacing the project's baseline.
No preset files are read during host state restore.

[`editor.rs`](crates/plugin/src/editor.rs) composes ordinary controls
and observes external changes through [`editor_refresh.rs`](crates/plugin/src/editor_refresh.rs).
Ordinary polling emits application updates only for changed values, active preset/gesture work, or diagnostic windows.
The retained spectrogram drains completed analysis directly on native redraws,
and retained correlation widgets sample publications and advance history without rebuilding the control tree.
The worker wakes from the display cadence with bounded fallback draining when frames pause.
On macOS, the pinned Iced/baseview integration forwards redraw requests to baseview's existing view-bound display link.
Polling and resize request native frames rather than presenting through a competing timer.
Windows retains compatibility scheduling until its backend's fixed redraw timer is separately qualified.
CI exercises dedicated spectrogram resource, invalidation, resize, and multi-widget contracts without timing samples.
Benchmark executables and Criterion targets may be compiled there, but all performance execution remains local.
Whole-pixel header allocation matches the C++ compression order and anchors monitoring and Output to the right.
The comparison/preset toolbar moves into available header space without changing its saved narrow-layout visibility choice.
All frequency ranges share the band-title row only when every measured heading fits, otherwise all use a second row.
Antialiased vector paths draw response guides, continuous under-curve fills, and control icons.
Response geometry is cached by plot bounds, scale, and crossovers, with tessellation retained for hover choices.
Hover disclosures, a separate controls margin, and compact band values preserve quiet interaction targets.
Settings appears as a modal callout without reallocating the plot or moving the parameter controls.
The complete background widget tree is excluded from focus operations and pointer, wheel, keyboard, and touch input
while it is open.
Ordinary Iced sliders synthesize their own release on window focus loss, clearing native drag capture before return.
Pre/Post and appearance settings are functional without creating host sound-parameter gestures.
Embedded Regular, SemiBold, Bold, and Mono faces provide deterministic typography with explicit Iced line metrics.
Band headings and value fields share consistent insets and alignment, and Solo artwork retains its larger click target.
Expanded values and Output use ordinary Iced text inputs beside sliders.
Compact values open an exact-value overlay through right-click, Enter on a focused control, or value double-click.
Slider and crossover double-clicks reset their controls.
Plot interaction supports individual crossovers, proportional whole-band movement, modifier gain drags and wheel edits,
with linked source trims and fine adjustment.
Shortcut help documents Undo/Redo, bypass/Delta, hover Solo, and navigation.
Custom chrome exposes retained Tab focus and keyboard activation, including divider arrow adjustment.
These input contracts do not establish native accessibility qualification.
Drafts remain local during typing and incoming automation. Enter commits one balanced host gesture, Escape cancels,
and focus loss commits valid text or discards invalid text.
Unchanged drafts and typing reverted to its starting text never overwrite newer automation.
The editor rechecks binding lifetime after reentrant host callbacks before publishing any parameter changes.
Units, muted gain, and centered pan use the registry domains without admitting non-finite parameter values.
The 31 sound parameters retain balanced host gestures, including linked source-trim gestures and interrupted teardown.
Undo/Redo, A/B selection/copy, and preset commands use the shared non-audio editing service.
Display settings expose analysis hiding/stopping, frame-rate diagnostics, and modal shortcut help.
The top-right FPS number counts successful spectrum command encoding over a half-second window.
It is neither GPU-completion timing nor physical presentation evidence.

## Verification and limits

The following describes test coverage and verification commands, not new passing evidence for this cleanup.
Retained passing results and performance observations predate it. Current cleanup verification is in progress.

The DSP tests use analytical signals, direct convolution, explicit Hann/derivative DFT expectations,
calibrated stereo cases, exact delayed-neutral bits, and monitoring-on/off PCM comparisons.
They cover independent tier deadlines, low-bass separation, reassignment, coherent level release,
queue overflow, stale generations, unsupported-rate recovery, and bounded shutdown under producer pressure.

Plugin tests exercise actual in-process and packaged VST3 interfaces,
transactional state validation, stable metadata, automation, native UI events, and editor close/reopen.
The separate C++ `ImageControlParity` host loads both packaged plugins with explicit plain-unit mappings,
seeded signals, analytical expectations, full latency/tail drains, and a fixed 264-case offline matrix.
Absolute limits are `1e-5` FS peak residual and `1e-6` FS RMS, while exact neutral and null contracts stay exact.
Recover compares the shared 0–200% domain, including the 200% endpoint and automation ramps.
An opt-in Rust integration tier and `parity.sh` require complete counterpart selection
and preserve per-case JSON reports.
Criterion and hyperfine measure preallocated DSP and actual packaged processing separately from functional validation.
The plugin's `spectrogram` Criterion target reuses warmed production GPU resources through `PreparedRenderer`.
It separates particle packing, CPU submission, and completed offscreen work, with completion bounded to 32-frame batches.
Device setup, shader compilation, immutable workload generation, and validation are outside measured intervals.
It disables timestamp instrumentation, so native timestamp capability does not determine whether Criterion can run.
Neither submission timing nor completed offscreen work establishes physical presentation cadence.

Native GPU tests execute real particle, fade, bloom, generation-clear, and resize passes with independent histories.
They establish resource and pass contracts, not screenshot or pixel-golden equivalence.

`./test.sh --miri` covers bounded parameter/controller and admission contracts, not full native FIR or GPU execution.
`./rtsan.sh` separates positive detector controls from required-clean packaged callbacks,
including cold automation, reset, concurrent state restore, and two-editor lifecycle with live monitoring.
Build-tree pluginval checks validate the exact packaged format.
A compile check for Windows does not establish native Windows rendering or lifecycle correctness.

Remaining gates include native Windows qualification, complete AU host qualification, native accessibility qualification,
upstream widget-cache cleanup, device-loss handling, and arbitrary module-unload safety.
The [evaluation](docs/rust-evaluation.md) separates dependency research and historical evidence from current code.

### AU dependency limitations

The macOS AU component statically hosts our Rust CLAP plugin.
Its translation layer uses upstream [clap-wrapper](https://github.com/free-audio/clap-wrapper)
and Apple's [AudioUnitSDK](https://github.com/apple/AudioUnitSDK).
The own-wrapper VST3 and direct CLAP artifacts do not use this AU translation layer.
The AU identity `aufx / Egir / Esgr` is distinct from C++ `aufx / Egic / Esgr`.

These are known dependency limitations and qualification blockers, not defects in our imaging algorithm:

- **Offline mode, clap-wrapper:** the pinned AU implementation does not forward `OfflineRender` to `clap.render`.
  Our Rust adapter supports offline mode, but the AU route does not activate it.
  Deterministic synchronous crossover replacement is therefore not established for AU bounces,
  particularly when crossovers change during rendering.
- **State lifecycle and errors, clap-wrapper:** save and restore reject access before initialization,
  and restore ignores the Boolean result of the CLAP state loader.
  Hosts using that initialization order or relying on restore failure reporting need independent qualification.
  Bounded initialized-state round trips pass. This is not a claim that ordinary state recall always fails.
- **Contention, clap-wrapper:** render try-admission can output a silent block when processing admission fails.
  Parameter ingress and reset use an unbounded spin wait.
  These are risks under concurrent calls, not evidence that every host session drops audio.
  A clean sanitizer run alone would not establish bounded waiting or dropout-free automation.
- **First-use allocation, clap-wrapper:** the exercised fresh-thread AU render allocates.
  RTSan attributes `malloc` to `Clap::Plugin::AlwaysAudioThread` lazy thread-local initialization.
- **Reset-entry locking, Apple AudioUnitSDK:** the exercised AU reset entry locks.
  RTSan attributes `pthread_mutex_lock` to `AUEntryGuard`.
  This requires separate consideration of the host's reset-thread contract.
  It does not establish that every ordinary processing callback locks.

We own the AU build configuration, immutable dependency pins, and diagnostic hosts/tests,
and remain responsible for the component we distribute.
Remediation requires an upstream dependency fix/update or a different AU integration,
followed by exact-artifact validation, rather than a DSP rewrite.
There are no local third-party source patches.

[`au/tests/audit_upstream.py`](au/tests/audit_upstream.py) checks the pinned wrapper's source identities
and state/offline/contention paths.
[`au/tests/AUHost.mm`](au/tests/AUHost.mm) exercises bounded functional contracts.
`./rtsan.sh --format au --diagnostic` verifies the known allocation and reset-lock reports as `BLOCKED`, not clean.
Passing these audits, bounded checks, or auval does not establish clean AU callbacks.
Separate VST3/CLAP realtime results do not qualify the AU dependency layer either.
None establishes audio deadline guarantees or complete AU host qualification.
