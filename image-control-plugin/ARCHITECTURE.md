# Architecture

ImageControl is a C++23 and JUCE multiband mid/side imaging effect with a spatial spectrogram editor.
It builds independently as VST3 on macOS and Windows, and as an Audio Unit on macOS.
The audio processor has no dependency on AudioBatch's application services, storage, or plugin-chain implementation.
Its namespace is `esgrove::image_control` and its plugin identity is `com.esgrove.imagecontrol`.

[README.md](README.md) describes features and commands.
The repository [agent instructions](../AGENTS.md), shared [C++ conventions](../cpp/AGENTS.md),
and [organization rules](../.agents/rules/code-organization.md) describe development constraints.
This document explains ownership, signal flow, concurrency, and the numerical contracts behind the implementation.

## Targets and source organization

[CMakeLists.txt](CMakeLists.txt) defines the source lists and JUCE wrapper targets.

- `ImageControl` is the shared plugin code built by `juce_add_plugin`.
  It links JUCE audio processing, audio utilities, DSP, OpenGL, and embedded fonts.
- `ImageControl_VST3` and `ImageControl_AU` contain JUCE's format-specific client wrappers around the same processor.
  The Audio Unit is an effect, and the VST3 categories are `Fx` and `Spatial`.
  Neither format accepts or produces MIDI.
- `ImageControl_AUSign` signs and verifies the completed macOS Audio Unit bundle after resources are generated.
  The installer copies bundles, not the compiler or JUCE's automatic post-build installation mechanism.
- `ImageControlAssets` embeds the Roboto fonts from the repository's shared `assets/` directory.
- `ImageControlTests` links the real plugin shared code into a GoogleTest executable.
  `ImageControlBenchmark` measures the processor and display CPU workloads.
  `ImageControlPresetWriter` hosts installed formats to prepare the format-comparison benchmark's chain presets.

The code is divided by responsibility rather than by plugin format:

- [`ImageControlProcessor.*`](src/ImageControlProcessor.h) owns the host-facing lifecycle, parameter state,
  analyzer tap, correlation monitoring control, and state synchronization.
- [`Parameters.*`](src/Parameters.h) defines stable parameter IDs, version hints, ranges, defaults,
  display conversions, finite-value parsing, crossover ordering, and editor-setting property names.
- [`dsp/`](src/dsp/MultibandImager.h) contains filter design, partitioned convolution, imaging, and band correlation.
- [`analysis/`](src/analysis/SpatialAnalyzer.h) contains the audio-to-editor SPSC queue and spatial spectral analysis.
- [`ImageControlEditor.*`](src/ImageControlEditor.h) composes controls, the analysis worker, and OpenGL rendering.
  [`ui/`](src/ui/BandStrip.h) contains reusable controls, gestures, display algorithms, and GPU drawing.

The DSP uses JUCE buffers, FFTs, smoothers, and threads, but does not access components or OpenGL.
`SpatialAnalyzer` knows neither the editor nor the renderer.
`SpectrogramParticleBuilder` converts analysis into graphics data without a component or graphics context.
Only `SpectrogramRenderer` implements the OpenGL backend.
Its GLSL programs live in `src/ui/shaders/` with stage-specific extensions.
`ImageControlAssets` embeds those files during the build, so the renderer needs no runtime shader files.
Shader uniform locations are cached for each linked program and rebuilt with the context.
Band-response overlay geometry is cached by plot bounds, frequency scale, and ordered crossovers,
so hover opacity changes reuse the paths.

## Ownership and execution threads

The host owns `ImageControlProcessor` through JUCE's plugin wrapper.
The processor owns APVTS, the message-thread editor-settings tree, `MultibandImager`, and `StereoSampleFifo`.
Non-owning atomic parameter references and reference-wrapper arrays refer into APVTS
and remain valid for the processor lifetime.
The host takes ownership of the editor returned by `createEditor`.
The editor refers to its processor, which must outlive it.

The main execution domains are:

- **Host processing thread.** Reads parameter atomics once per host block, sets DSP targets,
  processes arbitrary block lengths, applies the final Mono fold, and pushes the chosen analysis tap.
  It owns all mutable convolution history, gain ramps, correlation moments, and Mono smoothing.
- **Kernel design thread.** A low-priority `KernelDesignThread` polls crossover requests every 20 ms.
  It allocates and transforms replacement kernels into a spare slot.
- **JUCE message thread.** Owns components, parameter attachments, live editor settings,
  layout, tooltips, VBlank refresh, and application of queued host editor-state restores.
- **Spectrogram worker.** Owns `SpatialAnalyzer` and `SpectrogramParticleBuilder`.
  It is the only consumer of the audio FIFO, and submits completed particles to the renderer.
- **JUCE OpenGL thread.** Owns shaders, GPU buffers, render targets, and frame execution.
  JUCE paints the editor's components over the GPU result.
- **Host state threads.** May save and restore state without being the message thread.
  They use detached snapshots and APVTS's state API, never the live bound editor tree.

Preparation allocates the processing buffers and initial kernels before callbacks begin.
The host must serialize preparation, reset, release, and destruction against processing as required by JUCE.
Reset is allocation-free and clears signal history without resetting the user's parameters.
Release joins the design thread, and editor destruction joins analysis before detaching the OpenGL context.
The context remains alive while the renderer releases its GL resources.
Neither worker uses timeout-based forcible termination.

## Host parameters and controls

