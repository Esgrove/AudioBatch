/// Tests CPU-only spectrogram layout, stereo-position overlays, and cached band-response geometry.
/// Geometry observations use the production view without requiring GPU rendering or pixel assertions.

#include "analysis/SpatialAnalyzer.h"
#include "dsp/CrossoverDesign.h"
#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"

#include <gtest/gtest.h>

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace esgrove::image_control::test
{
/// Exposes cache observations without requiring GPU rendering or adding production control APIs.
struct SpectrogramViewTestAccess {
    /// Selects the production view whose cache and animation state are observed.
    explicit SpectrogramViewTestAccess(SpatialSpectrogramView& observed) : view(observed) {}

    /// Leaves the observed view's lifetime and cache unchanged.
    ~SpectrogramViewTestAccess() = default;

    SpectrogramViewTestAccess(const SpectrogramViewTestAccess&) = delete;
    SpectrogramViewTestAccess(SpectrogramViewTestAccess&&) = delete;
    SpectrogramViewTestAccess& operator=(const SpectrogramViewTestAccess&) = delete;
    SpectrogramViewTestAccess& operator=(SpectrogramViewTestAccess&&) = delete;

    /// Returns the cached geometry through the same lookup used during painting.
    const SpectrogramBandResponses& responses()
    {
        return view.bandResponses(view.currentCrossovers());
    }

    /// Reads the same scale-specific frequencies used by the production grid and axis labels.
    [[nodiscard]] std::span<const double> gridFrequencies() const
    {
        return view.gridFrequencies();
    }

    /// Reads the production frequency-axis overlay geometry without rendering pixels.
    [[nodiscard]] juce::Rectangle<float> frequencyAxisLabel(const double frequency) const
    {
        return view.frequencyAxisLabelBounds(frequency);
    }

    /// Reads the production stereo-axis overlay geometry without rendering pixels.
    [[nodiscard]] juce::Rectangle<float> positionAxisLabel(const double position) const
    {
        return view.positionAxisLabelBounds(position);
    }

    /// Reads the diagnostic overlay bounds to check it stays clear of inside position labels.
    [[nodiscard]] juce::Rectangle<float> frameRateLabel() const
    {
        return view.framesPerSecondArea();
    }

    /// Returns the label bounds used by the position mapping overlay without painting pixels.
    [[nodiscard]] juce::Rectangle<float> positionLabel(const float difference, const bool leftChannel) const
    {
        return view.positionCurveLabelBounds(difference, leftChannel);
    }

    /// Returns the same right-origin position mapping used by curve samples, guides, and centre-line ticks.
    [[nodiscard]] juce::Point<float> positionPoint(const float difference, const bool leftChannel) const
    {
        return view.positionCurvePoint(difference, leftChannel);
    }

    /// Counts geometry builds independently of path equality or allocator behavior.
    [[nodiscard]] std::uint64_t builds() const
    {
        return view.bandResponseBuilds;
    }

    /// Selects or clears the band consumed by the production hover animation.
    void hover(const std::optional<std::size_t> band)
    {
        view.hoveredBand = band;
    }

    /// View under observation, which outlives this accessor.
    SpatialSpectrogramView& view;
};

/// Mapping samples and centre-line ticks share a right-origin dB axis, independent of the frequency projection.
TEST(SpectrogramPositionOverlay, RightOriginEndpointsAndMirroredSettingsUseThePlotBounds)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    const SpectrogramViewTestAccess access(view);
    for (const auto& curve :
         std::array {
             StereoPositionCurve(3.0, 12.0),
             StereoPositionCurve(10.0, 32.0),
             StereoPositionCurve(24.0, 27.0),
             StereoPositionCurve(24.0, 60.0),
         })
    {
        view.setPositionCurve(curve);
        for (const int width : {300, 760, 999}) {
            view.setSize(width, 401);
            const auto plot = view.getPlotBounds();
            const float range = curve.edge() * 1.5F;
            for (const auto scale : {FrequencyScale::logarithmic, FrequencyScale::mel}) {
                view.setFrequencyScale(scale);
                for (const bool leftChannel : {true, false}) {
                    const auto origin = access.positionPoint(0.0F, leftChannel);
                    EXPECT_FLOAT_EQ(origin.x, plot.getRight());
                    EXPECT_FLOAT_EQ(origin.y, plot.getCentreY());
                    EXPECT_FLOAT_EQ(access.positionPoint(range, leftChannel).x, plot.getX());
                    const auto midpoint = access.positionPoint(curve.midpoint(), leftChannel);
                    EXPECT_FLOAT_EQ(midpoint.x, plot.getRight() - curve.midpoint() / range * plot.getWidth());
                    EXPECT_NEAR(midpoint.y, plot.getY() + (leftChannel ? 0.25F : 0.75F) * plot.getHeight(), 1.0e-4F);
                    const auto edge = access.positionPoint(curve.edge(), leftChannel);
                    EXPECT_FLOAT_EQ(edge.x, plot.getRight() - (2.0F / 3.0F) * plot.getWidth());
                    EXPECT_NEAR(edge.y, plot.getY() + (leftChannel ? 0.005F : 0.995F) * plot.getHeight(), 1.0e-4F);
                }
                float previousX = plot.getRight();
                for (const float difference : {3.0F, 6.0F, 12.0F, range}) {
                    const auto upper = access.positionPoint(difference, true);
                    const auto lower = access.positionPoint(difference, false);
                    EXPECT_FLOAT_EQ(upper.x, lower.x);
                    EXPECT_LT(upper.x, previousX);
                    EXPECT_GE(upper.x, plot.getX());
                    EXPECT_NEAR(upper.y + lower.y, plot.getCentreY() * 2.0F, 1.0e-4F);
                    previousX = upper.x;
                }
            }
        }
    }
}

