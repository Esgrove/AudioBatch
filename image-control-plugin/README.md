# ImageControl

A multiband stereo imaging VST3 effect by esgrove, built with JUCE.
It splits the signal into four frequency bands and sets the mid and side level of each band separately,
so the stereo image can be narrowed, widened, or rebalanced in one frequency range without touching the others.
A spatial spectrogram shows where the energy of each frequency sits in the stereo field.

The plugin lives in the AudioBatch repository for now and is mainly used from AudioBatch's plugin chain,
but it is a standalone plugin that works in any VST3 host on macOS and Windows.

## Parameters

| Id                           | Name                     | Range                        | Default |
| ---------------------------- | ------------------------ | ---------------------------- | ------- |
| `crossover_low`              | Low Crossover            | 20 Hz to 20 kHz, logarithmic | 100 Hz  |
| `crossover_mid`              | Mid Crossover            | 20 Hz to 20 kHz, logarithmic | 500 Hz  |
| `crossover_high`             | High Crossover           | 20 Hz to 20 kHz, logarithmic | 5 kHz   |
| `band1_mid` to `band4_mid`   | Low: Mid to High: Mid    | -60 dB (off) to +12 dB       | 0 dB    |
| `band1_side` to `band4_side` | Low: Side to High: Side  | -60 dB (off) to +12 dB       | 0 dB    |
| `output_gain`                | Output Gain              | -24 dB to +12 dB             | 0 dB    |
| `bypass`                     | Bypass                   | off or on                    | off     |

The bands are named Low, Low Mid, High Mid, and High,
so the parameters are named Low: Mid, Low Mid: Side, and so on.

The bands are below the low crossover, between the low and mid crossovers,
between the mid and high crossovers, and above the high crossover.
The crossovers always stay in order and at least a third of an octave apart.
The processing and the display both apply that rule, so dragging one crossover into another stops at the limit.

## Processing

The left and right channels are split into mid, (L + R) / 2, and side, (L − R) / 2.
Both go through three linear-phase lowpass filters, one per crossover.
Each crossover fades from one band to the next over one octave, with a raised-cosine step on a logarithmic frequency axis.
The bands are built as differences:
the lowest lowpass, the differences between neighbouring lowpasses,
and the delayed signal minus the highest lowpass.
So the four bands always add back up to the delayed input, whatever the crossover frequencies are.

Each band's mid and side are scaled by their gains, summed, and joined back into L = M + S and R = M − S.
Gain changes glide over 20 ms to avoid zipper noise.
A mono input is processed as mid only.

At the default settings, every gain is exactly unity,
and the output is the input delayed by the latency, bit for bit.
A plugin left at its defaults in an AudioBatch chain leaves the audio unchanged.

Bypass glides every gain to unity instead of skipping the processing,
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

The spatial spectrogram fills the top of the editor.
Frequency runs from 20 Hz to 20 kHz left to right on a logarithmic axis,
and the stereo position runs from full left (L, +1) at the top through the centre (C, 0) to full right (R, −1) at the bottom.
The position of each frequency bin is p = (4 / π) · atan2(|L|, |R|) − 1.
Brightness shows energy over a 60 dB range.
Colour shows the phase coherence between the channels:
blue to white for in-phase content, and red to pink for anti-phase content.
Anti-phase content has equal levels in both channels, so it sits in the centre like mono content,
and the colour is what tells the two apart.

The three crossovers are vertical lines on the spectrogram with their frequencies.
Drag a line to move its crossover.
The header switches the display between the input and the output of the plugin,
sets how long the trace persists, and sets the output gain.

The editor refreshes on every vertical blank of the display through `juce::VBlankAttachment`,
so it follows the native refresh rate, such as 120 or 144 Hz.
It renders the spectrogram image again only when a new analysis frame arrives,
about 47 times a second at 48 kHz, and repaints the crossover lines only when they move.

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

Sliders drag relative to where they are grabbed, drag finely with Shift, and reset on double-click.
Once clicked, the arrow keys nudge them, finely with Shift.

The Link toggles, the display settings, and the editor size are saved with the plugin state.

## Building

The plugin builds with the JUCE submodule in `../cpp/JUCE`, CMake 4.2, and a C++23 compiler,
with a minimum deployment target of macOS 15.

```shell
./build.sh
```

The script builds the Release bundle into `build/` and prints its path, which `--print` prints without building.
`--debug` builds Debug into `build-debug/` instead.
`--test` also builds and runs the unit tests.
`--install` copies the bundle into the user's VST3 folder,
`~/Library/Audio/Plug-Ins/VST3` on macOS and `%LOCALAPPDATA%\Programs\Common\VST3` on Windows.

The sources compile with strict warnings as errors.
`./lint.sh` checks them with clang-tidy using the AudioBatch C++ configuration,
and `--check` makes any warning fail the run.
`../cpp/format.sh` formats the sources, the CMake file, and the scripts, and the root `../format.sh` runs it too.
The `ImageControl` workflow in `../.github/workflows/image-control.yml` builds, tests, and lints the plugin on macOS
and Windows.

## Layout

- `src/Parameters.*` holds the parameter ids, ranges, and defaults,
  the crossover ordering rule, and the editor setting names.
- `src/ImageControlProcessor.*` is the audio processor, and `src/ImageControlEditor.*` the editor.
- `src/dsp/` holds the processing:
  `CrossoverDesign.*` designs the lowpass filters,
  `PartitionedConvolver.*` applies them,
  and `MultibandImager.*` splits, scales, and joins the bands.
- `src/analysis/` holds the spatial spectrogram analysis in `SpatialAnalyzer.*`,
  and `StereoSampleFifo.h`, which carries audio from the audio thread to the editor.
- `src/ui/` holds the spectrogram view and the band strips.
- `tests/` holds the GoogleTest unit tests.
