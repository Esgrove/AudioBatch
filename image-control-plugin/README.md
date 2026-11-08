# ImageControl

A multiband stereo imaging effect built with JUCE.
VST3 for Windows and MacOS, and Audio Unit for macOS.
It splits the signal into four frequency bands and sets the mid and side level and the pan of each band separately,
so the stereo image can be narrowed, widened, rebalanced, or moved to one side in specific frequency ranges.
A spatial spectrogram shows where the energy of each frequency sits in the stereo field.

The plugin lives in the AudioBatch repository and is mainly used from AudioBatch's plugin chain,
but it is a standalone plugin that works in any VST3 host on macOS and Windows, and in any Audio Unit host on macOS.

## Parameters

| Id                           | Name                    | Range                        | Default |
|------------------------------|-------------------------|------------------------------|---------|
| `crossover_low`              | Low Crossover           | 20 Hz to 20 kHz, logarithmic | 100 Hz  |
| `crossover_mid`              | Mid Crossover           | 20 Hz to 20 kHz, logarithmic | 500 Hz  |
| `crossover_high`             | High Crossover          | 20 Hz to 20 kHz, logarithmic | 5 kHz   |
| `band1_mid` to `band4_mid`   | Low: Mid to High: Mid   | -60 dB (off) to +12 dB       | 0 dB    |
| `band1_side` to `band4_side` | Low: Side to High: Side | -60 dB (off) to +12 dB       | 0 dB    |
| `band1_pan` to `band4_pan`   | Low: Pan to High: Pan   | 100L to 100R                 | C       |
| `output_gain`                | Output Gain             | -24 dB to +12 dB             | 0 dB    |
| `bypass`                     | Bypass                  | off or on                    | off     |

The bands are named Low, Low Mid, High Mid, and High,
so the parameters are named Low: Mid, Low Mid: Side, High: Pan, and so on.
The pans step in whole percent, and hosts can type them as `C`, `35L`, `35R`, or a signed percent.

The bands are below the low crossover, between the low and mid crossovers,
between the mid and high crossovers, and above the high crossover.
The crossovers always stay in order and at least a third of an octave apart.
The processing and the display both apply that rule, so dragging one crossover into another stops at the limit.

## Processing

The left and right channels are split into mid, (L + R) / 2, and side, (L − R) / 2.
Both go through three linear-phase lowpass filters, one per crossover.
Each crossover fades from one band to the next over one octave,
with a raised-cosine step on a logarithmic frequency axis.
The bands are built as differences:
the lowest lowpass, the differences between neighbouring lowpasses,
and the delayed signal minus the highest lowpass.
So the four bands always add back up to the delayed input, whatever the crossover frequencies are.

Each band's mid and side are scaled by their gains and joined back into L = M + S and R = M − S.
Each band is then panned, after its gains,
so Width shapes the band's image first and Pan places the result, and a band made mono can still sit to one side.
The pan moves the band toward one channel without discarding the other.
Panning right by p, from 0 to 1, gives L' = (1 − p) · L and R' = R + p · L,
and panning left mirrors that, so at 100L or 100R the whole band is in one channel.
For mono content this is the linear pan law, which keeps L' + R' constant,
so content the channels share peaks up to 6 dB higher in the one channel when panned fully to it.
The panned bands are summed, and the output gain scales the result.
Gain and pan changes glide over 20 ms to avoid zipper noise.
A mono input is processed as mid only, and its pans have no effect.

At the default settings, every gain is exactly unity and every pan exactly centred,
and the output is the input delayed by the latency, bit for bit.
A plugin left at its defaults in an AudioBatch chain leaves the audio unchanged.

Bypass glides every gain to unity and every pan to the centre instead of skipping the processing,
so the latency stays the same and the output becomes the delayed input bit for bit, without a click.
Hosts use the bypass parameter as the plugin's own bypass.

### Latency