APVTS owns three crossover parameters and each band's Mid, Side, Width, Recover, Pan, and Solo parameters,
plus output gain, Bypass, Delta, and Mono.
Version hints distinguish parameter generations for hosts that use ordering hints, including Audio Unit hosts.
Existing IDs and version hints are persistence and automation contracts, not refactoring details.
The Width parameters append with version hint 6 after the existing Mono generation at hint 5,
then the Recover parameters append with hint 7.
Width ranges from 0 to 2 and defaults to 1. Recover ranges from 0% to 200% and defaults to 0%.
`widthParameterId(band)` and `recoverParameterId(band)` name `band1_width` through `band4_width`
and `band1_recover` through `band4_recover`.
All four Width parameters follow Mono, then all four Recover parameters, preserving every existing host index.
Width uses steps of 0.01 and Recover uses whole percentage points.
Fractional text, host values, and saved plain percentages snap to the nearest whole percent.
Recover percentages convert to linear amplitude by dividing by 100, independent of the 200% ceiling.
Saved plain percentages retain their scale and snap to whole-percent precision.
The expanded range maps host-normalized automation 0–1 to 0–200%,
so existing host automation stored as normalized values must be rescaled to preserve its former percentages.
The processor passes `BandWidths` to `setWidths`
and converts host percentages to linear `BandRecoveries` for `setRecoveries`.
Both controls use the same processing path for real-time and offline automation.

The private `FiniteParameter` implementation preserves the declared ranges and host metadata.
It rejects non-finite direct values and invalid text while retaining the previous value.
Text conversion understands Hz/kHz, dB, signed pan percentages, `C`, and `L`/`R` suffixes.
The JUCE fork also rejects non-finite notification values before broadcasting them to hosts or attachments.
Rejecting only `setValue` would be insufficient because `setValueNotifyingHost` has a separate listener broadcast.

Crossovers are ordered and separated by a minimum one-third-octave ratio.
The processor, DSP, and editor use the same ordering helper.
Host parameters are sampled by block, and DSP changes take effect at convolution partition boundaries with smoothing.
The plugin does not implement a separate sample-accurate VST3 parameter-queue engine.

Width and Recover use their own APVTS slider attachments, not a gain-derived Width macro.
Recover sits beneath Width, displays every amount as a whole percentage including `0%`,
and contributes audio only below Width 1. Its parser accepts legacy Off input as zero.
Width remains available with Mid Off. Side Off prevents recovery, while Mid Off mutes only source Mid.
Link is persisted editor behavior that moves Side with a user Mid gain gesture, not incoming host automation.
It does not link or rewrite Width and Recover.
Gesture ownership is captured at pointer-down so Link changes or teardown cannot unbalance begin/end notifications.

## Local editing, comparison, and presets

`ProcessingState.*` represents detached plain-unit parameter snapshots using stable IDs.
Its versioned XML codec validates sound settings before applying them.
Sound recall includes crossovers, Mid, Side, Width, Recover, Pan, output gain, and Mono.
Solo, Delta, and Bypass remain live audition controls. Editor settings, including Link, stay outside sound snapshots.

`ParameterHistory.*` belongs to the processor, survives editor replacement, and is never serialized.
Explicit local transactions capture only touched parameters before their writes.
`ui/HistoryAttachments.*` separates user writes from parameter-to-control synchronization.
Linked controls and two-crossover spectrogram drags nest their boundaries into one logical transaction.
Host automation does not enter history, and host restore invalidates open transactions and recorded actions.
Replay sends balanced host gestures without recording itself or resetting the DSP.
A pre-replay callback advances matching effective preset references before parameter notifications.
Successful replay retains its exact parameter readback for that direction,
so repeated skewed parameter conversions do not create false modified feedback.

The processor owns two comparison slots and the active selection.
Live parameters are authoritative for the active slot, so headless automation is captured when saving or switching.
Switching saves the outgoing sound and recalls the destination. Copy changes only the inactive slot.
Comparison commands and preset recall share chronological history with ordinary parameter edits.
Both slots and their selection are stored separately from editor preferences in host state.
Legacy states initialize both slots from the restored sound and select A.
Bulk recall retains ordinary parameter-by-parameter host update and smoothing semantics,
not a sample-atomic audio transaction.

`UserPresets.*` owns a separate per-user library, shared by plugin formats and instances.
Each versioned file contains one sound snapshot and preset metadata, not a host-state blob.
Library operations validate names and content, serialize cooperating writers,
check stale selections, and replace files through temporary outputs.
The directory is injectable for tests, and construction does not create it.
`ui/ProcessingToolbar.*` schedules filesystem work outside audio and host-state callbacks,
then applies completed user commands on the message thread through checked component lifetimes.
Undo/redo live in the header as vector curved-arrow buttons, alongside a chevron for the A/B and preset row.
The persisted `showProcessingToolbar` editor property defaults to true.
Collapsing that row is immediate and leaves header history actions and asynchronous preset completion handling active.
Preset confirmations and operation errors use an explicitly owned `ImageControlLookAndFeel`,
including the plugin palette, embedded fonts, compact actions, and restrained semantic icons.
Preset names are edited inline in the selector, with a trailing asterisk marking unsaved name or sound changes.
Save atomically replaces the bound preset's name and sound. Save As creates a new preset after inline name entry.
Workflow tests exercise the real inline editor and inject hidden, peerless presentation for remaining alerts.
Their results use the same asynchronous completion handlers without native modal registration or taskbar entries.
The default production alert presentation remains visible, and library diagnostics stay in tooltips and menu feedback.
Successful metadata completions wait for active local sound gestures before publication,
then recheck restore generation, slot, baseline, and bound revision.
Project recall never reads the preset library.
An independently versioned `PresetProvenanceState` root sibling stores each slot's optional UUID, name,
and sound baseline.
It carries no file path, fingerprint, or derived modified flag, and leaves `ComparisonState` v1 unchanged.
The source baseline stays exact.
An optional effective baseline tracks the exact parameter representation established by recall,
so normalization round trips do not imply an edit and distinct published parameter values remain modified.
Malformed provenance clears identity without rejecting valid sound or comparison state.
Host restore clears transient library bindings, and explicit UUID reconnection reads and accepts an exact revision
without changing the project's baseline or sound.
Preset recall publishes provenance before synchronous host parameter callbacks, under the existing state mutex.
Metadata-only changes notify the host, while restores, scans, and transient reconnections do not dirty the project.