/// Maximized geometry expands on every edge, retains complete inside labels, and restores the original gutters.
TEST(SpectrogramLayout, MaximizedAxesStayInsideTheExpandedPlotWithoutCornerOrCrossoverCollisions)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    const SpectrogramViewTestAccess access(view);
    for (const auto size : {juce::Point<int> {300, 120}, {760, 160}, {1000, 401}}) {
        view.setSize(size.x, size.y);
        view.setMaximized(false);
        const auto standard = view.getPlotBounds();
        view.setMaximized(true);
        const auto plot = view.getPlotBounds();
        EXPECT_EQ(plot, view.getLocalBounds().toFloat().reduced(2.0F));
        EXPECT_TRUE(plot.contains(standard));
        EXPECT_GT(plot.getWidth(), standard.getWidth());
        EXPECT_GT(plot.getHeight(), standard.getHeight());
        const auto left = access.positionAxisLabel(1.0);
        const auto centre = access.positionAxisLabel(0.0);
        const auto right = access.positionAxisLabel(-1.0);
        EXPECT_TRUE(plot.contains(left));
        EXPECT_TRUE(plot.contains(centre));
        EXPECT_TRUE(plot.contains(right));
        EXPECT_FLOAT_EQ(centre.getCentreY(), plot.getCentreY());
        EXPECT_FALSE(access.frameRateLabel().intersects(left));
        for (const auto scale : {FrequencyScale::logarithmic, FrequencyScale::mel}) {
            view.setFrequencyScale(scale);
            for (const double frequency :
                 {20.0, 30.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10'000.0, 20'000.0})
            {
                const auto label = access.frequencyAxisLabel(frequency);
                EXPECT_TRUE(plot.contains(label));
                EXPECT_FALSE(label.intersects(right));
                for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
                    EXPECT_FALSE(label.intersects(view.crossoverLabelBounds(crossover)));
                    EXPECT_TRUE(plot.contains(view.crossoverLabelBounds(crossover)));
                }
            }
        }
        view.setMaximized(false);
        EXPECT_EQ(view.getPlotBounds(), standard);
        EXPECT_FALSE(standard.intersects(access.frequencyAxisLabel(1000.0)));
        EXPECT_FALSE(standard.intersects(access.positionAxisLabel(0.0)));
    }
}