The filters are 8193 taps long at 44.1 and 48 kHz, and twice as long for each doubling of the sample rate above that.
They are applied with uniformly partitioned FFT convolution in 256-sample partitions.
The latency is half the filter length plus one partition:
4352 samples at 44.1 and 48 kHz, about 90 ms at 48 kHz.
The plugin reports it, so hosts that compensate latency, such as AudioBatch, keep the audio in place.
The plugin reports no tail.
Past the end of the input, the filters only ring at the level of their stopband.

### Moving crossovers

New filters are designed off the audio thread and swapped in with a crossfade over one partition,
so moving a crossover does not click.
During offline rendering the new filters are designed before the next block is processed,
so the output never depends on thread timing.

## Editor

The header holds the Bypass button, the Settings button, and the output gain.
The spatial spectrogram fills the middle, and the band strips sit below it.
A divider between them appears when the pointer is over the gap,
and dragging it makes the band section taller or shorter.
The band section height is saved with the plugin state.

### Spatial spectrogram

Frequency runs from 20 Hz to 20 kHz left to right,
and the stereo position from full left (L) at the top through the centre (C) to full right (R) at the bottom.
The display follows the approach of particle-based spatial analyzers:
every analyzed frequency bin is drawn as a sharp particle with a faint halo,
new particles add to the picture, older ones fade, and dense areas merge into bright clusters with a soft glow.

- **Position:** the level difference between the channels in dB divided by the focus, clamped to the edges.
  The default focus of 18 dB is about where a source sounds fully to one side.
- **Colour:** frequency colours run from red in the bass through orange, yellow, and green to blue in the highs.
  Phase colours instead run from blue for in-phase content to red for anti-phase content.
  Anti-phase content has equal levels in both channels, so it sits in the centre like mono content,
  and the phase colours tell the two apart.
- **Brightness:** the level in dB over a 72 dB range through a power curve,
  so quiet content shows as faint dots and the noise floor stays dark.
  A new particle is bright from its first frame, and steady content builds up over the persistence.
  The tone curve keeps the hue of bright colours, and only the densest clusters lighten towards white.
- **Size:** particles keep the same size in logical pixels on every window size and display scale.
  At low frequencies, where the bins are wide on the axis, they widen to join into a continuous trace.
- **Resolution:** the analysis runs four transform lengths side by side.
  32768 samples cover 20 to 80 Hz with bins about 0.7 Hz apart after zero padding,
  8192 samples cover up to 500 Hz, 2048 samples up to 4 kHz, and 1024 samples the rest,
  so the bass is finely resolved and the highs stay sharp in time.
- **Refresh:** the editor refreshes on every vertical blank of the display through `juce::VBlankAttachment`,
  at the native refresh rate, such as 120 or 144 Hz.
  The fade advances by the time that passed, so it looks the same at any refresh rate.
- **Threads:** a background worker thread drains the audio queue, runs the analysis, and builds the particles,
  woken by the editor on every display refresh and every 50 ms otherwise.
  The message thread only animates and paints the overlays and handles the controls,
  so a busy analysis never delays a drag or a hover.
  One pass takes at most a queue's worth of audio,
  so a host that processes faster than real time, such as an offline render, cannot keep the worker busy forever.
- **Rendering:** the particles are drawn on the GPU with OpenGL on both macOS and Windows,
  through a `juce::OpenGLContext` attached to the editor.
  Each frame fades a float framebuffer at the plot's full physical resolution,
  adds the new particles as instanced Gaussian sprites with additive blending,
  builds the glow from a five-level downsample and upsample chain,
  and tone maps the result into the plot with rounded corners and a little dither against banding.
  JUCE paints the grid, the bands, and the controls over the GL picture, repainting only the regions that change,
  so the cost hardly grows with the window size.
  The frame rate readout counts the frames drawn, and Debug builds add the GPU time per frame.
  All the OpenGL code is in `SpectrogramRenderer`, so another backend, such as Metal, could replace it.
  Without OpenGL 3.3 the plot says that GPU rendering is unavailable, and the controls keep working.

