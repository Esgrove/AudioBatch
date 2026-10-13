/// Synthetic indexed-raster colour and time-projection tests, plus offscreen JUCE waveform brush comparisons.
/// Generated in-memory audio exercises coarse peaks and the close-zoom buffer reader without filesystem I/O.
/// Exact nonzero coverage verifies unchanged mono/stereo geometry at several gains and display scales.
/// Final bitmap compositing can round alpha and premultiplied RGB by one byte, but must not remove any PCM pixels.

#include "serato/SeratoWaveformColouring.h"
#include "TestSuites.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace audiobatch::tests
{
namespace
{
using Slice = std::array<std::uint8_t, serato::Overview::bandCount>;

/// Seeds background index 1 and fills a bounded number of vertical cells with one palette index.
Slice paletteSlice(const std::uint8_t paletteIndex, const std::size_t occupiedRows = serato::Overview::bandCount)
{
    Slice result;
    result.fill(1);
    std::fill_n(result.begin(), std::min(occupiedRows, result.size()), paletteIndex);
    return result;
}

/// Returns synthetic red, green and blue raster regions without external music, screenshots or tag payloads.
serato::Overview threeRegions()
{
    return {{paletteSlice(180), paletteSlice(30), paletteSlice(5)}};
}

/// Describes brush changes separately from waveform coverage and stereo channel occupancy.
struct PixelComparison {
    int coverageDifferences = 0;
    int maximumAlphaDifference = 0;
    int maximumPremultipliedColourDifference = 0;
    int coveredPixels = 0;
    int changedColours = 0;
    int redPixels = 0;
    int bluePixels = 0;
    int upperPixels = 0;
    int lowerPixels = 0;
};

/// Compares physical image pixels, including antialiasing at fractional display scale.
PixelComparison comparePixels(const juce::Image& solid, const juce::Image& coloured)
{
    PixelComparison result;
    const juce::Image::BitmapData solidPixels(solid, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData colouredPixels(coloured, juce::Image::BitmapData::readOnly);
    for (int row = 0; row < solid.getHeight(); ++row) {
        for (int column = 0; column < solid.getWidth(); ++column) {
            const auto reference = solidPixels.getPixelColour(column, row);
            const auto actual = colouredPixels.getPixelColour(column, row);
            result.coverageDifferences += (reference.getAlpha() == 0) != (actual.getAlpha() == 0) ? 1 : 0;
            result.maximumAlphaDifference = std::max(
                result.maximumAlphaDifference, std::abs(static_cast<int>(reference.getAlpha()) - actual.getAlpha())
            );
            juce::PixelARGB referencePixel {0, 0, 0, 0};
            juce::PixelARGB actualPixel {0, 0, 0, 0};
            std::memcpy(&referencePixel, solidPixels.getPixelPointer(column, row), sizeof(referencePixel));
            std::memcpy(&actualPixel, colouredPixels.getPixelPointer(column, row), sizeof(actualPixel));
            result.maximumPremultipliedColourDifference = std::max({
                result.maximumPremultipliedColourDifference,
                std::abs(static_cast<int>(referencePixel.getRed()) - actualPixel.getRed()),
                std::abs(static_cast<int>(referencePixel.getGreen()) - actualPixel.getGreen()),
                std::abs(static_cast<int>(referencePixel.getBlue()) - actualPixel.getBlue()),
            });
            if (reference.getAlpha() == 0) {
                continue;
            }
            ++result.coveredPixels;
            result.changedColours += reference != actual ? 1 : 0;
            result.redPixels += actual.getRed() > actual.getBlue() ? 1 : 0;
            result.bluePixels += actual.getBlue() > actual.getRed() ? 1 : 0;
            if (row < solid.getHeight() / 2) {
                ++result.upperPixels;
            } else {
                ++result.lowerPixels;
            }
        }
    }
    return result;
}

/// Draws onto a transparent, physically scaled bitmap using the coloured mask or the original solid brush.
juce::Image renderThumbnail(
    juce::AudioThumbnail& thumbnail,
    const juce::Range<double> range,
    const float gain,
    const float scale,
    const juce::ColourGradient* gradient,
    const juce::ImageType& imageType
)
{
    juce::Image image(
        juce::Image::ARGB,
        static_cast<int>(std::ceil(260.0f * scale)),
        static_cast<int>(std::ceil(104.0f * scale)),
        true,
        imageType
    );
    juce::Graphics graphics(image);
    graphics.addTransform(juce::AffineTransform::scale(scale));
    graphics.setColour(juce::Colours::blue);
    if (gradient != nullptr) {
        serato::drawColouredThumbnail(graphics, thumbnail, {2, 2, 256, 100}, range, gain, *gradient);
    } else {
        thumbnail.drawChannels(graphics, {2, 2, 256, 100}, range.getStart(), range.getEnd(), gain);
    }
    return image;
}

/// Generates one second of signed PCM with distinct channel amplitudes for stereo geometry checks.
juce::AudioBuffer<float> generatedAudio(const int channels)
{
    constexpr int sampleCount = 8192;
    juce::AudioBuffer<float> buffer(channels, sampleCount);
    for (int channel = 0; channel < channels; ++channel) {
        const auto amplitude = channel == 0 ? 0.85 : 0.3;
        for (int sample = 0; sample < sampleCount; ++sample) {
            const auto phase = juce::MathConstants<double>::twoPi * 37.0 * sample / sampleCount;
            buffer.setSample(channel, sample, static_cast<float>(amplitude * std::sin(phase)));
        }
    }
    return buffer;
}
}  // namespace

/// Verifies the indexed palette independently of audio amplitude and JUCE geometry.
class SeratoWaveformColouringTests final : public juce::UnitTest
{
public:
    /// Registers this suite through its explicit factory rather than a static instance.
    SeratoWaveformColouringTests() : juce::UnitTest("Serato waveform colouring", "AudioBatch") {}

    /// Runs portable palette, resampling and offscreen brush coverage checks.
    void runTest() override
    {
        testPalette();
        testProjection();
        testCompression();
        testInvalidInputs();
        testRendering();
    }

private:
    /// Allows only byte-rounding differences in independently sampled colours.
    void expectColourNear(const juce::Colour actual, const juce::Colour expected, const int tolerance = 2)
    {
        expect(std::abs(static_cast<int>(actual.getRed()) - expected.getRed()) <= tolerance);
        expect(std::abs(static_cast<int>(actual.getGreen()) - expected.getGreen()) <= tolerance);
        expect(std::abs(static_cast<int>(actual.getBlue()) - expected.getBlue()) <= tolerance);
        expectEquals(static_cast<int>(actual.getAlpha()), 255);
    }

    /// Checks cube colours, every vertical row, decoded RGB means and background or reserved fallback.
    void testPalette()
    {
        beginTest("Zero-based RGB cube primaries, mixed colours and neutral colours retain their brightness");
        const auto fallback = juce::Colours::blue;
        expectColourNear(serato::colourForOverviewSlice(paletteSlice(180), fallback), juce::Colours::red, 0);
        expectColourNear(serato::colourForOverviewSlice(paletteSlice(30), fallback), juce::Colours::lime, 0);
        expectColourNear(serato::colourForOverviewSlice(paletteSlice(5), fallback), juce::Colours::blue, 0);
        expectColourNear(serato::colourForOverviewSlice(paletteSlice(121), fallback), juce::Colour(153, 102, 51), 0);
        expectColourNear(serato::colourForOverviewSlice(paletteSlice(35), fallback), juce::Colour(0, 255, 255), 0);
        expectColourNear(serato::colourForOverviewSlice(paletteSlice(36), fallback), juce::Colour(51, 0, 0), 0);
        for (const int level : {1, 2, 3, 4, 5}) {
            const auto component = static_cast<std::uint8_t>(level * 51);
            expectColourNear(
                serato::colourForOverviewSlice(paletteSlice(static_cast<std::uint8_t>(level * 43)), fallback),
                juce::Colour(component, component, component),
                0
            );
        }

        beginTest("All sixteen vertical cells participate and identical indices are independent of row position");
        for (std::size_t row = 0; row < serato::Overview::bandCount; ++row) {
            auto isolated = paletteSlice(1);
            isolated[row] = 121;
            expectColourNear(serato::colourForOverviewSlice(isolated, fallback), juce::Colour(153, 102, 51), 0);
            auto mixed = paletteSlice(180);
            mixed[row] = 5;
            expectColourNear(serato::colourForOverviewSlice(mixed, fallback), juce::Colour(239, 0, 16), 1);
        }

        beginTest("Arithmetic mean mixes decoded dark red and bright blue, not numeric indices or strengths");
        auto mixed = paletteSlice(1);
        mixed[0] = 36;
        mixed[15] = 5;
        expectColourNear(serato::colourForOverviewSlice(mixed, fallback), juce::Colour(26, 0, 128), 1);
        expect(
            serato::colourForOverviewSlice(paletteSlice(36), fallback)
            != serato::colourForOverviewSlice(paletteSlice(180), fallback)
        );

        beginTest("Background 0/1 and all reserved 216-255 indices use opaque fallback and do not dilute colour");
        const auto transparentFallback = juce::Colour(17, 23, 29).withAlpha(0.0f);
        const auto opaqueFallback = transparentFallback.withAlpha(1.0f);
        expect(serato::colourForOverviewSlice(paletteSlice(0), transparentFallback) == opaqueFallback);
        expect(serato::colourForOverviewSlice(paletteSlice(1), transparentFallback) == opaqueFallback);
        for (int paletteIndex = 216; paletteIndex <= 255; ++paletteIndex) {
            auto reserved = paletteSlice(static_cast<std::uint8_t>(paletteIndex));
            expect(serato::colourForOverviewSlice(reserved, transparentFallback) == opaqueFallback);
            reserved[0] = 0;
            reserved[1] = 1;
            expect(serato::colourForOverviewSlice(reserved, transparentFallback) == opaqueFallback);
            reserved[15] = 121;
            expectColourNear(
                serato::colourForOverviewSlice(reserved, transparentFallback), juce::Colour(153, 102, 51), 0
            );
        }
    }

    /// Verifies exact endpoint sampling, pan/zoom projection and clamping beyond track boundaries.
    void testProjection()
    {
        beginTest("Gradient endpoints use exact inset coordinates and clamped visible times");
        const auto overview = threeRegions();
        const auto full
            = serato::makeOverviewGradient(overview, {0.0, 6.0}, 6.0, {2, 2, 101, 100}, juce::Colours::blue);
        const auto zoom
            = serato::makeOverviewGradient(overview, {2.0, 4.0}, 6.0, {2, 2, 101, 100}, juce::Colours::blue);
        const auto pan = serato::makeOverviewGradient(overview, {3.0, 5.0}, 6.0, {2, 2, 101, 100}, juce::Colours::blue);
        expect(full.has_value() && zoom.has_value() && pan.has_value());
        if (!full || !zoom || !pan) {
            return;
        }
        expectEquals(full->point1.x, 2.0f);
        expectEquals(full->point2.x, 103.0f);
        expectColourNear(full->getColourAtPosition(0.0), juce::Colours::red);
        expectColourNear(full->getColourAtPosition(1.0), juce::Colours::blue);
        expectColourNear(full->getColourAtPosition(0.5), zoom->getColourAtPosition(0.5));
        expectColourNear(full->getColourAtPosition(0.5), pan->getColourAtPosition(0.0));

        expectColourNear(zoom->getColourAtPosition(0.0), juce::Colour(128, 128, 0), 1);
        expectColourNear(zoom->getColourAtPosition(1.0), juce::Colour(0, 128, 128), 1);

        const auto outside
            = serato::makeOverviewGradient(overview, {-2.0, 8.0}, 6.0, {2, 2, 100, 100}, juce::Colours::blue);
        expect(outside.has_value());
        if (outside) {
            expectColourNear(outside->getColourAtPosition(0.0), juce::Colours::red);
            expectColourNear(outside->getColourAtPosition(1.0), juce::Colours::blue);
        }
        const auto before
            = serato::makeOverviewGradient(overview, {-4.0, -2.0}, 6.0, {2, 2, 100, 100}, juce::Colours::blue);
        expect(before.has_value());
        if (before) {
            expectColourNear(before->getColourAtPosition(0.5), juce::Colours::red);
        }

        beginTest("Adjacent indices across a cube discontinuity decode before time interpolation");
        const serato::Overview discontinuity {{paletteSlice(35), paletteSlice(36)}};
        const auto interpolated
            = serato::makeOverviewGradient(discontinuity, {0.0, 2.0}, 2.0, {2, 2, 101, 10}, juce::Colours::blue);
        expect(interpolated.has_value());
        if (interpolated) {
            expectColourNear(interpolated->getColourAtPosition(0.5), juce::Colour(26, 128, 128), 1);
        }

        beginTest("Time interpolation retains valid-cell weights instead of averaging per-slice means");
        const serato::Overview unequalRows {{paletteSlice(180, 1), paletteSlice(5, 3)}};
        const auto weighted
            = serato::makeOverviewGradient(unequalRows, {0.0, 2.0}, 2.0, {2, 2, 101, 10}, juce::Colours::lime);
        expect(weighted.has_value());
        if (weighted) {
            expectColourNear(weighted->getColourAtPosition(0.5), juce::Colour(64, 0, 191), 1);
        }

        beginTest("Background and unknown slices carry no colour or valid-cell weight during interpolation");
        const serato::Overview background {{paletteSlice(1), paletteSlice(180, 1), paletteSlice(223)}};
        const auto backgroundGradient = serato::makeOverviewGradient(
            background, {0.0, 3.0}, 3.0, {2, 2, 101, 10}, juce::Colours::blue.withAlpha(0.0f)
        );
        expect(backgroundGradient.has_value());
        if (backgroundGradient) {
            expectColourNear(backgroundGradient->getColourAtPosition(0.0), juce::Colours::blue, 0);
            expectColourNear(backgroundGradient->getColourAtPosition(1.0), juce::Colours::blue, 0);
            expectColourNear(backgroundGradient->getColourAtPosition(0.5), juce::Colours::red, 0);
        }
    }

    /// Requires decoded cell and interval-overlap aggregation before the RGB mean, including narrow and capped views.
    void testCompression()
    {
        beginTest("Many slices in one pixel combine decoded contributions with foreground row counts");
        serato::Overview compressed {
            {paletteSlice(180, 1), paletteSlice(5, 3), paletteSlice(5, 3), paletteSlice(5, 3)},
        };
        const auto singlePixel
            = serato::makeOverviewGradient(compressed, {0.0, 4.0}, 4.0, {2, 2, 1, 10}, juce::Colours::blue);
        expect(singlePixel.has_value());
        if (singlePixel) {
            expectColourNear(singlePixel->getColourAtPosition(0.5), juce::Colour(26, 0, 230), 1);
            expectEquals(singlePixel->getNumColours(), 3);
        }

        beginTest("Fractional edge overlaps and the final slice participate in compressed bins");
        compressed.slices = {paletteSlice(180, 8), paletteSlice(5, 2), paletteSlice(180, 8), paletteSlice(5, 2)};
        const auto fractional
            = serato::makeOverviewGradient(compressed, {0.5, 3.5}, 4.0, {2, 2, 2, 10}, juce::Colours::blue);
        expect(fractional.has_value());
        if (fractional) {
            expectColourNear(fractional->getColourAtPosition(0.25), juce::Colour(170, 0, 85), 1);
            expectColourNear(fractional->getColourAtPosition(0.75), juce::Colour(227, 0, 28), 1);
        }

        beginTest("Compressed cube discontinuities and reserved slices use only decoded foreground contributions");
        compressed.slices = {paletteSlice(35, 1), paletteSlice(36, 3), paletteSlice(1), paletteSlice(223)};
        const auto decoded
            = serato::makeOverviewGradient(compressed, {0.0, 4.0}, 4.0, {2, 2, 1, 10}, juce::Colours::blue);
        expect(decoded.has_value());
        if (decoded) {
            expectColourNear(decoded->getColourAtPosition(0.5), juce::Colour(38, 64, 64), 1);
        }

        beginTest("Maximum tag size does not determine gradient allocation or exceed the stop cap");
        serato::Overview maximum;
        maximum.slices.resize(serato::Overview::maxSlices);
        for (std::size_t slice = 0; slice < maximum.slices.size(); ++slice) {
            maximum.slices[slice] = slice % 2 == 0 ? paletteSlice(180, 16) : paletteSlice(5, 4);
        }
        for (const int width : {1, 7, 100, 10000}) {
            const auto gradient
                = serato::makeOverviewGradient(maximum, {0.0, 1.0}, 1.0, {2, 2, width, 10}, juce::Colours::blue);
            expect(gradient.has_value());
            if (gradient) {
                expect(gradient->getNumColours() <= serato::maxWaveformGradientStops);
                expect(gradient->getNumColours() <= width + 2);
                const auto mix = gradient->getColourAtPosition(0.5);
                expect(mix.getRed() > mix.getBlue());
                expect(mix.getBlue() > 0);
            }
        }
    }

    /// Guards empty, oversized and invalid projections without inventing a gradient.
    void testInvalidInputs()
    {
        beginTest("Empty data and invalid viewport dimensions or times produce no gradient");
        const auto overview = threeRegions();
        const juce::Rectangle<int> bounds {2, 2, 100, 100};
        const auto fallback = juce::Colours::blue;
        const serato::Overview empty;
        expect(!serato::makeOverviewGradient(empty, {0.0, 1.0}, 1.0, bounds, fallback));
        expect(!serato::makeOverviewGradient(overview, {0.0, 1.0}, 1.0, {}, fallback));
        expect(!serato::makeOverviewGradient(overview, {0.0, 0.0}, 1.0, bounds, fallback));
        for (const double length :
             {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        {
            expect(!serato::makeOverviewGradient(overview, {0.0, 1.0}, length, bounds, fallback));
        }
        expect(!serato::makeOverviewGradient(
            overview, {0.0, std::numeric_limits<double>::infinity()}, 1.0, bounds, fallback
        ));
        serato::Overview oversized;
        oversized.slices.resize(serato::Overview::maxSlices + 1);
        expect(!serato::makeOverviewGradient(oversized, {0.0, 1.0}, 1.0, bounds, fallback));
    }

    /// Exercises both coarse thumbnail peaks and close-zoom AudioBufferReader geometry at physical display scales.
    void testRendering()
    {
        beginTest("Coloured mask preserves JUCE pixel coverage at full/zoom/pan, gain, stereo and 1/1.5/2 scales");
        for (const int channels : {1, 2}) {
            const auto buffer = generatedAudio(channels);
            juce::AudioFormatManager formats;
            juce::AudioThumbnailCache cache(2);
            juce::AudioThumbnail thumbnail(16, formats, cache);
            thumbnail.setSource(&buffer, 8192.0, channels);
            thumbnail.addBlock(0, buffer, 0, buffer.getNumSamples());
            expect(thumbnail.isFullyLoaded());
            expectEquals(thumbnail.getNumChannels(), channels);
            for (const auto range : {juce::Range<double> {0.0, 1.0}, {0.1, 0.3}, {0.65, 0.85}, {-0.1, 0.3}, {0.8, 1.2}})
            {
                testRenderingRange(thumbnail, range, channels);
            }
        }
    }

    /// Checks each rendering setting separately and verifies background raster data cannot erase nonzero PCM peaks.
    void testRenderingRange(juce::AudioThumbnail& thumbnail, const juce::Range<double> range, const int channels)
    {
        const auto gradient
            = serato::makeOverviewGradient(threeRegions(), range, 1.0, {2, 2, 256, 100}, juce::Colours::blue);
        const auto silent = serato::makeOverviewGradient(
            serato::Overview {{paletteSlice(1)}}, range, 1.0, {2, 2, 256, 100}, juce::Colours::blue
        );
        expect(gradient.has_value() && silent.has_value());
        if (!gradient || !silent) {
            return;
        }
        const juce::SoftwareImageType software;
        const juce::NativeImageType native;
        for (const juce::ImageType* imageType :
             {static_cast<const juce::ImageType*>(&software), static_cast<const juce::ImageType*>(&native)})
        {
            for (const float gain : {0.0f, 0.35f, 1.0f, 2.5f}) {
                for (const float scale : {1.0f, 1.5f, 2.0f}) {
                    const auto solid = renderThumbnail(thumbnail, range, gain, scale, nullptr, *imageType);
                    const auto coloured = renderThumbnail(thumbnail, range, gain, scale, &*gradient, *imageType);
                    const auto comparison = comparePixels(solid, coloured);
                    expectEquals(comparison.coverageDifferences, 0, "Coloured waveform pixel coverage");
                    expect(comparison.maximumAlphaDifference <= 1, "Coloured waveform alpha quantisation");
                    if (gain > 0.0f) {
                        expect(comparison.coveredPixels > 0);
                        expect(comparison.changedColours > 0);
                    }
                    if (range == juce::Range<double> {0.0, 1.0} && gain == 1.0f) {
                        expect(comparison.redPixels > 0 && comparison.bluePixels > 0);
                        if (channels == 2) {
                            expect(comparison.upperPixels > comparison.lowerPixels);
                            expect(comparison.lowerPixels > 0);
                        }
                    }
                    const auto silentImage = renderThumbnail(thumbnail, range, gain, scale, &*silent, *imageType);
                    const auto silentComparison = comparePixels(solid, silentImage);
                    expectEquals(silentComparison.coverageDifferences, 0, "Silent fallback pixel coverage");
                    expect(silentComparison.maximumAlphaDifference <= 1, "Silent fallback alpha quantisation");
                    expect(
                        silentComparison.maximumPremultipliedColourDifference <= 1, "Silent fallback RGB quantisation"
                    );
                }
            }
        }
    }
};

/// Creates the suite for the owning runner's explicit registration.
std::unique_ptr<juce::UnitTest> createSeratoWaveformColouringTests()
{
    return std::make_unique<SeratoWaveformColouringTests>();
}
}  // namespace audiobatch::tests
