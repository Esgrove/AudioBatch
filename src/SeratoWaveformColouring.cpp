/// Decodes Serato Overview raster cells through the zero-based 6x6x6 RGB cube.
/// Averages valid foreground RGB contributions, preserving cell and time-overlap weights.
/// Samples only the visible time intervals and bounds gradient stops by viewport resolution.
/// JUCE remains responsible for waveform amplitude, channel geometry and clipping.

#include "SeratoWaveformColouring.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <cmath>

namespace audiobatch::serato
{
namespace
{
/// Retains decoded RGB sums and foreground cell weight until the final arithmetic mean.
struct ColourContributions {
    std::array<double, 3> components {};
    double weight = 0.0;
};

constexpr int paletteLevels = 6;
constexpr int palettePlaneSize = paletteLevels * paletteLevels;
constexpr int paletteSize = palettePlaneSize * paletteLevels;
constexpr int paletteComponentStep = 51;
constexpr double maximumColourComponent = 255.0;

/// Decodes every vertical cell with a time-sample weight, independently of its row position.
/// Indices 0 and 1 are background. Indices 216 through 255 remain provisionally unknown or reserved.
/// Reserved values are skipped rather than assigned an unverified grayscale palette.
void accumulateSlice(
    const std::array<std::uint8_t, Overview::bandCount>& cells,
    const double sampleWeight,
    ColourContributions& contributions
)
{
    for (const auto paletteIndex : cells) {
        if (paletteIndex <= 1 || paletteIndex >= paletteSize) {
            continue;
        }
        const auto red = (paletteIndex / palettePlaneSize) * paletteComponentStep;
        const auto green = ((paletteIndex % palettePlaneSize) / paletteLevels) * paletteComponentStep;
        const auto blue = (paletteIndex % paletteLevels) * paletteComponentStep;
        contributions.components[0] += static_cast<double>(red) * sampleWeight;
        contributions.components[1] += static_cast<double>(green) * sampleWeight;
        contributions.components[2] += static_cast<double>(blue) * sampleWeight;
        contributions.weight += sampleWeight;
    }
}

/// Produces the opaque arithmetic mean RGB without contrast adjustment, or opaque fallback for no valid cells.
juce::Colour colourForContributions(const ColourContributions& contributions, const juce::Colour fallback)
{
    if (contributions.weight <= 0.0) {
        return fallback.withAlpha(1.0f);
    }

    const auto divisor = contributions.weight * maximumColourComponent;
    return juce::Colour::fromFloatRGBA(
        static_cast<float>(contributions.components[0] / divisor),
        static_cast<float>(contributions.components[1] / divisor),
        static_cast<float>(contributions.components[2] / divisor),
        1.0f
    );
}

/// Decodes adjacent slices before interpolating RGB sums and valid-cell weights between their centres.
/// Clamps before the first and after the last centre without interpolating numeric palette indices.
ColourContributions contributionsAtTime(const Overview& overview, const double time, const double totalLength)
{
    const auto lastIndex = overview.slices.size() - 1;
    const auto position = std::clamp(
        std::clamp(time / totalLength, 0.0, 1.0) * static_cast<double>(overview.slices.size()) - 0.5,
        0.0,
        static_cast<double>(lastIndex)
    );
    const auto leftIndex = static_cast<std::size_t>(position);
    const auto rightIndex = std::min(leftIndex + 1, lastIndex);
    const auto fraction = position - static_cast<double>(leftIndex);
    ColourContributions left;
    ColourContributions right;
    accumulateSlice(overview.slices[leftIndex], 1.0, left);
    accumulateSlice(overview.slices[rightIndex], 1.0, right);
    ColourContributions result;
    for (std::size_t component = 0; component < result.components.size(); ++component) {
        result.components[component] = std::lerp(left.components[component], right.components[component], fraction);
    }
    result.weight = std::lerp(left.weight, right.weight, fraction);
    return result;
}

/// Accumulates decoded RGB and foreground weights over exact overlaps before computing their mean.
/// Avoids allocation or iteration over offscreen slices.
ColourContributions contributionsOverInterval(
    const Overview& overview,
    const double startTime,
    const double endTime,
    const double totalLength
)
{
    const auto sliceCount = static_cast<double>(overview.slices.size());
    const auto start = std::clamp(startTime / totalLength, 0.0, 1.0) * sliceCount;
    const auto end = std::clamp(endTime / totalLength, 0.0, 1.0) * sliceCount;
    if (end <= start) {
        return contributionsAtTime(overview, startTime, totalLength);
    }

    ColourContributions result;
    const auto firstIndex = static_cast<std::size_t>(start);
    const auto endIndex = std::min(static_cast<std::size_t>(std::ceil(end)), overview.slices.size());
    for (auto slice = firstIndex; slice < endIndex; ++slice) {
        const auto overlap
            = std::min(end, static_cast<double>(slice + 1)) - std::max(start, static_cast<double>(slice));
        accumulateSlice(overview.slices[slice], overlap, result);
    }
    return result;
}
}  // namespace

juce::Colour colourForOverviewSlice(
    const std::array<std::uint8_t, Overview::bandCount>& cells,
    const juce::Colour fallback
)
{
    ColourContributions contributions;
    accumulateSlice(cells, 1.0, contributions);
    return colourForContributions(contributions, fallback);
}

std::optional<juce::ColourGradient> makeOverviewGradient(
    const Overview& overview,
    const juce::Range<double> visibleRange,
    const double totalLength,
    const juce::Rectangle<int> waveformBounds,
    const juce::Colour fallback
)
{
    if (overview.slices.empty() || overview.slices.size() > Overview::maxSlices || waveformBounds.isEmpty()
        || !std::isfinite(totalLength) || totalLength <= 0.0 || !std::isfinite(visibleRange.getStart())
        || !std::isfinite(visibleRange.getEnd()) || !std::isfinite(visibleRange.getLength())
        || visibleRange.getLength() <= 0.0)
    {
        return std::nullopt;
    }

    juce::ColourGradient gradient(
        colourForContributions(contributionsAtTime(overview, visibleRange.getStart(), totalLength), fallback),
        static_cast<float>(waveformBounds.getX()),
        0.0f,
        colourForContributions(contributionsAtTime(overview, visibleRange.getEnd(), totalLength), fallback),
        static_cast<float>(waveformBounds.getRight()),
        0.0f,
        false
    );

    const auto sampleCount = std::min(waveformBounds.getWidth(), maxWaveformGradientStops - 2);
    const auto sampleDuration = visibleRange.getLength() / static_cast<double>(sampleCount);
    const auto combineSlices = sampleDuration / totalLength * static_cast<double>(overview.slices.size()) > 1.0;
    for (int sample = 0; sample < sampleCount; ++sample) {
        const auto position = (static_cast<double>(sample) + 0.5) / static_cast<double>(sampleCount);
        const auto time = std::lerp(visibleRange.getStart(), visibleRange.getEnd(), position);
        const auto contributions = combineSlices
            ? contributionsOverInterval(
                  overview,
                  std::lerp(visibleRange.getStart(), visibleRange.getEnd(), static_cast<double>(sample) / sampleCount),
                  std::lerp(
                      visibleRange.getStart(), visibleRange.getEnd(), static_cast<double>(sample + 1) / sampleCount
                  ),
                  totalLength
              )
            : contributionsAtTime(overview, time, totalLength);
        gradient.addColour(position, colourForContributions(contributions, fallback));
    }
    return gradient;
}

void drawColouredThumbnail(
    juce::Graphics& graphics,
    juce::AudioThumbnail& thumbnail,
    const juce::Rectangle<int> waveformBounds,
    const juce::Range<double> visibleRange,
    const float displayGain,
    const juce::ColourGradient& gradient
)
{
    if (waveformBounds.isEmpty()) {
        return;
    }

    const auto scale = graphics.getInternalContext().getPhysicalPixelScaleFactor();
    const auto physicalBounds
        = waveformBounds.toFloat().transformedBy(juce::AffineTransform::scale(scale)).getSmallestIntegerContainer();
    const auto software
        = dynamic_cast<juce::LowLevelGraphicsSoftwareRenderer*>(&graphics.getInternalContext()) != nullptr;
    const juce::SoftwareImageType softwareImageType;
    const juce::NativeImageType nativeImageType;
    const auto& imageType = software ? static_cast<const juce::ImageType&>(softwareImageType)
                                     : static_cast<const juce::ImageType&>(nativeImageType);
    const juce::Image image(juce::Image::ARGB, physicalBounds.getWidth(), physicalBounds.getHeight(), true, imageType);
    {
        juce::Graphics maskGraphics(image);
        maskGraphics.addTransform(
            juce::AffineTransform::scale(scale).translated(
                -static_cast<float>(physicalBounds.getX()), -static_cast<float>(physicalBounds.getY())
            )
        );
        maskGraphics.setColour(juce::Colours::white);
        thumbnail.drawChannels(
            maskGraphics, waveformBounds, visibleRange.getStart(), visibleRange.getEnd(), displayGain
        );
    }

    {
        const juce::Image::BitmapData pixels(image, juce::Image::BitmapData::readWrite);
        for (int column = 0; column < image.getWidth(); ++column) {
            const auto logicalX = static_cast<double>(physicalBounds.getX() + column) / static_cast<double>(scale);
            const auto position = (logicalX - static_cast<double>(gradient.point1.x))
                / static_cast<double>(gradient.point2.x - gradient.point1.x);
            const auto colour = gradient.getColourAtPosition(position);
            for (int row = 0; row < image.getHeight(); ++row) {
                const auto alpha = pixels.getPixelColour(column, row).getAlpha();
                if (alpha != 0) {
                    pixels.setPixelColour(column, row, colour.withAlpha(alpha));
                }
            }
        }
    }

    const juce::Graphics::ScopedSaveState savedState(graphics);
    graphics.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
    graphics.drawImageTransformed(
        image,
        juce::AffineTransform::translation(
            static_cast<float>(physicalBounds.getX()), static_cast<float>(physicalBounds.getY())
        )
            .scaled(1.0f / scale)
    );
}
}  // namespace audiobatch::serato