The three crossovers are vertical lines with their frequencies.
Drag a line to move its crossover, and double-click it to reset it.
Hovering a band highlights it and shows its width.
Dragging a band left or right moves it:
a middle band moves both its crossovers and keeps its width in octaves,
and the Low and High bands move only their inner crossover.
The move stops where a crossover would come closer than a third of an octave to one that stays.
Dragging a band up or down with Ctrl, or Cmd on macOS, changes its width,
and so does the mouse wheel over it, both finely with Shift.
The cursor shows what a drag will do, and every drag is one host gesture.
Hovering a band also draws the response of every band as a curve in the band's colour,
the hovered band bright with a soft fill and the others faint, so their overlap at the crossovers shows.
The crossover lines are faint at rest, and glow brighter and thicker while hovered or dragged.

Every control has a tooltip, which fades and slides in once the pointer rests on it for about half a second,
and fades out when the pointer moves on.
The editor draws the tooltips itself, inside its window, instead of JUCE's tooltip window.
Over the spectrogram, the tooltip explains what a drag at the pointer does.

### Settings

The Settings button opens the display settings:

- whether the spectrogram shows the output or the input of the plugin,
- frequency or phase colours,
- a logarithmic or mel frequency scale, with the same HTK mel definition as AudioBatch's spectrogram,
- the persistence, how long particles take to fade,
- the focus, the level difference that reaches the edges,
- a frame rate readout in the top-left corner of the spectrogram,
- **Bypass rendering**, which stops analyzing and drawing the spectrogram while the audio is still processed,
- and **Hide the spectrogram**, which shrinks the window to the header and the band strips.
  Showing it again restores the previous height.

### Band strips

The controls use AudioBatch's palette and its Roboto and Roboto Mono fonts from the shared `../assets/`.
Each band has a strip with its own controls:

- **Mid** and **Side** are full-width sliders that set the gain of the band's mid and side signals.
  These are the parameters the host automates and saves.
- **Width** is the side gain relative to the mid gain as a linear ratio.
  0 makes the band mono, 1 leaves the stereo image as it is, and 2 doubles the side relative to the mid.
  Moving Width sets Side to Mid plus 20 · log10(width) dB, and width 0 turns Side off.
  Width is an editor control, not a parameter, so automation never has two values fighting over the side gain.
  It cannot be set while Mid is off.
- **Link** makes dragging Mid move Side by the same amount, which keeps the width while changing the level of the band.
- **Pan** moves the band toward the left or right channel after its gains, as described under Processing.
  It is a parameter, and double-clicking it centres the band.

Sliders drag relative to where they are grabbed, drag finely with Shift, and reset on double-click.
Once clicked, the arrow keys nudge them, finely with Shift.

The Link toggles, the display settings, and the editor size are saved with the plugin state.

## Building

The plugin builds with the JUCE submodule in `../cpp/JUCE`, CMake 4.2, and a C++23 compiler,
with a minimum deployment target of macOS 15.

```shell
./build.sh
```

The script builds the Release VST3 into `build/`, and on macOS the Audio Unit next to it,
and prints the VST3 path, which `--print` prints without building.
`--debug` builds Debug into `build-debug/` instead.
`--test` also builds and runs the unit tests.
`--install` copies the VST3 into the standard VST3 folder,
`~/Library/Audio/Plug-Ins/VST3` on macOS and `C:\Program Files\Common Files\VST3` on Windows,
which triggers a UAC prompt when the folder is not writable,
and on macOS the Audio Unit into `~/Library/Audio/Plug-Ins/Components`, restarting the Audio Unit registry.
`--validate` installs and then validates the Audio Unit with `auval`.
The Audio Unit is built from the same code as the VST3, with no format-specific code in the plugin.

The build includes `../cpp/cmake/Optimizations.cmake`, so it targets the same CPUs as AudioBatch:
the host architecture on macOS, which is arm64 with NEON on Apple Silicon, and an AVX2 baseline on Windows.
Release builds use link-time optimization and fuse multiply-adds,
and local Release builds also target the local CPU, which CI never does.
The hot loops are written for that SIMD width:
the convolver keeps its spectra split into real and imaginary parts, so its multiply-add is lane by lane.
On an Apple Silicon Mac these took the audio processing from about 4.2 to 3.5 ms per second of stereo audio.