## Audio signal flow

`processBlock` and `processBlockBypassed` reach the same `processWithBypass` path.
`ScopedNoDenormals` protects the DSP callback from denormal slowdowns.
The callback reads parameter atomics and supplies changed targets to the imager.
Processing-thread caches reuse dB and percentage conversions while exact raw values are unchanged.
Bypass supplies neutral effective controls without changing saved host values.
Crossover requests are reordered and published only when needed,
while an offline-mode transition still completes outstanding design work.

For stereo input, the imaging representation is
\[
M = (L + R) / 2, \qquad S = (L - R) / 2.
\]
Each signal is divided into four complementary bands.
Source Mid and Side gains are applied before Width/Recover, followed by reconstruction into left and right,
then per-band Pan and Solo contribution, summation, output gain, Delta, the global Mono fold, and the Post tap.
For a band with source trims \(g_M\) and \(g_S\), Width \(w\), and linear Recover \(r\),
\[
M' = g_M M + r\max(0, 1-w)g_S Q(S), \qquad S' = w g_S S.
\]
Here \(Q(S)\) is the band's finite quadrature-rotated Side signal aligned with its ordinary Mid and Side.
The recovery coefficient follows removed waveform amplitude, not removed power.
At Width 0.5 and Recover 100%, it adds half of the phase-rotated, source-trimmed Side into Mid.
Recover 200% doubles that contribution, a 6.0206 dB amplitude increase relative to Recover 100%,
without applying an equivalent gain to the whole output.
There is no square-root power-transfer law, signal-dependent gain compensation, or automatic center boost.
Recovery is independently controlled and defaults to off, rather than being inferred from gain ratios.
Side Off removes both ordinary and recovered Side. Mid Off removes source Mid but leaves recovered content.
Width 0 eliminates ordinary Side before Pan, Width 1 preserves it, and Width 2 doubles it after the source trim.
Recover has no effect at or above Width 1.
The global Mono parameter does not activate recovery or change Width.
It folds whatever the band processing produces, so recovery must be enabled on a narrowed band to preserve its contribution.
A stereo source with identical channels has no Side to recover, and these controls do not synthesize new stereo content.
Mono input uses the Mid path and ignores Width, Recover, and Pan.
Only matching mono-to-mono and stereo-to-stereo main bus layouts are supported.

Pan transfers signal rather than simply attenuating one channel.
For a rightward pan amount \(p\) between 0 and 1,
\[
L' = (1-p)L, \qquad R' = R + pL.
\]
Leftward pan mirrors this operation.
A fully panned shared signal can therefore peak 6 dB higher in the receiving channel.
The processor neither limits nor clips floating-point output to prevent that boost.

Gain, Width, Recover, Pan, Solo, output gain, Delta, and Mono transitions use linear 20 ms ramps.
Preparation and reset read the current parameter targets before settling the imager smoothers,
so saved recovery applies even to the first FIR pre-ring rather than starting a fresh callback ramp.
When all Pans are centered, the mixer sums Mid and Side bands separately and reconstructs once.
Otherwise it reconstructs and pans each band before summing.

### Crossover design

[`CrossoverDesign.cpp`](src/dsp/CrossoverDesign.cpp) designs symmetric FIR lowpasses by frequency sampling.
The desired magnitude transitions from one to zero over one octave on a logarithmic frequency axis,
using a raised-cosine step centered on the crossover.
The design samples that response on an FFT grid four times the kernel half-length,
inverse-transforms it, centers the impulse response, applies a four-term Blackman-Harris window,
and normalizes the taps to unity DC gain before rounding to float.

The half-length \(H\) is 4096 samples through 48 kHz.
It doubles for each doubling above that reference rate, retaining approximately the same frequency resolution.
The complete kernel has \(2H+1\) taps.
At 48 kHz it is 8193 taps long.

Three lowpass outputs \(F_0\), \(F_1\), and \(F_2\), together with the input delayed by \(H\), define the bands:
\[ B_0=F_0, \quad B_1=F_1-F_0, \quad B_2=F_2-F_1, \quad B_3=x_H-F_2. \]
Their algebraic sum is the delayed input, independent of crossover placement.
A separate neutral shortcut preserves bit-exact output rather than relying on floating-point cancellation.

### Finite quadrature recovery

