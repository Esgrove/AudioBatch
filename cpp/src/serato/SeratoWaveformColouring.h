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
/// A coloured waveform rendered at the physical resolution of the graphics context it is drawn in.
struct ColouredThumbnailImage {
    /// The waveform pixels in the colours of the overview, or an invalid image for empty bounds.
    juce::Image image;
    /// Bounds of the image in the physical pixels of the context.
    juce::Rectangle<int> physicalBounds;
    /// Physical pixels per logical pixel of the context the image was rendered for.
    float scale {1.0f};
    /// True when the image was rendered for the software renderer, whose image type differs from the native one.
    bool software {false};
};

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

[[nodiscard]] ColouredThumbnailImage renderColouredThumbnail(
    const juce::Graphics& graphics,
    juce::AudioThumbnail& thumbnail,
    juce::Rectangle<int> waveformBounds,
    juce::Range<double> visibleRange,
    float displayGain,
    const juce::ColourGradient& gradient
);
[[nodiscard]] bool isRenderedFor(const ColouredThumbnailImage& rendered, const juce::Graphics& graphics);

void drawColouredThumbnail(juce::Graphics& graphics, const ColouredThumbnailImage& rendered);
}  // namespace audiobatch::serato