/// FPS stays right-anchored in either layout, and maximized endpoint pills keep only their measured text padding.
TEST(SpectrogramLayout, FrameRateUsesTopRightAndMaximizedFrequencyLabelsFitTheirGlyphs)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    const SpectrogramViewTestAccess access(view);
    for (const bool maximized : {false, true}) {
        view.setMaximized(maximized);
        for (const int width : {300, 760, 1000}) {
            view.setSize(width, 200);
            const auto plot = view.getPlotBounds();
            const auto fps = access.frameRateLabel();
            EXPECT_TRUE(plot.contains(fps));
            EXPECT_FLOAT_EQ(fps.getRight(), plot.getRight() - 4.0F);
            EXPECT_FLOAT_EQ(fps.getY(), plot.getY() + 4.0F);
            const auto endpoint = access.frequencyAxisLabel(20'000.0);
            if (maximized) {
                const float textWidth = juce::GlyphArrangement::getStringWidth(textFont(11.0F), "20k");
                EXPECT_FLOAT_EQ(endpoint.getWidth(), textWidth + 8.0F);
                EXPECT_FLOAT_EQ(endpoint.getRight(), plot.getRight() - 4.0F);
                EXPECT_GT(endpoint.getCentreX(), plot.getRight() - 24.0F);
            } else {
                EXPECT_FLOAT_EQ(endpoint.getWidth(), 40.0F);
            }
        }
    }
    view.setSize(1000, 200);
    view.setMaximized(true);
    const auto plot = view.getPlotBounds();
    const auto thirtyHertz = access.frequencyAxisLabel(30.0);
    const float expectedX
        = plot.getX() + static_cast<float>(frequencyToShare(30.0, FrequencyScale::logarithmic)) * plot.getWidth();
    EXPECT_NEAR(thirtyHertz.getCentreX(), expectedX, 0.0001F);
    EXPECT_FALSE(thirtyHertz.intersects(access.frequencyAxisLabel(20.0)));
    EXPECT_FALSE(thirtyHertz.intersects(access.frequencyAxisLabel(50.0)));
}