Recovery uses odd-length, Type III antisymmetric FIRs with the same half-length \(H\) as the ordinary crossover kernels.
`designQuadratureKernel` samples an imaginary spectrum on the same four-times-half-length FFT grid.
Positive-frequency bins contain minus the target magnitude, conjugate negative-frequency bins contain plus the magnitude,
and the DC and Nyquist bins are zero.
The lowpass target is the ordinary crossover magnitude, while the fullband target is unity between the endpoints.
After inverse transformation and centering, the designer projects mirrored tap pairs onto exact antisymmetry,
applies the shared Blackman-Harris window, and stores each float tap with its exact negative partner.
The center tap stays zero. There is no DC normalization, because the quadrature impulse has zero sum.
Relative to the common delay, the useful positive-frequency response is approximately minus 90 degrees:
a cosine becomes a sine, and a sine becomes a negative cosine.

The design produces three quadrature lowpass kernels and a fullband quadrature kernel.
Its low band is the first quadrature lowpass.
The middle bands are differences of neighboring quadrature lowpasses.
The high band is the fullband quadrature output minus the highest quadrature lowpass.
Their algebraic sum is the fullband quadrature output, not the delayed original Side.
The ordinary and quadrature Side kernels read the same raw Side spectral history.
The quadrature branch does not filter an already filtered or delayed band, so it is not a cascaded Hilbert stage.
Its fixed finite support adds no extra convolution delay or tail beyond the existing \(H+P\) latency and \(H/f_s\) tail.

Antisymmetry makes the quadrature response zero at DC and Nyquist.
The fullband filter tests bound gain error to 0.00055 from unity on logarithmic 20 Hz to 20 kHz grids,
with additional 20, 21, 25, and 30 Hz probes at 44.1, 48, 88.2, 96, 176.4, and 192 kHz.
The centered response remains quadrature within a real-component tolerance of 1e-12 on those probes.
These are sampled response bounds, not a continuous-frequency proof or a perceptual preservation guarantee.
The lowest legal 20 Hz crossover retains its finite transition and is not normalized as an ordinary DC-passing lowpass.
Finite-band quadrature is not lossless recovery, loudness matching, automatic gain compensation, or a peak limiter.
For arbitrary source phases, recovered Side can cancel with source Mid or add to it and raise peaks.
Quadrature reduces the channel bias of direct addition, where full \(M+S\) recovery would simply select the original Left.
It does not make arbitrary Mid and Side signals orthogonal or preserve their combined power.
The overlapping crossover bands are amplitude-complementary, not an orthogonal energy decomposition.
Recovery is not invariant under swapping L/R, which reverses Side while leaving Mid unchanged.
Global polarity inversion remains consistent with the linear signal law for fixed controls.

### Partitioned convolution and latency

[`PartitionedConvolver.cpp`](src/dsp/PartitionedConvolver.cpp) implements uniform overlap-save convolution.
The processing partition \(P\) is 256 samples and the transform size is \(2P=512\).
Each kernel partition is zero-padded and transformed once during kernel design.
Each Mid or Side input partition is transformed together with the previous partition and retained in a spectral ring.
All three ordinary filters reuse that input transform and history.
The three quadrature lowpasses and fullband quadrature kernel reuse the raw Side history as parallel filters.
`isRecoveryActive` checks current and target Width/Recover ramps before deciding whether outputs are required.
This includes fades into and out of recovery, rather than only the settled parameter values.
If any stereo band can recover, `convolvePartition` computes all three quadrature lowpasses and the fullband reference.
Selecting fewer bands changes their mix coefficients but does not currently prune individual recovery kernels.
When recovery is inactive, all four extra output convolutions are skipped while raw Side history stays warm.
Activation therefore needs no second filter warmup, and neither meter visibility nor an open editor enables this DSP.
The exact-neutral shortcut still advances inactive Recover ramps, preserving later automation behavior.

For each filter, the convolver multiplies kernel partitions by appropriately delayed input spectra and accumulates them.
The inverse transform's second half is the next causal output partition.
Spectra use separate contiguous real and imaginary planes to make the multiply-add loop SIMD-friendly.
JUCE supplies the platform FFT implementation.

`MultibandImager::process` adapts any host block length by filling an input partition
while returning samples from the already computed output partition.
It never resizes buffers according to the incoming block length.
That adapter adds one partition of latency, giving \(H+P\) reported samples.
At 48 kHz this is 4352 samples, about 90.7 ms.
A delay ring aligns the dry reference with the filtered signals before the partition output delay.
The finite tail beyond the latency-compensated endpoint is bounded by \(H/f_s\) seconds.
Tail reporting remains conservative regardless of current gains or solos.

### Kernel replacement and offline rendering

Crossover requests are published as three atomic values protected by an even/odd sequence counter.
The design thread retries until it reads one complete request rather than a mixture of requests.
It builds the ordinary and quadrature `KernelSpectra` together in the inactive slot
and publishes `switchPending` with release semantics.

At a processing partition boundary, the audio thread acquires the publication,
convolves the same input history with both kernel slots,
and crossfades ordinary and quadrature outputs coherently over one partition.
All ordinary and recovered band differences use the same active-slot publication and fade progression.
After Mid, Side, and any active recovery outputs finish, it publishes the active slot
and releases the old slot for later redesign.
The callback swaps indices rather than moving or destroying allocated spectra.

