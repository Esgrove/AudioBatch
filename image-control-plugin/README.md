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
| `band1_solo` to `band4_solo` | Low: Solo to High: Solo | off or on                    | off     |
| `output_gain`                | Output Gain             | -24 dB to +12 dB             | 0 dB    |
| `bypass`                     | Bypass                  | off or on                    | off     |
| `delta`                      | Delta                   | off or on                    | off     |

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

Bypass glides every gain to unity, every pan to the centre, and every band muted by a solo back in,
instead of skipping the processing,
so the latency stays the same and the output becomes the delayed input bit for bit, without a click.
Hosts use the bypass parameter as the plugin's own bypass.

### Delta

Delta outputs only what the processing changes: the output minus the input, delayed by the latency to line up.
At the default settings it is exact silence, and narrowing one band leaves only that band's removed side.
It includes the output gain, so a raised output gain shows as the input scaled by the difference.
Turning it on or off glides over the same 20 ms as the gains, so it does not click.
Bypass turns Delta off, so a bypassed plugin always passes the input through.
While some bands are soloed, Delta subtracts only those bands of the input,
so it carries only what the processing changes in the soloed bands.

### Solo

Soloing a band mutes the bands that are not soloed, so only the soloed bands are heard.
Solos combine, so two or more bands can be heard together.
Soloing every band is the same as soloing none, but each band keeps its own solo,
so unsoloing some of them afterwards leaves the others soloed.
Soloing and unsoloing glide over the same 20 ms as the gains, so they do not click.
The solos are parameters, so hosts save and can automate them.
Bypass ignores them and passes every band through.
Without any band muted, the output is still the delayed input bit for bit at the default settings.

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

The header holds the Settings button and the Pre and Post switch on the left,
and the Delta and Bypass buttons and the output gain on the right.
Pre shows the input of the plugin in the spectrogram, and Post its output, which is the difference while Delta is on.
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

- **Position:** the level difference between the channels in dB through an S-curve set by two display settings.
  The pan midpoint, 12 dB by default, is drawn half way from the centre to the edge,
  and the pan edge, 32 dB by default, 99% of the way there,
  so only nearly fully panned sound reaches the top or the bottom.
  A difference of d dB is drawn at tanh((d / s)^p) of the way, towards the louder channel,
  where p = ln(atanh(0.99) / atanh(0.5)) / ln(edge / midpoint) and s = midpoint / atanh(0.5)^(1/p).
  The curve rises gently below the midpoint and flattens towards the edge,
  the more sharply the closer the two settings are.
  Sound in one channel only sits at that channel's edge.
  The level difference of a bin is measured over its neighbours within 1/32 octave, while its level is its own.
  Decorrelated sound, such as reverb or wide cymbals, has a random level difference in every bin,
  so the dense highs draw at their average position instead of spraying over the whole height,
  while a sine, whose bins share one position, stays where it is.
  Changing either setting draws the curve over the spectrogram for about three seconds before it fades:
  guide lines where 3, 6, 12, 18, 24, 36, and 48 dB land, the midpoint and edge highlighted,
  and the curve itself, with the level difference along the centre line.
- **Colour:** frequency colours run from red in the bass through yellow in the low mids, green around 1 kHz,
  and cyan in the presence range to blue in the highs and violet towards magenta in the air band.
  Phase colours instead run from blue for in-phase content to red for anti-phase content.
  Anti-phase content has equal levels in both channels, so it sits in the centre like mono content,
  and the phase colours tell the two apart.
- **Brightness:** the level in dB with a tilt of 3 dB per octave around 1 kHz,
  which evens out the falling spectrum of music so the highs show as clearly as the bass.
  An auto-scale reference follows the loudest tilted bin, rising at once and falling by 12 dB per second,
  and the 36 dB below it map to the brightness through an S-curve set by the Contrast setting,
  so quiet and loud material look alike, quiet content shows as faint dots, and the noise floor stays dark.
  A share s of the range draws at s^g / (s^g + (p · (1 − s) / (1 − p))^g),
  where a contrast c from 0 to 1 sets the steepness g = 1 + 3c and the pivot p = 0.5 + 0.2c, which draws half bright.
  At 0 the brightness follows the dB level linearly, and the default 60% darkens the quieter half and brightens the top.
  Above 1 kHz the contrast rises further, by up to 0.25 at 16 kHz on the logarithmic axis,
  so the strongest elements of the dense, noisy highs stand out from the rest.
  A new particle is bright from its first frame, and steady content builds up over the persistence.
  The particles add up in linear light, and the result is encoded as sRGB for the display.
  The tone curve keeps the hue of bright colours, and only the densest clusters lighten towards white.
- **Size:** brighter particles are larger, down to half the size for the faintest,
  and only bright particles get a halo, so faint ones draw as sharp dots instead of a haze.
  Sizes are in logical pixels, so they look the same on every window size and display scale.
  At low frequencies, where the bins are wide on the axis, they widen by up to 6 logical pixels to join into a trace.
