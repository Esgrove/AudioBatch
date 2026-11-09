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

## Ownership and execution threads

The host owns `ImageControlProcessor` through JUCE's plugin wrapper.
The processor owns APVTS, the message-thread editor-settings tree, `MultibandImager`, and `StereoSampleFifo`.
Raw atomic parameter pointers refer into APVTS and remain valid for the processor lifetime.
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

APVTS owns three crossover parameters and each band's Mid, Side, Pan, and Solo parameters,
plus output gain, Bypass, Delta, and Mono.
Version hints distinguish parameter generations for hosts that use ordering hints, including Audio Unit hosts.
Existing IDs and version hints are persistence and automation contracts, not refactoring details.

The private `FiniteParameter` implementation preserves the declared ranges and host metadata.
It rejects non-finite direct values and invalid text while retaining the previous value.
Text conversion understands Hz/kHz, dB, signed pan percentages, `C`, and `L`/`R` suffixes.
The JUCE fork also rejects non-finite notification values before broadcasting them to hosts or attachments.
Rejecting only `setValue` would be insufficient because `setValueNotifyingHost` has a separate listener broadcast.

Crossovers are ordered and separated by a minimum one-third-octave ratio.
The processor, DSP, and editor use the same ordering helper.
Host parameters are sampled by block, and DSP changes take effect at convolution partition boundaries with smoothing.
The plugin does not implement a separate sample-accurate VST3 parameter-queue engine.

Width is an editor macro, not a second parameter competing with Side automation.
It expresses Side gain relative to Mid, and editing it writes the Side parameter.
The normal track spans 0 to 2, but the numeric and accessible values retain larger ratios caused by independent gains.
Link is persisted editor behavior that moves Side with a user Mid gesture, not with incoming host Mid automation.
Gesture ownership is captured at pointer-down so Link changes or teardown cannot unbalance begin/end notifications.

## Audio signal flow

`processBlock` and `processBlockBypassed` reach the same `processWithBypass` path.
`ScopedNoDenormals` protects the DSP callback from denormal slowdowns.
The callback reads parameter atomics, converts dB and percentages, and supplies targets to the imager.

For stereo input, the imaging representation is
\[
M = (L + R) / 2, \qquad S = (L - R) / 2.
\]
Each signal is divided into four complementary bands.
Band Mid and Side gains are applied independently, followed by reconstruction into left and right,
then per-band Pan and Solo contribution, summation, and output gain.
Mono input uses the Mid path and ignores Pan.
Only matching mono-to-mono and stereo-to-stereo main bus layouts are supported.

Pan transfers signal rather than simply attenuating one channel.
For a rightward pan amount \(p\) between 0 and 1,
\[
L' = (1-p)L, \qquad R' = R + pL.
\]
Leftward pan mirrors this operation.
A fully panned shared signal can therefore peak 6 dB higher in the receiving channel.
The processor neither limits nor clips floating-point output to prevent that boost.

Gain, Pan, Solo, output gain, Delta, and Mono transitions use linear 20 ms ramps.
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

### Partitioned convolution and latency

[`PartitionedConvolver.cpp`](src/dsp/PartitionedConvolver.cpp) implements uniform overlap-save convolution.
The processing partition \(P\) is 256 samples and the transform size is \(2P=512\).
Each kernel partition is zero-padded and transformed once during kernel design.
Each Mid or Side input partition is transformed together with the previous partition and retained in a spectral ring.
All three filters reuse that input transform and history.

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
It builds new `KernelSpectra` in the inactive slot and publishes `switchPending` with release semantics.

At a processing partition boundary, the audio thread acquires the publication,
convolves the same input history with both kernel slots,
and crossfades their outputs over one partition.
After both Mid and Side finish, it publishes the active slot and releases the old slot for later redesign.
The callback swaps indices rather than moving or destroying allocated spectra.

With `isNonRealtime()` set by the host, the calling processing thread designs moved crossovers synchronously.
A mutex serializes that work against any design already in flight, and the background designer stands aside.
A pending second request is designed after the previous slot switch.
This makes offline results independent of worker scheduling, but deliberately allows allocation and locking offline.
It is not a real-time-safe mode switch policy for a host that mislabels its callbacks.

### Transparency, bypass, Delta, and Mono

Once neutral controls settle, the imager copies delayed original channel samples directly.
It still advances convolution history, including during kernel replacements,
so later processing changes have a warm filter history.
Exact neutral Delta clears the output partition rather than reconstructing a nearly zero difference.
These shortcuts also operate during crossover crossfades.