With `isNonRealtime()` set by the host, the calling processing thread designs moved crossovers synchronously.
A mutex serializes that work against any design already in flight, and the background designer stands aside.
Realtime pending requests retain bounded slot ownership until their next partition handoff.
Entering offline mode supersedes stale pending slots with the latest complete request before processing.
Returning to the active frequencies cancels obsolete pending work instead of replaying it.
Request identity is exact, so small accepted automation changes are not discarded by approximate display equality.
This makes offline results independent of worker scheduling, but deliberately allows allocation and locking offline.
It is not a real-time-safe mode switch policy for a host that mislabels its callbacks.

### Transparency, bypass, Delta, and Mono

Once neutral controls settle, the imager copies delayed original channel samples directly.
Neutrality includes unity Width and no effective recovery contribution, as well as unity gains and centered Pans.
Recover alone at Width 1 or above does not make a contribution.
It still advances convolution history, including during kernel replacements,
so later processing changes have a warm filter history.
When correlation is disabled, exact-neutral partitions skip the unused output convolutions,
while retaining raw input spectra, delay history, control progression, and pending kernel publication.
Exact neutral Delta clears the output partition rather than reconstructing a nearly zero difference.
These shortcuts also operate during crossover crossfades.

Bypass supplies unity source gains, unity Width, Recover off, centered Pans, all bands audible,
and Delta off through the same ramps.
It does not skip processing or change latency.
Both the exposed host bypass parameter and `processBlockBypassed` use this path.

Delta subtracts an aligned ordinary dry reference from the processed result, including Width/Recover.
With solos active, its dry reference contains only the selected bands.
Mono runs last in `ImageControlProcessor`, before the Post analyzer tap.
It smoothly folds the full processed output to \((L+R)/2\) in both channels, including after Delta or bypass.
The saved, automatable `mono` parameter has version hint 5 and defaults to off for states without it.
Its 20 ms ramp applies identically in real-time and offline callbacks.
Preparation and reset initialize the blend from the parameter so an enabled fold applies from the first output sample.
Pre analysis remains the unprocessed input.

## Per-band correlation

[`BandCorrelation`](src/dsp/BandCorrelation.h) measures each processed band's stereo signal
after source trims, Width/Recover, Solo, and Pan,
before global Delta and Mono processing.
It reuses the existing ordinary and quadrature outputs and copies control smoothers for observation,
without advancing the audio path's ramps or adding another filter bank.
The measured values do not depend on the spectrogram's Pre/Post selection.

Each band accumulates exponentially weighted means, second moments, and a cross moment in double precision.
The integration time is 150 ms, with the per-sample weight derived from the prepared sample rate.
Centered covariance divided by the geometric mean of the two centered variances gives correlation.
Values are clamped to [-1, +1].
Insufficient power in either channel, including silence or a fully one-sided band, produces an unavailable NaN reading.

The audio thread publishes one atomic float per band per partition.
These are independent meter readings, not a coherent four-band transaction.
The editor requests measurement only while its correlation row is visible and the editor is showing.
Generation counters reject results from a callback finishing after its view was hidden or closed.
Lifecycle transitions invalidate published values, and callback timestamps expire stale readings
when a host stops calling without releasing resources.
Expiry is at least 250 ms and allows two prepared host blocks when those are longer.
Only the processing thread touches the accumulated moments.

The message-thread meter keeps the live marker immediate.
A yellow highlight with a narrow core and soft halo adds an exponential trend, with Persistence as its time constant.
The numerical readout and accessible value follow that trend by default.
`Smooth correlation numbers` binds the persisted `smoothCorrelationReadout` editor property, which defaults to true.
Disabling it selects live numerical and accessible values immediately, without clearing or advancing history.
A soft spatial field fills the range between recent observations and the live reading.
Each observation's coverage fades with a smoothstep age envelope, reaching zero at Persistence without an extra release tail.
A fixed 129-sample field has feathered horizontal edges and layered vertical glow, matching the spectrogram's soft treatment.
The exact extrema remain the minimum and maximum display-observed readings over the trailing Persistence duration.
They do not promise audio-thread extrema between display refreshes.
A fixed ring retains 1/120-second buckets of observed minima and maxima for the maximum Persistence window.
Each extreme keeps its actual observation time so the visual fade is continuous across bucket boundaries.
An extreme can remain in the exact range at most one bucket beyond its nominal expiry.
Shortening or increasing Persistence immediately selects a new window from that retained history,
without resetting the live value or trend.
Elapsed seconds, rather than frame counts or capped animation steps, drive smoothing and expiry.
Unavailable readings, hidden meters or editors, and processor lifecycle generation changes clear presentation history.
A display gap longer than maximum Persistence also reseeds it from the next valid reading.
The snapshot carries the existing lifecycle generation so resets between display refreshes cannot preserve old history.
History is never saved, and display smoothing does not change DSP accumulation or audio output.

## Analyzer transport and stream boundaries

[`StereoSampleFifo.h`](src/analysis/StereoSampleFifo.h) owns fixed stereo audio storage
and a source epoch for each frame.
It uses `juce::AbstractFifo` with one producer, the audio callback, and one consumer, the spectrogram worker.
Storage is 32768 frames, with one slot reserved by the ring's full/empty convention.
Frames that do not fit are dropped rather than blocking processing.

An epoch encodes the selected input/output tap and a generation, with a transient unstable bit while publishing sample rate.
The callback captures the epoch before choosing Pre or Post and pushes with that same token.
Tap, rate, preparation, reset, clear, or analysis-enable transitions invalidate previous generations.
The worker discards stale frames and clears its analysis history before accepting the current generation.
No control-thread action resets FIFO indices or becomes a second consumer.

