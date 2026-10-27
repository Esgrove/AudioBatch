/// Decodes Serato Overview raster cells through the zero-based 6x6x6 RGB cube.
/// Averages valid foreground RGB contributions, preserving cell and time-overlap weights.
/// Samples only the visible time intervals and bounds gradient stops by viewport resolution.
/// JUCE remains responsible for waveform amplitude, channel geometry and clipping.

#include "serato/SeratoWaveformColouring.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <cmath>
#include <ranges>

namespace audiobatch::serato
{
namespace
{
constexpr int paletteLevels {6};
constexpr int palettePlaneSize {paletteLevels * paletteLevels};
constexpr int paletteSize {palettePlaneSize * paletteLevels};
constexpr int paletteComponentStep {51};
constexpr double maximumColourComponent {255.0};

/// Retains decoded RGB sums and foreground cell weight until the final arithmetic mean.
struct ColourContributions {
    /// Weighted sums of the red, green, and blue components.
    std::array<double, 3> components {};
    /// Summed weight of the foreground cells that contributed.
    double weight {0.0};
};

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
        const auto red = paletteIndex / palettePlaneSize * paletteComponentStep;
        const auto green = paletteIndex % palettePlaneSize / paletteLevels * paletteComponentStep;
        const auto blue = paletteIndex % paletteLevels * paletteComponentStep;
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
    for (auto&& [component, leftComponent, rightComponent] :
         std::views::zip(result.components, left.components, right.components))
    {
        component = std::lerp(leftComponent, rightComponent, fraction);
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

/// Produces the opaque arithmetic mean RGB of valid nonbackground raster cells, independent of row position.
/// The zero-based 6x6x6 cube is R=(index/36)*51, G=((index%36)/6)*51, B=(index%6)*51 using integer division.
/// Background indices 0 and 1 are skipped. Indices 216 through 255 are provisionally unknown or reserved and skipped.
/// No grayscale mapping is asserted for reserved values. With no eligible cells, the supplied fallback becomes opaque.
/// An index is not a strength, so uniform audio gain is not represented by scaling these bytes.
juce::Colour colourForOverviewSlice(
    const std::array<std::uint8_t, Overview::bandCount>& cells,
    const juce::Colour fallback
)
{
    ColourContributions contributions;
    accumulateSlice(cells, 1.0, contributions);
    return colourForContributions(contributions, fallback);
}

/// Creates an opaque horizontal brush for the same inset bounds and seconds range used by AudioThumbnail.
/// Endpoint colours sample the exact visible times, clamped to the track.
/// Interior samples decode adjacent slices before interpolating RGB sums and valid-cell weights at close zoom.
/// When multiple slices fit a sample interval, their overlap-weighted decoded contributions combine before the mean.
/// Storage is bounded by visible logical pixels and maxWaveformGradientStops, not offscreen slices.
/// Empty or oversized overviews, non-finite times and empty viewports return no gradient.
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

/// Returns true when the image was rendered for the pixel scale and the renderer of the graphics context,
/// so drawing it there gives the same pixels as rendering it again.
bool isRenderedFor(const ColouredThumbnailImage& rendered, const juce::Graphics& graphics)
{
    auto& context = graphics.getInternalContext();
    return juce::exactlyEqual(rendered.scale, context.getPhysicalPixelScaleFactor())
        && rendered.software == (dynamic_cast<juce::LowLevelGraphicsSoftwareRenderer*>(&context) != nullptr);
}

/// Draws JUCE's solid waveform once at physical resolution and changes only its RGB channels.
/// Direct gradient fills use different raster paths from solid fills on both software and Direct2D backends.
/// Preserving the solid mask avoids their changed edge coverage, including disappearing low-alpha pixels.
/// The image uses the backend of the renderer of the graphics context,
/// and stays valid for drawing there until the PCM, range, gain, bounds, or scale change.
ColouredThumbnailImage renderColouredThumbnail(
    const juce::Graphics& graphics,
    juce::AudioThumbnail& thumbnail,
    const juce::Rectangle<int> waveformBounds,
    const juce::Range<double> visibleRange,
    const float displayGain,
    const juce::ColourGradient& gradient
)
{
    const auto scale = graphics.getInternalContext().getPhysicalPixelScaleFactor();
    const auto software
        = dynamic_cast<juce::LowLevelGraphicsSoftwareRenderer*>(&graphics.getInternalContext()) != nullptr;
    if (waveformBounds.isEmpty()) {
        return {.image = {}, .physicalBounds = {}, .scale = scale, .software = software};
    }

    const auto physicalBounds
        = waveformBounds.toFloat().transformedBy(juce::AffineTransform::scale(scale)).getSmallestIntegerContainer();
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

    return {.image = image, .physicalBounds = physicalBounds, .scale = scale, .software = software};
}

/// Draws a rendered coloured waveform at its physical pixel bounds without resampling it.
void drawColouredThumbnail(juce::Graphics& graphics, const ColouredThumbnailImage& rendered)
{
    if (!rendered.image.isValid()) {
        return;
    }

    const juce::Graphics::ScopedSaveState savedState(graphics);
    graphics.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
    graphics.drawImageTransformed(
        rendered.image,
        juce::AffineTransform::translation(
            static_cast<float>(rendered.physicalBounds.getX()), static_cast<float>(rendered.physicalBounds.getY())
        )
            .scaled(1.0f / rendered.scale)
    );
}
}  // namespace audiobatch::serato
