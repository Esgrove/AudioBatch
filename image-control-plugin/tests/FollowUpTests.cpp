/// Pins non-finite notification rejection with real JUCE attachments and stale correlation expiry.
/// Host lifecycle and display-generation transitions must not leave unsafe UI values or old meter readings.
/// CPU-only overlay and editor tests pin response caching and visibility-driven drain-only analysis.

#include "analysis/SpatialAnalyzer.h"
#include "analysis/StereoSampleFifo.h"
#include "dsp/BandCorrelation.h"
#include "dsp/CrossoverDesign.h"
#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"

#include <gtest/gtest.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <thread>
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

/// Provides worker synchronization and observations while leaving its single-consumer queue ownership intact.
struct ImageControlEditorTestAccess {
    /// Selects the real editor whose settings and background worker are observed.
    explicit ImageControlEditorTestAccess(ImageControlEditor& observed) : editor(observed) {}

    /// Leaves worker shutdown and component destruction to the observed editor.
    ~ImageControlEditorTestAccess() = default;

    ImageControlEditorTestAccess(const ImageControlEditorTestAccess&) = delete;
    ImageControlEditorTestAccess(ImageControlEditorTestAccess&&) = delete;
    ImageControlEditorTestAccess& operator=(const ImageControlEditorTestAccess&) = delete;
    ImageControlEditorTestAccess& operator=(ImageControlEditorTestAccess&&) = delete;

    /// Reads the effective settings last synchronously handed to the worker.
    [[nodiscard]] bool analyzes() const
    {
        return editor.analysisSettings.analyze;
    }

    /// Reads particles that have reached the renderer, which need no OpenGL context to accumulate.
    [[nodiscard]] std::size_t pendingParticles()
    {
        return editor.spectrogramRenderer.pendingParticleCount();
    }

    /// Waits for a real worker pass instead of racing the FIFO's consumer index.
    [[nodiscard]] bool finishPass()
    {
        const auto ticket = editor.spectrogramWorker.requestPass();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (editor.spectrogramWorker.completedPassCount() < ticket) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    /// Detaches graphics and VBlank before host visibility is exercised, so tests need no GPU or refresh callbacks.
    void disableDisplayCallbacks()
    {
        editor.openGLContext.detach();
        editor.vblankAttachment = juce::VBlankAttachment {};
    }

    /// Applies the same settings path used by display refreshes, without requesting a graphics frame.
    void applySettings()
    {
        editor.applyViewSettings();
    }

    /// Editor under observation, which outlives this accessor.
    ImageControlEditor& editor;
};

namespace
{
/// Observes the actual notification payload rather than APVTS's reread of the current value.
class ParameterValueListener final : public juce::AudioProcessorParameter::Listener
{
public:
    /// Records whether even one non-finite notification escaped the public parameter boundary.
    void parameterValueChanged(int /*index*/, const float value) override
    {
        ++notifications;
        allFinite = allFinite && std::isfinite(value);
    }

    /// Gesture boundaries are unrelated to invalid-value rejection.
    void parameterGestureChanged(int /*index*/, bool /*starting*/) override {}