The sources compile with strict warnings as errors.
`./lint.sh` checks them with clang-tidy using the AudioBatch C++ configuration,
and `--check` makes any warning fail the run.

Like AudioBatch, the build enables standard library hardening:
the fast libc++ mode in Release and the extensive one in Debug on macOS, and `_MSVC_STL_HARDENING` on Windows.
An out-of-range container access then stops the host at the bug instead of silently corrupting its memory.
`-DIMAGE_CONTROL_HARDENING=OFF` turns it off.

`ImageControlBenchmark`, built with the tests, times the audio processing per second of stereo audio
and the CPU side of one spectrogram refresh:
the analysis and the particles on the worker thread, and the overlay paint on the message thread.
The GPU drawing needs an OpenGL context, so the benchmark does not time it.
Build it in Release, once with and once without hardening, to see what the checks cost.
On an Apple Silicon Mac, hardening adds about 3% to the audio processing.
The convolver's inner loop keeps its spans the same length and its indices provably in range,
so the compiler drops the checks there and still vectorizes it.
`../cpp/format.sh` formats the sources, the CMake file, and the scripts, and the root `../format.sh` runs it too.
The `ImageControl` workflow in `../.github/workflows/image-control.yml` builds, tests, and lints the plugin on macOS
and Windows, and validates the Audio Unit on macOS.

### Comparing the VST3 and the Audio Unit

`benchmarks/compare_formats.py` renders the same files through both formats with the AudioBatch CLI on macOS:

```shell
uv run fixtures/benchmark/generate.py
image-control-plugin/build.sh --validate
uv run --script image-control-plugin/benchmarks/compare_formats.py
```

It builds `ImageControlPresetWriter`, which hosts the installed VST3 and Audio Unit through JUCE as AudioBatch does,
sets the same non-neutral settings on both, and writes an AudioBatch chain preset for each.
It then renders three 60-second stereo files from `../fixtures/benchmark/output` through each preset.
Each run uses `audiobatch process` with isolated application storage, and the formats alternate over several runs.
Last, it compares the 16-bit PCM the two formats wrote, sample by sample.
`--cli` picks the AudioBatch CLI, `--runs` the number of runs, and `--jobs` how many files render at once.
On an Apple Silicon Mac both formats rendered at about 58 times real time,
with bit-identical output.

## Layout

- `src/Parameters.*` holds the parameter ids, ranges, and defaults,
  the crossover ordering rule, and the editor setting names.
- `src/ImageControlProcessor.*` is the audio processor, and `src/ImageControlEditor.*` the editor.
- `src/dsp/` holds the processing:
  `CrossoverDesign.*` designs the lowpass filters,
  `PartitionedConvolver.*` applies them,
  and `MultibandImager.*` splits, scales, and joins the bands.
- `src/analysis/` holds the multi-resolution spatial analysis in `SpatialAnalyzer.*`,
  and `StereoSampleFifo.h`, which carries audio from the audio thread to the editor.
- `src/ui/` holds the editor parts:
  `SpatialSpectrogramView.*` paints the overlays and handles the mouse,
  `SpectrogramParticles.*` turns analyzed bins into particles and holds the frequency scales and colours,
  `SpectrogramWorker.*` runs the analysis and builds the particles on a background thread,
  `SpectrogramRenderer.*` draws the particles with OpenGL,
  `BandStrip.*` the controls of one band, `SettingsPanel.*` the display settings,
  `ControlSlider.*` the slider with fine dragging and key nudges,
  and `ImageControlLookAndFeel.*` the AudioBatch palette and fonts.
- `tests/` holds the GoogleTest unit tests.
- `benchmarks/` holds `Benchmark.cpp`, which times the processing and the spectrogram,
  `PresetWriter.cpp`, which writes AudioBatch presets for both formats,
  and `compare_formats.py`, which renders and compares the formats through AudioBatch.

## Future Work

- Add a Metal backend for `SpectrogramRenderer` on macOS if Apple removes OpenGL.
  [JUCE Metal rendering on macOS](https://github.com/UVI-Dev/JUCE/tree/metal-renderer)