The worker drains in 4096-frame chunks, examines at most one storage capacity per pass,
and checks shutdown between chunks.
A faster-than-real-time producer therefore cannot keep a pass running forever.
It wakes on VBlank notification or a 50 ms fallback timeout, so the queue keeps draining when display refresh stalls.
When analysis is hidden or bypassed, it drains without running the FFT analysis.
Completion tickets let tests observe a finished pass without consuming audio themselves.

## Multi-resolution spatial analysis

[`SpatialAnalyzer`](src/analysis/SpatialAnalyzer.h) retains stereo history and runs six independent analysis tiers.
At rates through 48 kHz, the window lengths and reported ranges are:

- 32768 samples below 80 Hz, advancing by 2048 samples.
- 16384 samples from 80 to 160 Hz, advancing by 1024 samples.
- 8192 samples from 160 to 320 Hz, advancing by 512 samples.
- 4096 samples from 320 to 640 Hz, advancing by 512 samples.
- 2048 samples from 640 Hz to 4 kHz, advancing by 512 samples.
- 1024 samples above 4 kHz, advancing by 512 samples.

Windows and hops double with each sample-rate doubling above 48 kHz.
Input writes stop at exact hop deadlines, so callback window endpoints do not depend on input span boundaries.
The reported frequency range is limited to 20 Hz through 20 kHz and available FFT bins below Nyquist.
These display windows do not add audio-processing latency, but long bass windows respond more slowly.

Each tier packs left samples into the real part and right samples into the imaginary part of one complex FFT input.
The transform is twice the window length, providing zero-padding interpolation rather than additional true resolution.
Conjugate symmetry separates the two real-channel spectra.
A periodic Hann window and its time derivative are applied as three-tap spectral filters using bins two steps apart.
The derivative estimates instantaneous frequency for reassignment, bounded to the window's main-lobe neighborhood.
Reassignment changes where the point is drawn, not the source power assigned to it.

Stereo position comes from the channel magnitude ratio in dB through `StereoPositionCurve`.
The configurable midpoint maps to half the distance from center, and the edge setting maps to 99%.
Position and phase coherence average neighboring bins over a narrow one-thirty-second-octave interval.
Each point's brightness input remains its own normalized combined channel power.

Phase coherence is the real cross spectrum divided by the product of channel magnitudes.
It distinguishes in-phase and anti-phase content, while position alone cannot:
equal-level opposite-polarity audio remains centered.
This spectral coherence is not the same estimator as the time-domain centered correlation meters.

## Particle construction and GPU rendering

### Worker-side particles

[`SpectrogramParticleBuilder`](src/ui/SpectrogramParticles.h) maps each `SpectralPoint` to plot-relative position,
horizontal spread, linear-light color, size, and emphasis.
The horizontal scale is logarithmic or HTK mel, shared with overlays and input handling.
Position maps full left to the top, center to the middle, and full right to the bottom.

Displayed level receives a 3 dB-per-octave tilt around 1 kHz
and is mapped into a 36 dB range below an automatic reference.
The reference rises immediately and falls at 12 dB per audio second, with a -90 dB lower bound.
Frames carry their tier identity and absolute audio endpoint.
Simultaneous tier frames share a reference after all due peaks are observed,
and each tier retains its latest peak between updates.
The common release clock advances only once per endpoint, independently of tier count and input chunking.
Contrast emphasizes stronger points and increases toward the highs.
Frame-duration weighting and persistence compensation keep tiers with different hops from drawing at arbitrary brightness.
Colors are either a frequency palette interpolated in linear light or a phase-coherence blend.
The resulting relative display is not an absolute output-level meter.

### Message-thread refresh and overlays

`ImageControlEditor` uses `juce::VBlankAttachment` rather than an always-running GL repaint loop.
Every refresh applies changed display settings, wakes analysis, advances hover/tooltip animations,
updates correlation components, submits plot geometry and elapsed fade time, and requests a GL frame.
Message-thread ValueTree notifications mark property-derived settings dirty.
Unchanged frames reuse their derived controls and worker settings, while visibility, bypass, and diagnostics remain live.
Effective analysis enablement also follows host/ancestor visibility and renderer availability.
Hidden or unavailable displays keep draining the audio queue without FFT or particle work.
Unchanged position-curve settings reuse their derived coefficients.
Large refresh gaps are capped for visual animation and fade progression.

