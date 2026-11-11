# ImageControl RS architecture

ImageControl RS is a Rust VST3 effect with framework-independent DSP and an embedded Iced/wgpu editor.
It is a separate plugin identity, not a binary-compatible replacement for the C++ ImageControl implementation.
[README.md](README.md) covers capabilities, build commands, installation, and current limitations.

## Workspace and boundaries

The [repository-root Cargo workspace](../../Cargo.toml) and lockfile own the toolchain metadata and shared dependencies.
There is one Rust plugin implementation, divided into three packages:

- [`image-control-dsp`](crates/dsp/src/lib.rs) owns processing, crossover design, correlation,
  spatial analysis, particle projection, and bounded audio-to-editor transport.
  It depends on neither VST3 nor a graphics or GUI framework.
- [`image-control-plugin-rs`](crates/plugin/src/lib.rs) owns the narrow VST3 ABI adapter,
  sound-parameter registry, host-state codec, native editor attachment, Iced controls, and wgpu passes.
- [`image-control-xtask`](crates/xtask/src/main.rs) packages the native library into a separate VST3 bundle.
  Packaging does not load the plugin or install it.

The production plugin does not import AudioBatch's application core, playback engine, GUI, or storage.
The `vst3-host` package is development-only infrastructure for native host tests and examples.
There is no nice-plug runtime, GPUI editor, vendored embedding patch, or software spectrogram renderer.

## Ownership and execution

### Host processor and controller

[`host/component.rs`](crates/plugin/src/host/component.rs) implements both interfaces on one COM object.
It shares authoritative parameter atomics and a stable `SharedState` allocation.
The 31 sound-parameter IDs, conversions, defaults, and metadata live in [`params.rs`](crates/plugin/src/params.rs).
Controller callbacks and host restores never borrow mutable DSP history.

[`AudioState`](crates/plugin/src/host/audio.rs) admits mutable DSP access through one atomic compare-exchange attempt.
An overlapping lifecycle or processing call is rejected rather than waiting.
The admission guard releases ownership on normal returns and Rust unwinding.
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
No process-global detached executor is created for an editor.
The analysis worker is the sole consumer of the instance's audio queue.
Capture enablement and the exclusive consumer claim are separate controls.

### Native editor lifetime

[`iced_bridge.rs`](crates/plugin/src/iced_bridge.rs) attaches a baseview child to the host-owned `NSView` or `HWND`.
It does not create an unrelated standalone application window or replace the host's event loop.
Ordinary parameter gestures call the retained host handler only on the UI thread,
with handler locks released before invoking host code.

[`host/view_deferred.rs`](crates/plugin/src/host/view_deferred.rs) coalesces host geometry changes.
Cancellable native UI work applies them outside active Iced update borrows.
Native child ownership remains valid through reentrant operations.
Closing balances host gestures, detaches the child, cancels pending geometry, and joins owned workers.

Native reentrant removal is exercised on macOS.
Windows rejects removal during an active callback or geometry flush so the host can retry safely.
The host must not destroy the parent or unload the module after rejected removal.
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
Preparation and reset settle current saved targets before the first audible FIR pre-ring.
Host automation is block-sampled, with band processing advancing at partition boundaries.

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
Correlation presentation history is transient.

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
Old frames are discarded, and a source change clears analyzer, reference, and pending-particle history.
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
The current editor uses logarithmic frequency projection and the default position curve.
Pure projection helpers also support mel, but there is no editor mel control.

[`spectrogram.rs`](crates/plugin/src/spectrogram.rs) owns the Iced shader primitive and per-widget GPU history.
[`spectrogram.wgsl`](crates/plugin/src/spectrogram.wgsl) implements the rendering passes:

1. Clear accumulation and bloom when the stream generation changes, including empty batches.
2. On ordinary resize, resample the previous current accumulation into the replacement GPU target.
3. Fade retained linear energy exponentially using elapsed display time and the selected persistence.
4. Add instanced Gaussian cores and halos with spacing-aware horizontal spread and weighted energy.
5. Generate reduced-resolution bloom and composite tone-mapped energy with the plot background.

Accumulation uses half-float textures and ping-pong targets.
Zero-sized or invalid layouts allocate no history, and target dimensions are bounded by device limits and 4096 pixels.
Each widget has its own stable history identity.
Generation is checked again at rendering so a tap or reset change after upload cannot inject old observations.
Normal rendering neither reads pixels back nor waits synchronously for GPU completion.
The surrounding Iced controls and plot overlays use the same GPU renderer.
Complete renderer/device-loss fallback is not implemented.

## State and editor preferences

[`host/state.rs`](crates/plugin/src/host/state.rs) reads and writes bounded length-prefixed JSON.
A four-byte little-endian payload length precedes at most 4096 bytes.
State version 2 stores normalized sound parameters and detached Pre/Post, persistence, contrast, and palette preferences.
Version 1 remains readable with fixed default preferences, independently of the instance's current editor choices.
Unknown versions, malformed fields, invalid values, and impossible stream progress are rejected.
The complete detached document is validated before publishing any live sound or display values.

State save/restore belongs off the audio thread and does not dispatch to or wait for the GUI.
Publication uses atomics and does not mutate convolution history.
It is not a sample-atomic transaction across all parameters during concurrent automation.
Live analyzer history, particles, GPU energy, correlation history, Link, and layout state are not serialized.
The independent state format does not imply C++ project migration.

[`editor.rs`](crates/plugin/src/editor.rs) composes ordinary controls and polls atomics and completed analysis.
Pre/Post and compact appearance settings are functional without creating host sound-parameter gestures.
The 31 sound parameters retain balanced host gestures, including linked source-trim gestures and interrupted teardown.
Undo/Redo, A/B, and preset commands remain disabled rather than simulating missing services.

## Verification and limits

The DSP tests use analytical signals, direct convolution, explicit Hann/derivative DFT expectations,
calibrated stereo cases, exact delayed-neutral bits, and monitoring-on/off PCM comparisons.
They cover independent tier deadlines, low-bass separation, reassignment, coherent level release,
queue overflow, stale generations, unsupported-rate recovery, and bounded shutdown under producer pressure.

Plugin tests exercise actual in-process and packaged VST3 interfaces,
transactional state validation, stable metadata, automation, native UI events, and editor close/reopen.
Native GPU tests execute real particle, fade, bloom, generation-clear, and resize passes with independent histories.
They establish resource and pass contracts, not screenshot or pixel-golden equivalence.

`./test.sh --miri` covers bounded parameter/controller and admission contracts, not full native FIR or GPU execution.
`./rtsan.sh` separates positive detector controls from required-clean packaged callbacks,
including cold automation, reset, concurrent state restore, and two-editor lifecycle with live monitoring.
Build-tree pluginval checks validate the exact packaged format.
A compile check for Windows does not establish native Windows rendering or lifecycle correctness.

Remaining gates include native Windows qualification, macOS AU delivery, complete accessibility,
upstream widget-cache cleanup, device-loss handling, and arbitrary module-unload safety.
The [evaluation](docs/rust-evaluation.md) separates dependency research and historical evidence from current code.