/// Each projection uses deliberate reference ticks, retaining 16 kHz and adding finer upper-frequency detail for Mel.
TEST(SpectrogramLayout, FrequencyGridUsesScaleSpecificTicksWithSixteenKilohertzInBoth)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(1000, 300);
    const SpectrogramViewTestAccess access(view);
    const std::vector<double> logarithmic {20, 30, 50, 100, 200, 500, 1000, 2000, 5000, 10'000, 16'000, 20'000};
    const std::vector<double> mel {
        20,
        100,
        200,
        500,
        1000,
        2000,
        3000,
        4000,
        6000,
        8000,
        10'000,
        12'000,
        14'000,
        16'000,
        18'000,
        20'000,
    };
    for (const bool maximized : {false, true}) {
        view.setMaximized(maximized);
        for (const auto scale : {FrequencyScale::logarithmic, FrequencyScale::mel, FrequencyScale::logarithmic}) {
            view.setFrequencyScale(scale);
            const auto frequencies = access.gridFrequencies();
            const auto& expected = scale == FrequencyScale::mel ? mel : logarithmic;
            EXPECT_EQ(std::vector<double>(frequencies.begin(), frequencies.end()), expected);
            EXPECT_TRUE(std::ranges::is_sorted(frequencies));
            EXPECT_EQ(std::ranges::count(frequencies, 16'000.0), 1);
            EXPECT_EQ(std::ranges::count(frequencies, 30.0), scale == FrequencyScale::mel ? 0 : 1);
            const auto highTicks
                = std::ranges::count_if(frequencies, [](const double frequency) { return frequency >= 2000.0; });
            EXPECT_EQ(highTicks, scale == FrequencyScale::mel ? 11 : 5);
            const auto plot = view.getPlotBounds();
            const float expectedX
                = plot.getX() + static_cast<float>(frequencyToShare(16'000.0, scale)) * plot.getWidth();
            EXPECT_NEAR(access.frequencyAxisLabel(16'000.0).getCentreX(), expectedX, 0.0001F);
        }
    }
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// Geometry toggles rebuild response paths on demand, without modifying audio or window dimensions.
TEST(SpectrogramLayout, MaximizingInvalidatesResponseGeometryOnlyWhenNeeded)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(760, 300);
    SpectrogramViewTestAccess access(view);
    const auto original = access.responses().plot;
    EXPECT_EQ(access.builds(), 1U);
    view.setMaximized(true);
    EXPECT_EQ(access.builds(), 1U);
    EXPECT_EQ(access.responses().plot, view.getPlotBounds());
    EXPECT_NE(access.responses().plot, original);
    EXPECT_EQ(access.builds(), 2U);
    view.setMaximized(true);
    EXPECT_EQ(access.responses().plot, view.getPlotBounds());
    EXPECT_EQ(access.builds(), 2U);
    view.setMaximized(false);
    EXPECT_EQ(access.responses().plot, original);
    EXPECT_EQ(access.builds(), 3U);
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// Right-aligned guide pills remain exposed and wholly inside even when the stereo mapping reaches an edge.
TEST(SpectrogramPositionOverlay, GuideLabelsStayInsideTheRightSideOfThePlot)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    const SpectrogramViewTestAccess access(view);
    for (const auto& curve : std::array {StereoPositionCurve(3.0, 12.0), StereoPositionCurve(24.0, 60.0)}) {
        view.setPositionCurve(curve);
        for (const int width : {300, 760, 999}) {
            for (const int height : {120, 401}) {
                view.setSize(width, height);
                const auto plot = view.getPlotBounds();
                for (const float difference : {3.0F, 6.0F, 12.0F, 18.0F, 24.0F, 36.0F, 48.0F, curve.edge()}) {
                    const auto upper = access.positionLabel(difference, true);
                    const auto lower = access.positionLabel(difference, false);
                    EXPECT_FALSE(upper.isEmpty());
                    EXPECT_FALSE(lower.isEmpty());
                    EXPECT_TRUE(plot.contains(upper));
                    EXPECT_TRUE(plot.contains(lower));
                    EXPECT_GT(upper.getX(), plot.getCentreX());
                    EXPECT_GT(lower.getX(), plot.getCentreX());
                    EXPECT_FLOAT_EQ(upper.getRight(), plot.getRight() - 4.0F);
                    EXPECT_FLOAT_EQ(lower.getRight(), plot.getRight() - 4.0F);
                    EXPECT_NEAR(upper.getCentreY() + lower.getCentreY(), plot.getCentreY() * 2.0F, 1.0e-4F);
                }
            }
        }
    }
}

/// Geometry and frequency-scale changes leave the existing hold, fade, and same-setting timing untouched.
TEST(SpectrogramPositionOverlay, GeometryDoesNotChangeTheHoldAndFadeContract)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(760, 401);
    const SpectrogramViewTestAccess access(view);
    view.setPositionCurve(StereoPositionCurve {});
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 0.0F);
    const StereoPositionCurve curve(10.0, 32.0);
    view.setPositionCurve(curve);
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 1.0F);
    view.advance(2.5);
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 1.0F);
    view.setFrequencyScale(FrequencyScale::mel);
    view.setSize(999, 401);
    EXPECT_FLOAT_EQ(access.positionPoint(0.0F, true).x, view.getPlotBounds().getRight());
    view.setPositionCurve(curve);
    view.advance(0.25);
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 0.5F);
    view.advance(0.25);
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 0.0F);
    view.setPositionCurve(StereoPositionCurve(12.0, 40.0));
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 1.0F);
}

