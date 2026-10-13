/// Serato indexed-raster colouring for JUCE's existing waveform geometry.
/// Decodes all sixteen vertical cells and projects a bounded horizontal colour field into a seconds-based viewport.
/// Raw bytes and the existing Overview API are preserved, but each byte is a palette index, not a band strength.

#pragma once

#include "SeratoOverview.h"

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <optional>

namespace juce
{
class AudioThumbnail;
}

namespace audiobatch::serato
{
/// Bounds gradient storage independently of tag size and offscreen slice count.
inline constexpr int maxWaveformGradientStops = 4096;

/// Produces the opaque arithmetic mean RGB of valid nonbackground raster cells, independent of row position.
/// The zero-based 6x6x6 cube is R=(index/36)*51, G=((index%36)/6)*51, B=(index%6)*51 using integer division.
/// Background indices 0 and 1 are skipped. Indices 216 through 255 are provisionally unknown or reserved and skipped.
/// No grayscale mapping is asserted for reserved values. With no eligible cells, the supplied fallback becomes opaque.
/// An index is not a strength, so uniform audio gain is not represented by scaling these bytes.
[[nodiscard]] juce::Colour colourForOverviewSlice(
    const std::array<std::uint8_t, Overview::bandCount>& cells,
    juce::Colour fallback
);

/// Creates an opaque horizontal brush for the same inset bounds and seconds range used by AudioThumbnail.
/// Endpoint colours sample the exact visible times, clamped to the track.
/// Interior samples decode adjacent slices before interpolating RGB sums and valid-cell weights at close zoom.
/// When multiple slices fit a sample interval, their overlap-weighted decoded contributions combine before the mean.
/// Storage is bounded by visible logical pixels and maxWaveformGradientStops, not offscreen slices.
/// Empty or oversized overviews, non-finite times and empty viewports return no gradient.
[[nodiscard]] std::optional<juce::ColourGradient> makeOverviewGradient(
    const Overview& overview,
    juce::Range<double> visibleRange,
    double totalLength,
    juce::Rectangle<int> waveformBounds,
    juce::Colour fallback
);

/// Draws JUCE's solid waveform once at physical resolution and changes only its RGB channels.
/// Direct gradient fills use different raster paths from solid fills on both software and Direct2D backends.
/// Preserving the solid mask avoids their changed edge coverage, including disappearing low-alpha pixels.
/// The temporary image uses the current renderer's backend and is rebuilt as PCM, range, gain or scale changes.
void drawColouredThumbnail(
    juce::Graphics& graphics,
    juce::AudioThumbnail& thumbnail,
    juce::Rectangle<int> waveformBounds,
    juce::Range<double> visibleRange,
    float displayGain,
    const juce::ColourGradient& gradient
);
}  // namespace audiobatch::serato