    /// Number of value notifications received.
    int notifications {0};
    /// Whether all received payloads were finite.
    bool allFinite {true};
};

/// Fills and processes a coherent stereo high-frequency signal that yields a valid High-band reading.
void publishCorrelation(ImageControlProcessor& processor)
{
    juce::AudioBuffer<float> block(2, 256);
    juce::MidiBuffer midi;
    for (int pass = 0; pass < 64; ++pass) {
        for (int sample = 0; sample < 256; ++sample) {
            const float value = sample % 2 == 0 ? 0.25F : -0.25F;
            block.setSample(0, sample, value);
            block.setSample(1, sample, value);
        }
        processor.processBlock(block, midi);
    }
}

/// Checks the explicit unavailable state without treating NaN as a numeric correlation.
void expectUnavailable(const BandCorrelations& readings)
{
    for (const float value : readings) {
        EXPECT_TRUE(std::isnan(value));
    }
}
}  // namespace

TEST(ParameterNotifications, RejectedValuesNeverReachListenersOrAttachments)
{
    ImageControlProcessor processor;
    auto& pan = *processor.getParameters().getParameter(panParameterId(0));
    auto& solo = *processor.getParameters().getParameter(soloParameterId(0));
    juce::Slider slider;
    juce::ToggleButton button;
    const juce::SliderParameterAttachment sliderAttachment(pan, slider);
    const juce::ButtonParameterAttachment buttonAttachment(solo, button);
    ParameterValueListener panListener;
    ParameterValueListener soloListener;
    pan.addListener(&panListener);
    solo.addListener(&soloListener);
    pan.setValueNotifyingHost(pan.convertTo0to1(40.0F));
    solo.setValueNotifyingHost(1.0F);
    ASSERT_EQ(panListener.notifications, 1);
    ASSERT_EQ(soloListener.notifications, 1);
    for (const float invalid :
         {
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(),
         })
    {
        pan.setValueNotifyingHost(invalid);
        solo.setValueNotifyingHost(invalid);
        pan.sendValueChangedMessageToListeners(invalid);
        solo.sendValueChangedMessageToListeners(invalid);
        EXPECT_NEAR(slider.getValue(), 40.0, 0.001);
        EXPECT_TRUE(button.getToggleState());
        EXPECT_TRUE(std::isfinite(pan.getValue()));
        EXPECT_FLOAT_EQ(solo.getValue(), 1.0F);
    }
    EXPECT_EQ(panListener.notifications, 1);
    EXPECT_EQ(soloListener.notifications, 1);
    EXPECT_TRUE(panListener.allFinite);
    EXPECT_TRUE(soloListener.allFinite);
    pan.removeListener(&panListener);
    solo.removeListener(&soloListener);
}

TEST(CorrelationLifecycle, StoppedAndHiddenViewsCannotShowAnOldReading)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(48'000.0, 256);
    processor.setCorrelationEnabled(true);
    publishCorrelation(processor);
    EXPECT_NEAR(processor.getBandCorrelations().back(), 1.0F, 1.0e-6F);
    processor.setCorrelationEnabled(false);
    expectUnavailable(processor.getBandCorrelations());
    processor.setCorrelationEnabled(true);
    expectUnavailable(processor.getBandCorrelations());
    juce::AudioBuffer<float> shortBlock(2, 1);
    shortBlock.clear();
    juce::MidiBuffer midi;
    processor.processBlock(shortBlock, midi);
    expectUnavailable(processor.getBandCorrelations());
    publishCorrelation(processor);
    EXPECT_NEAR(processor.getBandCorrelations().back(), 1.0F, 1.0e-6F);
    processor.releaseResources();
    expectUnavailable(processor.getBandCorrelations());
}

TEST(CorrelationLifecycle, SnapshotsExposeTransitionsEvenWhenPublicationResumesBeforeTheNextRead)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(48'000.0, 256);
    processor.setCorrelationEnabled(true);
    publishCorrelation(processor);
    auto snapshot = processor.getBandCorrelationSnapshot();
    ASSERT_TRUE(std::isfinite(snapshot.readings.back()));
    processor.setCorrelationEnabled(true);
    EXPECT_EQ(processor.getBandCorrelationSnapshot().generation, snapshot.generation);

    processor.reset();
    publishCorrelation(processor);
    auto next = processor.getBandCorrelationSnapshot();
    EXPECT_NE(next.generation, snapshot.generation);
    EXPECT_NEAR(next.readings.back(), 1.0F, 1.0e-6F);
    snapshot = next;

    processor.prepareToPlay(96'000.0, 256);
    publishCorrelation(processor);
    next = processor.getBandCorrelationSnapshot();
    EXPECT_NE(next.generation, snapshot.generation);
    EXPECT_NEAR(next.readings.back(), 1.0F, 1.0e-6F);
    snapshot = next;

    // No callback or display read observes the disabled interval.
    processor.setCorrelationEnabled(false);
    processor.setCorrelationEnabled(true);
    publishCorrelation(processor);
    next = processor.getBandCorrelationSnapshot();
    EXPECT_NE(next.generation, snapshot.generation);
    EXPECT_NEAR(next.readings.back(), 1.0F, 1.0e-6F);
    snapshot = next;
    processor.releaseResources();
    next = processor.getBandCorrelationSnapshot();
    EXPECT_NE(next.generation, snapshot.generation);
    expectUnavailable(next.readings);
}

TEST(CorrelationLifecycle, MissingCallbacksExpireWithoutReadingOrMutatingAudioHistory)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(48'000.0, 256);
    processor.setCorrelationEnabled(true);
    publishCorrelation(processor);
    ASSERT_TRUE(std::isfinite(processor.getBandCorrelations().back()));
    juce::Thread::sleep(300);
    expectUnavailable(processor.getBandCorrelations());
    publishCorrelation(processor);
    EXPECT_NEAR(processor.getBandCorrelations().back(), 1.0F, 1.0e-6F);
    processor.reset();
    expectUnavailable(processor.getBandCorrelations());
    processor.releaseResources();
}

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

/// Host visibility updates the real worker without a VBlank or a GPU, retaining drain-only behavior while hidden.
TEST(ImageControlEditorLifecycle, VisibilityDisablesAndReenablesAnalysisWithoutDisplayRefreshes)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    ImageControlEditorTestAccess access(editor);
    access.disableDisplayCallbacks();
    EXPECT_FALSE(editor.isShowing());
    EXPECT_FALSE(access.analyzes());
    ASSERT_TRUE(access.finishPass());

    std::vector<float> tone(8192);
    for (std::size_t sample = 0; sample < tone.size(); ++sample) {
        tone.at(sample) = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(sample) / ANALYZER_DEFAULT_SAMPLE_RATE)
        );
    }
    auto& fifo = processor.getAnalyzerFifo();
    ASSERT_EQ(fifo.push(tone, tone), static_cast<int>(tone.size()));
    ASSERT_TRUE(access.finishPass());
    EXPECT_EQ(access.pendingParticles(), 0U);

    juce::Component host;
    host.addToDesktop(0);
    host.setVisible(true);
    host.addAndMakeVisible(editor);
    ASSERT_TRUE(editor.isShowing());
    EXPECT_TRUE(access.analyzes());
    ASSERT_TRUE(access.finishPass());
    ASSERT_EQ(fifo.push(tone, tone), static_cast<int>(tone.size()));
    ASSERT_TRUE(access.finishPass());
    EXPECT_GT(access.pendingParticles(), 0U);

    const auto shownEpoch = fifo.getEpoch();
    host.setVisible(false);
    EXPECT_FALSE(access.analyzes());
    EXPECT_NE(fifo.getEpoch(), shownEpoch);
    ASSERT_TRUE(access.finishPass());
    EXPECT_EQ(access.pendingParticles(), 0U);
    ASSERT_EQ(fifo.push(tone, tone), static_cast<int>(tone.size()));
    ASSERT_TRUE(access.finishPass());
    EXPECT_EQ(access.pendingParticles(), 0U);
    const std::vector<float> full(static_cast<std::size_t>(ANALYZER_FIFO_FRAMES - 1), 0.0F);
    EXPECT_EQ(fifo.push(full, full), ANALYZER_FIFO_FRAMES - 1);
    ASSERT_TRUE(access.finishPass());

    // Resuming invalidates a hidden backlog without analyzing it under the visible settings.
    ASSERT_EQ(fifo.push(tone, tone), static_cast<int>(tone.size()));
    const auto hiddenEpoch = fifo.getEpoch();
    host.setVisible(true);
    EXPECT_TRUE(access.analyzes());
    EXPECT_NE(fifo.getEpoch(), hiddenEpoch);
    ASSERT_TRUE(access.finishPass());
    EXPECT_EQ(access.pendingParticles(), 0U);
    ASSERT_EQ(fifo.push(tone, tone), static_cast<int>(tone.size()));
    ASSERT_TRUE(access.finishPass());
    EXPECT_GT(access.pendingParticles(), 0U);

    editor.setVisible(false);
    EXPECT_FALSE(access.analyzes());
    editor.setVisible(true);
    EXPECT_TRUE(access.analyzes());
    host.removeChildComponent(&editor);
    EXPECT_FALSE(editor.isShowing());
    EXPECT_FALSE(access.analyzes());
    ASSERT_TRUE(access.finishPass());
    EXPECT_EQ(access.pendingParticles(), 0U);
}

/// Hidden and bypassed display settings remain authoritative when a peer becomes visible before another refresh.
TEST(ImageControlEditorLifecycle, VisibilityCannotOverrideHiddenOrBypassedAnalysis)
{
    for (const auto* property : {HIDE_SPECTROGRAM_PROPERTY, BYPASS_RENDERING_PROPERTY}) {
        ImageControlProcessor processor;
        processor.getEditorState().setProperty(property, true, nullptr);
        ImageControlEditor editor(processor);
        ImageControlEditorTestAccess access(editor);
        access.disableDisplayCallbacks();
        editor.setVisible(false);
        editor.addToDesktop(0);
        editor.setVisible(true);
        EXPECT_TRUE(editor.isShowing());
        EXPECT_FALSE(access.analyzes());
        editor.setVisible(false);
        processor.getEditorState().setProperty(property, false, nullptr);
        editor.setVisible(true);
        EXPECT_TRUE(access.analyzes());
        access.applySettings();
        EXPECT_TRUE(access.analyzes());
        processor.getEditorState().setProperty(property, true, nullptr);
        access.applySettings();
        EXPECT_FALSE(access.analyzes());
    }
}
}  // namespace esgrove::image_control::test