/// Hover, gains, solo veils, and status opacity reuse geometry, while projection changes invalidate it.
TEST(SpectrogramOverlayCache, OnlyPlotScaleAndOrderedCrossoversInvalidateGeometry)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(999, 401);
    SpectrogramViewTestAccess access(view);
    EXPECT_EQ(access.builds(), 0U);
    const auto originalFills = access.responses().fills;
    const auto originalColours = access.responses().colours;
    EXPECT_EQ(access.builds(), 1U);

    const juce::Image image(juce::Image::ARGB, 1024, 512, true);
    juce::Graphics graphics(image);
    view.setPositionCurve(StereoPositionCurve {});
    view.setPositionCurve(StereoPositionCurve(10.0, 32.0));
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        access.hover(band);
        view.advance(0.05);
        view.paint(graphics);
    }
    setParameterInGesture(*processor.getParameters().getParameter(widthParameterId(0)), 0.5F);
    setParameterInGesture(*processor.getParameters().getParameter(midGainParameterId(1)), 6.0F);
    setParameterInGesture(*processor.getParameters().getParameter(soloParameterId(2)), 1.0F);
    view.setRenderingBypassed(true);
    view.setRenderingUnavailable(true);
    view.advance(0.1);
    view.paint(graphics);
    EXPECT_EQ(access.builds(), 1U);
    EXPECT_EQ(access.responses().fills, originalFills);
    EXPECT_EQ(access.responses().colours, originalColours);

    view.setFrequencyScale(FrequencyScale::mel);
    EXPECT_NE(access.responses().fills, originalFills);
    EXPECT_EQ(access.responses().colours, originalColours);
    EXPECT_EQ(access.builds(), 2U);
    view.setFrequencyScale(FrequencyScale::mel);
    static_cast<void>(access.responses());
    EXPECT_EQ(access.builds(), 2U);

    setParameterInGesture(*processor.getParameters().getParameter(crossoverParameterId(0)), 160.0F);
    EXPECT_NE(access.responses().colours, originalColours);
    EXPECT_EQ(access.builds(), 3U);
    view.setSize(1000, 401);
    static_cast<void>(access.responses());
    EXPECT_EQ(access.builds(), 4U);
    view.setSize(1000, 450);
    static_cast<void>(access.responses());
    EXPECT_EQ(access.builds(), 5U);

    // Ordering clamps each named crossover above its predecessor rather than sorting the raw values.
    // Distinct Mid requests below its lower limit therefore share exactly the same ordered projection.
    auto& middleCrossover = *processor.getParameters().getParameter(crossoverParameterId(1));
    setParameterInGesture(middleCrossover, MIN_FREQUENCY);
    const float firstRequest = plainValue(middleCrossover);
    const auto orderedGeometry = access.responses().fills;
    const auto orderedCrossovers = access.responses().crossovers;
    EXPECT_EQ(access.builds(), 6U);
    setParameterInGesture(middleCrossover, MIN_FREQUENCY * 2.0F);
    EXPECT_GT(plainValue(middleCrossover), firstRequest);
    Crossovers actualRequests {};
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        actualRequests.at(crossover)
            = plainValue(*processor.getParameters().getParameter(crossoverParameterId(crossover)));
    }
    ASSERT_EQ(orderCrossovers(actualRequests), orderedCrossovers);
    EXPECT_EQ(access.responses().fills, orderedGeometry);
    EXPECT_EQ(access.builds(), 6U);
}