`SpatialSpectrogramView` paints axes, grids, band regions, response guides, crossover lines, and status text.
The optional `maximizeSpectrogram` editor setting expands the plot to a two-unit panel inset,
with frequency and L/C/R labels overlaid inside it. Standard outside-axis gutters remain the default.
Both layouts share renderer submission and pointer geometry, and switching balances an active plot gesture.
The setting preserves window dimensions, row visibility, saved full-strip sizing, and sound settings.
Its separate `SpatialSpectrogramViewInput.cpp` owns hit testing and parameter gestures.
The plot itself is left unpainted so the GPU image beneath the components remains visible.
`BandStrip` owns APVTS slider/button attachments, including independent Width and Recover sliders,
and gain-only Link gesture behavior.
Band names and frequency ranges share a heading only when every band's text fits beside Solo and Link.
Otherwise all ranges move to the second line together, keeping the slider rows aligned.
Resizing and live range changes update that shared layout without committing numeric drafts.
Band names use embedded Roboto SemiBold at 16 logical units, with zero-border headings aligned to the slider labels.
Solo fits 16-unit artwork inside its unchanged 20-unit hit and focus target.
`ImageControlLookAndFeel.h` centralizes surface radii,
including the 8-unit panels and 6-unit plot mask shared by the CPU cutout and GPU composite.
The independent `BandCorrelationMeter` components occupy columns aligned with those strips.
Compact layout and row visibility preserve the saved expanded editor height.
`DisplayDisclosure` fades a centered translucent triangle in only near its section-border action.
A narrow vertical proximity region retargets the fade immediately, with a bounded 100 ms fade-out after exit.
Hidden sections retain full-width labelled restore rows, without adding a display toolbar.
Pointer-acquired focus stays quiet, while keyboard traversal has a local neutral outline.
The spectrum and expanded band-controls triangles are centered on their corresponding top borders.
The controls target occupies only the border and upper margin, clear of editable values.
The divider grip is hidden until hover, dragging, or keyboard focus.
Spectrum Left/Right keys move a hovered crossover or proportionally move a whole band.
Ordinary steps use ten percent of the crossover or geometric band-centre frequency, and Shift uses one percent,
with each reference-frequency step bounded to 1–1000 Hz.
The band-control disclosure replaces full strips with a compact Mid/Side/Width/Pan row,
using the same sliders and attachments.
Its persisted `compactBandControls` choice defaults to false, leaving legacy projects in the full view.
Compacting retains the window dimensions and saved full-strip height, releasing the remaining space to the spectrogram.
A single 28-pixel row uses value suffixes and directional pan text, without band headings.
Its values and inline editors use embedded Roboto Mono SemiBold, with measured widths for complete signed values.
Wider columns use full inline control names based on measured text widths, never a separate label row.
The brand title uses embedded Roboto SemiBold rather than synthetic font weight.
With the spectrogram hidden, the fixed-height editor instead follows the compact value row.
A/B and preset controls reuse the same live toolbar when the header's controls and minimum preset width fit.
The dedicated second row closes automatically while inline and returns only when its saved visibility preference is on.
Automatic placement never changes that preference or the saved expanded height.
The preset name field is capped at 320 pixels in either placement.
Preset identity appears only in the selector, leaving the brand title unchanged when the row is hidden.
`ControlSlider` separates numeric text from fixed display units in its plugin-local value label.
The inline editor shares the readout's font and placement.
Up/Down uses the slider's ordinary fine/coarse nudge path.
Wheel input over the focused numeric editor uses the slider's wheel accumulation and Shift steps,
with the same host and history gestures.
It retains numeric editing after a changed value. Substep and clamped events preserve the uncommitted text.
`SettingsPanel` uses natural content height up to the editor's available bottom edge,
bounds any remaining scrolling content to that space, keeps focused controls visible,
validates numeric tokens, and scopes appearance reset separately from sound, visibility, and geometry.
The modal Settings callout admits pointer events only to its opener,
so a repeat click completes the ordinary release-driven close action without reopening.
Its footer and F1 enter a shortcuts submenu inside the same callout.
Back or Escape returns to preferences without losing their scroll position or changing sound.
The stereo-position overlay starts at the plot's right edge, leaving the centre-to-edge mapping visible beside Settings.

### OpenGL passes

[`SpectrogramRenderer`](src/ui/SpectrogramRenderer.h) receives particles from the worker
and frame settings from the message thread.
A mutex protects pending work, never the audio callback.
Particles accumulate in a bounded queue, and elapsed fade is bounded until the GL thread consumes it.
The renderer swaps vector storage for reuse instead of copying the whole queue each frame.

The context requests OpenGL 4.1, and the shaders require 3.3.
The frame pipeline is:

1. Size the accumulation target in physical display pixels and retain its image across a successful resize.
2. Multiply the previous accumulation by an exponential persistence fade.
3. Add instanced Gaussian particle sprites, with sharp cores and emphasis-dependent halos.
4. Downsample into five glow levels and upsample additively to build bloom.
5. In solid mode, retain a smooth density field for translucent layers and contour treatment.
6. Composite accumulated energy and glow with exposure, tone mapping, background, and rounded plot edges.
7. Restore the surrounding GL state needed by JUCE's subsequent component painting.

macOS uses half-float accumulation textures, while Windows uses full-float textures.
Shader or framebuffer failure marks rendering unavailable, leaving processing and ordinary controls usable.
Context creation and closure own GL allocation and destruction.
Debug builds use timer queries read several frames later, avoiding a synchronous GPU completion wait for timing.
No production DSP depends on a successful graphics context.

## Saved state and host threading

`getStateInformation` writes JUCE binary XML containing the APVTS parameter tree,
comparison state, and an `EditorState` child.
Editor properties include links, analysis mode, display preferences, correlation visibility, dimensions, and divider height.
Live analysis, meter history, and GPU contents are not persisted.
Mono is persisted with the other audio parameters.
Missing editor properties restore from deterministic defaults, independently of the instance's previous edits.
Missing audio parameters use their declared defaults through APVTS.
Detached restore targets expose the actual clamped and snapped parameter values during reentrant save callbacks,
while retaining the original request separately to avoid repeated skew rounding and preserve preset provenance.
A host recall cancels pending band and Output numeric drafts even when the restored values are unchanged.
Older states without Width or Recover load unity Width and Recover Off, preserving their existing gain-driven sound.
There is no gain-derived Width migration, so unequal saved Mid/Side gains remain independent source trims.