Bypass supplies unity gains, centered Pans, all bands audible, and Delta off through the same ramps.
It does not skip processing or change latency.
Both the exposed host bypass parameter and `processBlockBypassed` use this path.

Delta subtracts an aligned dry reference from the gain-adjusted result.
With solos active, its dry reference contains only the selected bands.
Mono runs last in `ImageControlProcessor`, before the Post analyzer tap.
It smoothly folds the full processed output to \((L+R)/2\) in both channels, including after Delta or bypass.
The saved, automatable `mono` parameter has version hint 5 and defaults to off for states without it.
Its 20 ms ramp applies identically in real-time and offline callbacks.
Preparation and reset initialize the blend from the parameter so an enabled fold applies from the first output sample.
Pre analysis remains the unprocessed input.

## Per-band correlation

[`BandCorrelation`](src/dsp/BandCorrelation.h) measures each processed band's stereo signal after Mid/Side, Solo, and Pan,
before global Delta and Mono processing.
It reuses the existing lowpass outputs and copies control smoothers for observation,
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
The reference rises immediately and falls at 12 dB per second, with a -90 dB lower bound.
Contrast emphasizes stronger points and increases toward the highs.
Frame-duration weighting and persistence compensation keep tiers with different hops from drawing at arbitrary brightness.
Colors are either a frequency palette interpolated in linear light or a phase-coherence blend.
The resulting relative display is not an absolute output-level meter.

### Message-thread refresh and overlays

`ImageControlEditor` uses `juce::VBlankAttachment` rather than an always-running GL repaint loop.
Every refresh applies display settings, wakes analysis, advances hover/tooltip animations,
updates correlation components, submits plot geometry and elapsed fade time, and requests a GL frame.
Large refresh gaps are capped for visual animation and fade progression.

`SpatialSpectrogramView` paints axes, grids, band regions, response guides, crossover lines, and status text.
Its separate `SpatialSpectrogramViewInput.cpp` owns hit testing and parameter gestures.
The plot itself is left unpainted so the GPU image beneath the components remains visible.
`BandStrip` owns APVTS slider/button attachments and Width/Link gesture behavior.
The independent `BandCorrelationMeter` components occupy columns aligned with those strips.
Compact layout and row visibility preserve the saved expanded editor height.

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

`getStateInformation` writes JUCE binary XML containing the APVTS parameter tree and an `EditorState` child.
Editor properties include links, analysis mode, display preferences, correlation visibility, dimensions, and divider height.
Live analysis, meter history, and GPU contents are not persisted.
Mono is persisted with the other audio parameters.
Missing editor properties restore from deterministic defaults, independently of the instance's previous edits.
Missing audio parameters use their declared defaults through APVTS.

The live editor `ValueTree`, its retained copies, and bound `juce::Value` objects belong exclusively to the message thread.
Host state callbacks use a detached binary editor snapshot under `editorStateMutex`.
A separate recursive state mutex serializes host saves/restores and allows synchronous reentrant host save requests.
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

`cmake/Verification.cmake` adds plugin-specific sanitizer or source-coverage instrumentation.
CI runs Debug and Release on macOS and Windows, pinned pluginval validation,
macOS AU validation, clang-tidy, macOS sanitizers, coverage, and comparator tooling contracts.
Source coverage does not establish GPU correctness or packaged-host integration.
The current unit environment exercises little of the renderer's actual GPU drawing path.

`ImageControlBenchmark` separates full callback distributions from isolated imager, analysis, particle, and overlay costs.
It varies rates, block sizes, processing modes, correlation, analysis load, and paced crossover requests.
Reported p50/p95/p99/max values describe observed callback work, not an audio-device scheduling guarantee.
GPU drawing, native compositor work, and physical device deadlines are outside those CPU measurements.
The VST3/AU comparison benchmark instead measures end-to-end CLI rendering and validates every 16-bit PCM output.
It cannot establish float-level output identity or isolate DSP cost from host and file I/O.

Real-time plugin code avoids application heap allocation and kernel destruction during normal callbacks,
but third-party wrappers and FFT implementations require separate inspection and validation.
An additional real-time checker has observed first-use thread-local allocation in the JUCE VST3 automation wrapper.
Passing throughput tests or wrapper validation must not be described as proof of allocation-free automation.