/// Idle paints and crossover automation defer all response geometry until a band highlight needs it.
TEST(SpectrogramOverlayCache, UnhoveredPaintsDoNotBuildOrRefreshResponseGeometry)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(999, 401);
    SpectrogramViewTestAccess access(view);
    const juce::Image image(juce::Image::ARGB, 1024, 512, true);
    juce::Graphics graphics(image);
    auto& crossover = *processor.getParameters().getParameter(crossoverParameterId(0));
    view.paint(graphics);
    EXPECT_EQ(access.builds(), 0U);
    setParameterInGesture(crossover, 120.0F);
    view.paint(graphics);
    view.setFrequencyScale(FrequencyScale::mel);
    view.setSize(1000, 450);
    view.paint(graphics);
    EXPECT_EQ(access.builds(), 0U);

    access.hover(0);
    view.advance(0.05);
    view.paint(graphics);
    EXPECT_EQ(access.builds(), 1U);
    view.advance(0.05);
    view.paint(graphics);
    EXPECT_EQ(access.builds(), 1U);

    access.hover(std::nullopt);
    view.advance(1.0);
    view.paint(graphics);
    setParameterInGesture(crossover, 160.0F);
    view.paint(graphics);
    view.setFrequencyScale(FrequencyScale::logarithmic);
    view.setSize(999, 401);
    view.paint(graphics);
    EXPECT_EQ(access.builds(), 1U);

    access.hover(1);
    view.advance(0.05);
    view.paint(graphics);
    EXPECT_EQ(access.builds(), 2U);
}

/// Sparse CPU path observations verify all four complementary responses and the exact final curve sample.
TEST(SpectrogramOverlayCache, ResponsesMatchTheLowpassesAndReachTheExactRightEndpoint)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(999, 401);
    SpectrogramViewTestAccess access(view);
    for (const auto scale : {FrequencyScale::logarithmic, FrequencyScale::mel}) {
        view.setFrequencyScale(scale);
        const auto& responses = access.responses();
        const auto plot = view.getPlotBounds();
        std::array<std::vector<juce::Point<float>>, BAND_COUNT> samples;
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            ASSERT_FALSE(responses.strokes.at(band).isEmpty());
            juce::Path::Iterator iterator(responses.fills.at(band));
            while (iterator.next()) {
                if (iterator.elementType == juce::Path::Iterator::startNewSubPath
                    || iterator.elementType == juce::Path::Iterator::lineTo)
                {
                    samples.at(band).emplace_back(iterator.x1, iterator.y1);
                }
            }
            // The final two lines close the filled area along the baseline.
            ASSERT_GE(samples.at(band).size(), 4U);
            samples.at(band).resize(samples.at(band).size() - 2);
            EXPECT_FLOAT_EQ(samples.at(band).front().x, plot.getX());
            EXPECT_FLOAT_EQ(samples.at(band).back().x, plot.getRight());
            ASSERT_EQ(samples.at(band).size(), samples.front().size());
        }
        for (std::size_t sample = 0; sample < samples.front().size(); ++sample) {
            const float pixel = samples.front().at(sample).x;
            const double frequency
                = shareToFrequency(static_cast<double>((pixel - plot.getX()) / plot.getWidth()), scale);
            std::array<double, BAND_COUNT + 1> lowpasses {};
            for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
                lowpasses.at(crossover + 1)
                    = crossoverLowpassMagnitude(frequency, static_cast<double>(responses.crossovers.at(crossover)));
            }
            lowpasses.back() = 1.0;
            double sum = 0.0;
            for (std::size_t band = 0; band < BAND_COUNT; ++band) {
                const double magnitude = static_cast<double>(plot.getBottom() - samples.at(band).at(sample).y)
                    / static_cast<double>(plot.getHeight() * 0.8F);
                EXPECT_NEAR(magnitude, lowpasses.at(band + 1) - lowpasses.at(band), 1.0e-6);
                sum += magnitude;
            }
            EXPECT_NEAR(sum, 1.0, 1.0e-6);
        }
    }
}
}  // namespace esgrove::image_control::test