The live editor `ValueTree`, its retained copies, and bound `juce::Value` objects belong exclusively to the message thread.
Host state callbacks use a detached binary editor snapshot under `editorStateMutex`.
A separate recursive state mutex serializes host saves/restores and local processing commands,
and allows synchronous reentrant host save requests.
These APIs allocate and lock and are not intended for real-time processing callbacks.

An off-thread restore publishes its snapshot and analyzer tap immediately, restores APVTS parameters,
and posts an `AsyncUpdater` for message-thread application.
It never waits for message dispatch, avoiding a host worker/message-thread join deadlock.
`getEditorState` also flushes pending restores before returning on the message thread.
The live tree keeps its identity while properties are replaced, preserving component bindings.

Snapshot publication defines ordering.
A later restore replaces the complete pending snapshot.
A later UI property callback patches only that property, preserving unrelated settings from a queued restore.
No-op assignments with no ValueTree notification do not override pending values.
Restore-generated callbacks neither mark the host dirty nor republish intermediate tree properties.
Ordinary user property changes publish the snapshot and notify the host of non-parameter state changes.
The host must finish all in-flight state, editor, async, and audio callbacks before destroying the processor.

## Validation and performance boundaries

Tests exercise the ordinary processor and components, not a separate DSP model.
Independent direct convolution expectations, analytical signals, exact neutral output checks,
and host lifecycle tests establish different parts of the contract.
The suites cover arbitrary blocks and sample rates, endpoint tails, Mono/Delta/bypass interactions,
correlation, FIFO wrap and generation changes, interrupted gestures,
state migration, and concurrent host state callbacks.
Accessibility checks use native component peers, without screenshot assertions.
[CrossoverTests.cpp](tests/CrossoverTests.cpp) checks quadrature taps against independent analytical expectations.
It uses sine sums and cotangent impulses.
It also measures endpoint zeros, sampled response, and complementary recovery bands.
[RecoveryTests.cpp](tests/RecoveryTests.cpp) checks analytical signal laws, source trims, arbitrary blocks, ramps,
warm activation, crossover handoffs, reset, finite tails, polarity, and adversarial cancellation.
Host and control tests cover parameter indices, old-state defaults, headless real-time/offline processing,
Post analysis, independent gestures, and initialization from the first FIR pre-ring.
These numerical contracts do not establish the subjective sound of recovered reverb or transient detail.

`cmake/Verification.cmake` adds plugin-specific sanitizer or source-coverage instrumentation.
CI runs Debug and Release builds and tests on macOS and Windows, pinned pluginval validation,
macOS AU validation, clang-tidy, AddressSanitizer, UndefinedBehaviorSanitizer,
ThreadSanitizer, RealtimeSanitizer, and coverage. It runs no benchmarks.
Source coverage does not establish GPU correctness or packaged-host integration.
The current unit environment exercises little of the renderer's actual GPU drawing path.

`ImageControlBenchmark` measures full callbacks and isolated imager, analysis, particle, and overlay costs.
It varies rates, block sizes, processing modes, correlation, analysis load, and paced crossover requests.
The baseline workloads explicitly set Width 1 and Recover Off.
Matched narrowed workloads compare recovery Off, selected bands, and all bands,
with correlation and analysis variants and paced moving-crossovers cases.
Direct imager probes cover centered/panned workloads and the same recovery comparisons.
An isolated batched setter probe measures unchanged and changing `setWidths`/`setRecoveries` pairs,
excluding the untimed processing used to advance ramps and observe results.
Separate analyzer and captured-frame particle probes isolate worker-side costs.
Hovered overlay probes exercise response geometry,
while `--focused` selects those probes and small Solo/Delta callbacks.
These probes are available for local use only.
They are observations, not performance improvement evidence without a matched baseline.
`test-signals/generate.py --group listening` supplies deterministic synthetic decays, pure-Side sweeps/combs,
phase/adversarial pairs of both signs, and left/right hard-panned transients with bass for manual comparisons.
Those generated inputs do not establish that any listening evaluation has taken place.
Reported p50/p95/p99/max values describe observed callback work, not an audio-device scheduling guarantee.
GPU drawing, native compositor work, and physical device deadlines are outside those CPU measurements.
The VST3/AU comparison benchmark instead measures end-to-end CLI rendering and validates every 16-bit PCM output.
It cannot establish float-level output identity or isolate DSP cost from host and file I/O.

Real-time plugin code avoids application heap allocation and kernel destruction during normal callbacks,
but third-party wrappers and FFT implementations require separate inspection and validation.
An additional real-time checker has observed first-use thread-local allocation in the JUCE VST3 automation wrapper.
Passing throughput tests or wrapper validation must not be described as proof of allocation-free automation.

`sanitize.sh --realtime` startup-links the selected Homebrew LLVM RTSan runtime into a dedicated test host.
Direct processing and built VST3 probes check first callbacks on fresh threads, without unmarked processing warmup.
Preparation, test coordination, assertions, and intentional offline kernel design stay outside marked regions.
The host verifies one loaded RTSan runtime before checking the exact build-tree bundle.
Clean callback cases reject every detected violation.
Known host/wrapper findings are explicit `BLOCKED` audit contracts,
matching unsafe call names and callback stacks rather than suppressing those calls.
Per-case stdout, stderr, and outcome records remain under `cmake-build-mac-rtsan/realtime-artifacts/`.
The checks detect exercised intercepted calls, not deadline compliance,
arbitrary plugin safety, or every atomic spin loop.