- **Resolution:** the analysis runs six transform lengths side by side.
  32768 samples cover 20 to 80 Hz with bins about 0.7 Hz apart after zero padding,
  16384 samples cover up to 160 Hz, 8192 samples up to 320 Hz, 4096 samples up to 640 Hz,
  2048 samples up to 4 kHz, and 1024 samples the rest,
  so the bass is finely resolved and the highs stay sharp in time.
  Up to 640 Hz each length covers one octave and takes over where its length times the frequency is the same,
  so a sine's main lobe covers about a fifth of an octave throughout, without widening where a shorter window takes over.
  Both channels go through one complex transform, the left as the real part and the right as the imaginary part,
  which halves the transform work with JUCE's own FFT on Windows.
  The frames are transformed unwindowed, and the periodic Hann window and its time derivative are applied to the spectrum,
  as three-tap filters two bins apart, so one transform gives both.
  The derivative reassigns each bin to the instantaneous frequency of its energy,
  and the particle is drawn there, so every bin of a sine's main lobe lands on the sine's frequency.
  A sine then draws as a thin line at any window size, instead of a fifth of an octave wide,
  while its bins still give it the brightness of the whole lobe.
  A bin keeps its own frequency when the reassignment would move it outside the window's main lobe.
  On a Windows desktop, the analysis and the particles take about 0.57 ms of the worker thread per 120 Hz refresh.
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
  With **Solid layers** on, the composite draws the particles as stacked translucent solids instead.
  It reads the quarter-size level of the glow chain, a smooth field of the particles, with a bicubic sample.
  The field is copied before the smaller levels are added back into it,
  since their wide blurs would draw wide layers around a narrow mono line.
  The composite fills four layers where its tone-mapped density passes 6, 18, 38, and 62%,
  each more opaque and brighter than the one around it and tinted with the field's own colour,
  with a thin antialiased outline on each.
  Weak sound draws as faint, open shapes and strong sound as dense stacked solids,
  and the sharp particles show on top only where they are bright, so the main elements stay drawn as lines.
  It adds only that quarter-size copy, so switching costs almost nothing and keeps the picture.
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

- frequency or phase colours,
- a logarithmic or mel frequency scale, with the same HTK mel definition as AudioBatch's spectrogram,
- **Solid layers**, which draws the spectrogram as stacked translucent layers with outlines instead of particles,
- the persistence, how long particles take to fade,
- the contrast, from 0 to 100% in whole percent, how strongly loud sound stands out from quiet sound,
- the pan midpoint and the pan edge in whole dB, which shape the stereo position curve,
  and push each other along to stay at least 3 dB apart,
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

The Link toggles, the display settings, and the editor size are saved with the plugin state,
and a reopened editor comes back at its saved size.
The position of the window belongs to the host, and AudioBatch reopens each plugin window where it was.

## Building

The plugin builds with the JUCE submodule in `../cpp/JUCE`, CMake 4.2, and a C++23 compiler,
with a minimum deployment target of macOS 15.

```shell
./build.sh
```

The script builds the Release VST3 into `cmake-build-release/`, and on macOS the Audio Unit next to it,
and prints the VST3 path, which `--print` prints without building.
On Windows the default generator is Visual Studio, and `--ninja` builds with Ninja into `cmake-build-ninja-release/` instead.
`--debug` builds Debug into `cmake-build-debug/` instead, and `--build-type` selects any other CMake build type.
`--reuse` skips the CMake generation, and `--clean` deletes the build directory first.
`--test` also builds and runs the unit tests.

```shell
./install.sh
```

`install.sh` builds with `build.sh`, then copies the VST3 into the standard VST3 folder,
`~/Library/Audio/Plug-Ins/VST3` on macOS and `C:\Program Files\Common Files\VST3` on Windows,
which triggers a UAC prompt when the folder is not writable,
and on macOS the Audio Unit into `~/Library/Audio/Plug-Ins/Components`, restarting the Audio Unit registry.
It takes `--reuse`, `--clean`, and `--debug` like `build.sh`.
`--validate` then validates the VST3 with `pluginval` when it is in the path, and on macOS the Audio Unit with `auval`.
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
`--changed` and `--staged` lint only the changed sources, `--build-dir` selects the compile database,
`--jobs` sets the number of parallel clang-tidy processes, and arguments after `--` go to clang-tidy.
The script first builds the Debug tests and benchmark, which configures the build directory when it is missing,
regenerates the compile database when the CMake files changed, and generates the JUCE headers.
On Windows it uses the Ninja workspace in `cmake-build-ninja-debug/`, since Visual Studio writes no compile database.

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
image-control-plugin/install.sh --validate
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

### Test signals

`test-signals/generate.py` writes stereo test signals for checking the spectrogram by eye into `test-signals/output/`,
which git ignores:

```shell
uv run --script image-control-plugin/test-signals/generate.py
```

They are 10-second 48 kHz float WAV files:
1 kHz sines at the centre, at 6, 12, 20, and 32 dB to either side, and in one channel only,
and one file stepping through all of those pans,
90 Hz sines and a logarithmic sweep that show how wide a sine draws in the bass and across the range,
octave combs of sines from 40 Hz to 10240 Hz in several pan patterns,
and white and pink noise at the centre, uncorrelated, in one channel, 12 dB to one side, and in anti-phase.
A pan in the file names is the level difference between the channels, so the files show the position curve directly.
The unit tests check the same kinds of signals against the analyzer.

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
- `test-signals/` holds `generate.py`, which writes the test signals for checking the spectrogram by eye.

## Future Work

- Add a Metal backend for `SpectrogramRenderer` on macOS if Apple removes OpenGL.
  [JUCE Metal rendering on macOS](https://github.com/UVI-Dev/JUCE/tree/metal-renderer)
