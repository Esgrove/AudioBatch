# Serato Overview Color Validation

## Corrected Interpretation

The initial sixteen-frequency-band interpretation was incorrect for the tested `01 05` payloads.
The tag contains a small indexed-color raster, commonly 240 time columns with 16 vertical cells per column.
Treating the row number as a frequency band made the occupied central rows dominate green on unrelated tracks.
Changing low/mid/high weighting would hide that decoding error instead of fixing it.

Matching source tags and user-supplied Serato overview screenshots support a zero-based 6x6x6 RGB cube.
For a foreground palette index, integer arithmetic gives:

```text
red   = (index / 36) * 51
green = ((index % 36) / 6) * 51
blue  = (index % 6) * 51
```

Zero is black. The ubiquitous index 1 is treated as background for the combined color field.
Indices 216 through 255 are reserved or unknown and do not contribute.
An observed reserved entry coincided with neutral screenshot pixels,
but that does not establish its exact gray level or the remaining reserved palette.
These entries are not assigned guessed colors.

## Reference Evidence

On 2026-10-13, local read-only comparisons used the supplied AIFF tracks and full-track overview screenshots.
No audio, payload dumps or screenshot assets are part of the portable test suite.
The screenshots' Serato version, display scale and selected color mode were not supplied.
The narrow overview panes were compared separately from the detailed and zoomed waveform panes.

The zero-based RGB interpretation ranked first among tested offsets, channel permutations and component reversals
on each of the three AIFF references.
Normalized RGB comparison used foreground screenshot pixels, excluding dark and near-neutral pixels.
Nominal whole-track alignment gave the following chromaticity root-mean-square errors:

| Reference | Zero-based RGB | Offset by one | Offset by sixteen |
| --- | --- | --- | --- |
| Adrians Mystery | 0.0785 | 0.1749 | 0.2989 |
| Reality Prelude | 0.0825 | 0.1836 | 0.2825 |
| Your Good Loving | 0.0530 | 0.1338 | 0.2291 |

Exact screenshot matches included index 121 as RGB (153, 102, 51) and index 115 as RGB (153, 51, 51).
Localized blue and pink regions also agreed with the RGB cube after accounting for reduced screenshot brightness.
Temporal shifts worsened the comparison, supporting the whole-track time-column interpretation.
These measurements establish a much better decoding hypothesis,
not an exact reproduction of Serato's layered rendering or a universal compatibility claim.

## AudioBatch Presentation

AudioBatch averages the decoded foreground RGB cells into one opaque color per time column.
It decodes indices before interpolating adjacent columns or combining time-overlapping columns.
Interpolating numeric indices would produce unrelated colors at palette-plane boundaries.
Background-only or unknown-only data uses the existing opaque blue fallback.

JUCE still supplies every signed peak, channel strip, gain adjustment and close-zoom waveform detail.
The same horizontal color field applies to both stereo strips.
The tag's own vertical raster does not replace JUCE's shape.
Cached raw bytes remain compatible because the parser and serialization have not changed.

This combines the small Serato overview's colors with a more detailed JUCE waveform.
It cannot reproduce the individual-hit color changes visible in Serato's zoomed detailed pane.
That pane needs finer analysis data than the small embedded Overview raster contains.
The source and storage of Serato's detailed analysis have not been established by these comparisons.
No additional spectral analysis or guessed per-hit color is introduced.

## Verification

- Windows debug GUI, CLI and test targets built successfully.
- CTest passed the complete portable suite, including palette decoding, cell weighting and decoded-color interpolation.
- Synthetic tests cover palette boundaries, all sixteen row positions, background/reserved fallback and viewport projection.
- Offscreen rendering tests preserve JUCE pixel coverage across mono/stereo, gain, zoom/pan and 100/150/200% scales.
- An ignored local diagnostic exercised production reader, cache, gradient and waveform rendering APIs on seven tracks.
- All seven tags contained 240 columns. Fresh and reopened-cache bytes and renders matched.
- The local full-track renders preserved waveform coverage at 100/150/200% scale and showed track-dependent colors.
- SHA-256 checks confirmed all seven tracks and sixteen reference screenshots remained unchanged.

The local diagnostic and its images are development artifacts only and are not required by committed tests.
Live GUI playback and selection were not exercised by that diagnostic.
macOS validation and the reserved palette remain pending.
Repository-wide `./lint.sh --check` reported 27 findings outside the corrected palette implementation and its tests.
They include existing code and Windows SDK diagnostics, plus new tag-reader and cache-test diagnostics.
The feature is therefore not claimed to have a clean lint sign-off.

## Superseded Assumptions

The original planning document described sixteen frequency strengths and direct gradient-brush rendering.
Both assumptions were revised after executable checks and reference comparisons.
The palette interpretation above replaces the frequency-strength model.
Direct gradient drawing changed JUCE edge coverage in the rendering comparison,
so the implementation draws JUCE geometry once into a physical-resolution alpha mask
and replaces only its RGB values before compositing.
The existing raw cache format and parser version are unchanged by palette correction.

The Rust overview module has also had its format descriptions corrected.
Its existing terminal threshold renderer remains a heuristic rather than a reference for Serato colors.
The historical local plan is not part of the published findings,
and its unchecked acceptance list is not evidence that platform and live GUI validation are complete.