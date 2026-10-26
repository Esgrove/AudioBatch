/// Serato indexed-raster colouring for JUCE's existing waveform geometry.
/// Decodes all sixteen vertical cells and projects a bounded horizontal colour field into a seconds-based viewport.
/// Raw bytes and the existing Overview API are preserved, but each byte is a palette index, not a band strength.

#pragma once

#include "serato/SeratoOverview.h"

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
inline constexpr int maxWaveformGradientStops {4096};

[[nodiscard]] juce::Colour colourForOverviewSlice(
    const std::array<std::uint8_t, Overview::bandCount>& cells,
    juce::Colour fallback
);

[[nodiscard]] std::optional<juce::ColourGradient> makeOverviewGradient(
    const Overview& overview,
    juce::Range<double> visibleRange,
    double totalLength,
    juce::Rectangle<int> waveformBounds,
    juce::Colour fallback
);

void drawColouredThumbnail(
    juce::Graphics& graphics,
    juce::AudioThumbnail& thumbnail,
    juce::Rectangle<int> waveformBounds,
    juce::Range<double> visibleRange,
    float displayGain,
    const juce::ColourGradient& gradient
);
}  // namespace audiobatch::serato
