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
#include "ProcessingState.h"
#include "ui/BandStrip.h"
#include "ui/DisplayDisclosure.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/SettingsPanel.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"

#include <gtest/gtest.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
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

    /// Applies the range-update path used by display refreshes, without requiring a VBlank or graphics frame.
    void updateBandRanges()
    {
        editor.updateBandRanges();
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

/// Measures the widest actual expanded heading independently of its responsive layout decision.
int widestBandHeading(const ImageControlEditor& editor)
{
    int widest = 0;
    for (const auto* child : editor.getChildren()) {
        const auto* strip = dynamic_cast<const BandStrip*>(child);
        if (strip == nullptr) {
            continue;
        }
        int width = 20 + 20 + 60 + 6 + 8 * 2;
        for (auto* control : strip->getChildren()) {
            auto* label = dynamic_cast<juce::Label*>(control);
            if (label == nullptr
                || (strip->getTitle() != label->getText() + " band controls" && !label->getText().contains("Hz")))
            {
                continue;
            }
            auto& lookAndFeel = label->getLookAndFeel();
            width += static_cast<int>(std::ceil(
                         juce::GlyphArrangement::getStringWidth(lookAndFeel.getLabelFont(*label), label->getText())
                     ))
                + lookAndFeel.getLabelBorderSize(*label).getLeftAndRight();
        }
        widest = std::max(widest, width);
    }
    return widest;
}

/// Checks every production frequency label has the expected shared row after a live crossover update.
void expectBandRangeRow(const ImageControlEditor& editor, const int expectedY)
{
    int rangeCount = 0;
    for (const auto* child : editor.getChildren()) {
        const auto* strip = dynamic_cast<const BandStrip*>(child);
        if (strip == nullptr) {
            continue;
        }
        for (const auto* control : strip->getChildren()) {
            if (const auto* label = dynamic_cast<const juce::Label*>(control);
                label != nullptr && label->getText().contains("Hz"))
            {
                ++rangeCount;
                EXPECT_TRUE(label->isVisible());
                EXPECT_EQ(label->getY(), expectedY);
            }
        }
    }
    EXPECT_EQ(rangeCount, static_cast<int>(BAND_COUNT));
}

/// Sets host crossover values without creating local editing history.
void setHeadingCrossovers(ImageControlProcessor& processor, const Crossovers& crossovers)
{
    for (std::size_t index = 0; index < CROSSOVER_COUNT; ++index) {
        setParameterInGesture(
            *processor.getParameters().getParameter(crossoverParameterId(index)), crossovers.at(index)
        );
    }
}

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

/// Crossover text changes move every heading together at unchanged editor bounds, including while compacted.
TEST(ImageControlEditorLayout, CrossoverChangesSynchronizeEveryFrequencyRangeRow)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    ImageControlEditorTestAccess access(editor);
    access.disableDisplayCallbacks();
    const Crossovers shortRanges {80.0F, 120.0F, 800.0F};
    const Crossovers longRanges {1250.0F, 10'000.0F, 16'000.0F};
    setHeadingCrossovers(processor, shortRanges);
    access.updateBandRanges();
    const int requiredWidth = widestBandHeading(editor);
    int stripSpace = 0;
    for (const auto* child : editor.getChildren()) {
        if (dynamic_cast<const BandStrip*>(child) != nullptr) {
            stripSpace += child->getWidth();
        }
    }
    editor.setSize(editor.getWidth() - stripSpace + static_cast<int>(BAND_COUNT) * requiredWidth, editor.getHeight());
    juce::Button* toggle = nullptr;
    for (auto* child : editor.getChildren()) {
        if (child->getComponentID() == "band-controls-disclosure") {
            toggle = dynamic_cast<juce::Button*>(child);
        }
    }
    ASSERT_NE(toggle, nullptr);
    const auto bounds = editor.getBounds();
    for (const bool compact : {false, true}) {
        if (compact) {
            toggle->onClick();
        }
        for (const bool longText : {false, true, false}) {
            const auto& crossovers = longText ? longRanges : shortRanges;
            setHeadingCrossovers(processor, crossovers);
            access.updateBandRanges();
            EXPECT_EQ(editor.getBounds(), bounds);
            if (compact) {
                toggle->onClick();
            }
            expectBandRangeRow(editor, longText ? 30 : 10);
            if (compact) {
                toggle->onClick();
            }
        }
        if (compact) {
            toggle->onClick();
        }
    }
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// The real settings toggle grows only the plot and preserves window geometry, sound, and saved full-strip sizing.
/// Host state and editor reopening retain the opt-in, while older states deterministically restore standard axes.
TEST(ImageControlEditorLayout, MaximizedSpectrogramIsOptionalPersistentAndIndependentOfSound)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    EXPECT_FALSE(static_cast<bool>(state.getProperty(MAXIMIZE_SPECTROGRAM_PROPERTY)));
    ImageControlEditor editor(processor);
    ImageControlEditorTestAccess access(editor);
    access.disableDisplayCallbacks();
    const SettingsPanel settings(state);
    juce::ToggleButton* toggle = nullptr;
    SpatialSpectrogramView* view = nullptr;
    juce::Component* disclosure = nullptr;
    for (auto* child : editor.getChildren()) {
        if (auto* candidate = dynamic_cast<SpatialSpectrogramView*>(child)) {
            view = candidate;
        }
        if (child->getComponentID() == "spectrogram-disclosure") {
            disclosure = child;
        }
    }
    // The toggle lives in the settings viewport's content, not directly on the panel.
    const auto* viewport = dynamic_cast<juce::Viewport*>(settings.getChildComponent(0));
    ASSERT_NE(viewport, nullptr);
    for (auto* child : viewport->getViewedComponent()->getChildren()) {
        if (child->getComponentID() == MAXIMIZE_SPECTROGRAM_PROPERTY) {
            toggle = dynamic_cast<juce::ToggleButton*>(child);
        }
    }
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(view, nullptr);
    ASSERT_NE(disclosure, nullptr);
    const auto standard = view->getPlotBounds();
    const auto window = editor.getBounds();
    const auto section = view->getBounds();
    const auto bandHeight = state.getProperty(BAND_SECTION_HEIGHT_PROPERTY);
    const auto sound = processor.getSoundSnapshot();
    for (const bool maximized : {true, false, true}) {
        toggle->setToggleState(maximized, juce::sendNotificationSync);
        access.applySettings();
        EXPECT_EQ(view->getPlotBounds(), maximized ? view->getLocalBounds().toFloat().reduced(2.0F) : standard);
        EXPECT_EQ(editor.getBounds(), window);
        EXPECT_EQ(view->getBounds(), section);
        EXPECT_EQ(state.getProperty(BAND_SECTION_HEIGHT_PROPERTY), bandHeight);
        EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
        EXPECT_FALSE(processor.getHistory().canUndo());
        const auto* action = dynamic_cast<DisplayDisclosure*>(disclosure);
        ASSERT_NE(action, nullptr);
        EXPECT_NEAR(
            static_cast<float>(action->getY()) + action->getIndicatorBounds().getCentreY(),
            static_cast<float>(view->getY()) + view->getPlotBounds().getY(),
            0.5F
        );
    }
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    ImageControlProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    EXPECT_TRUE(static_cast<bool>(restored.getEditorState().getProperty(MAXIMIZE_SPECTROGRAM_PROPERTY)));
    const ImageControlEditor reopened(restored);
    for (const auto* child : reopened.getChildren()) {
        if (const auto* plot = dynamic_cast<const SpatialSpectrogramView*>(child)) {
            EXPECT_EQ(plot->getPlotBounds(), plot->getLocalBounds().toFloat().reduced(2.0F));
        }
    }
    const auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
    ASSERT_NE(xml, nullptr);
    auto* editorState = xml->getChildByName("EditorState");
    ASSERT_NE(editorState, nullptr);
    editorState->removeAttribute(MAXIMIZE_SPECTROGRAM_PROPERTY);
    juce::AudioProcessor::copyXmlToBinary(*xml, saved);
    processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    access.applySettings();
    EXPECT_FALSE(static_cast<bool>(state.getProperty(MAXIMIZE_SPECTROGRAM_PROPERTY)));
    EXPECT_FALSE(toggle->getToggleState());
    EXPECT_EQ(view->getPlotBounds(), standard);
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
