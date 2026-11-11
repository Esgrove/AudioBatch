/// Tests the mouse drags on SpatialSpectrogramView with synthesized mouse events through its real handlers:
/// moving a middle band moves both its crossovers, moving the Low band moves only its upper crossover,
/// a band move stops at the minimum ratio to the crossover that stays,
/// Ctrl or Cmd with a vertical drag changes the band's mid gain and, with Link, its side gain,
/// Alt or Option with a vertical drag changes its side gain, and the wheel changes the gains by 0.1 dB a step.
/// Also tests ControlSlider and BandStrip gesture balance, independent Width and Recover attachments,
/// accessible processing controls, and accessible meter values.
/// The real editor composition verifies Mono actions, compact layout, and saved expanded sizing.
/// Also tests that the stereo position curve overlay shows after a curve change and fades out.
/// Also tests the particles SpectrogramParticleBuilder prepares for the GPU renderer:
/// which points become particles, how bright and large they are under the spectral tilt and the auto-scale,
/// where they are placed, and the frequency palette.

#include "analysis/SpatialAnalyzer.h"
#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/SettingsPanel.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Records the host gesture boundaries of one parameter without changing its values.
class GestureCounter final : public juce::AudioProcessorParameter::Listener
{
public:
    /// Accepts value notifications without using them for the gesture contract.
    void parameterValueChanged(int /*parameterIndex*/, float /*value*/) override {}
    /// Counts the exact start/end sequence, including overlapping starts.
    void parameterGestureChanged(int /*parameterIndex*/, const bool starting) override
    {
        events.push_back(starting);
    }

    /// Ordered host gesture boundaries, true for begin and false for end.
    std::vector<bool> events;
};

/// Finds a real child control by its accessible title, or null when absent.
/// The component hierarchy owns the result, which must be checked and used only while that hierarchy retains it.
juce::Component* childWithTitle(const juce::Component& parent, const juce::String& title)
{
    std::vector<juce::Component*> pending;
    for (auto* child : parent.getChildren()) {
        pending.push_back(child);
    }
    while (!pending.empty()) {
        auto* child = pending.back();
        pending.pop_back();
        if (child->getTitle() == title) {
            return child;
        }
        for (auto* descendant : child->getChildren()) {
            pending.push_back(descendant);
        }
    }
    return nullptr;
}

/// Finds a button by its visible text, or null when absent, without relying on private editor fields.
/// The parent owns the result. Check it before use and use it only while the parent retains the button.
juce::Button* buttonWithText(const juce::Component& parent, const juce::String& text)
{
    std::vector<juce::Component*> pending;
    for (auto* child : parent.getChildren()) {
        pending.push_back(child);
    }
    while (!pending.empty()) {
        auto* child = pending.back();
        pending.pop_back();
        if (auto* button = dynamic_cast<juce::Button*>(child); button != nullptr && button->getButtonText() == text) {
            return button;
        }
        for (auto* nested : child->getChildren()) {
            pending.push_back(nested);
        }
    }
    return nullptr;
}

/// Size of the view in the tests, in pixels.
constexpr int VIEW_WIDTH {1000};
/// Height of the view in the tests, in pixels.
constexpr int VIEW_HEIGHT {400};

/// Returns the plain value of a parameter of a processor.
float parameterValue(ImageControlProcessor& processor, const juce::String& parameterId)
{
    return plainValue(*processor.getParameters().getParameter(parameterId));
}

/// Returns a mouse event on a component at a position, for a drag that started at another position.
juce::MouseEvent mouseEvent(
    juce::Component& view,
    const juce::Point<float> position,
    const juce::Point<float> downPosition,
    const juce::ModifierKeys modifiers
)
{
    const auto now = juce::Time::getCurrentTime();
    return {
        juce::Desktop::getInstance().getMainMouseSource(),
        position,
        modifiers,
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        &view,
        &view,
        now,
        downPosition,
        now,
        1,
        position != downPosition,
    };
}

/// Returns a notched mouse wheel event that turns the wheel by a number of Windows notches, positive for up.
juce::MouseWheelDetails notch(const float notches)
{
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = notches * 60.0F / 256.0F;
    return wheel;
}

/// Drags on the view from a start to an end position with modifier keys held, through its mouse handlers.
void dragOnView(
    SpatialSpectrogramView& view,
    const juce::Point<float> start,
    const juce::Point<float> end,
    const juce::ModifierKeys modifiers = {}
)
{
    const auto buttons = modifiers.withFlags(juce::ModifierKeys::leftButtonModifier);
    view.mouseDown(mouseEvent(view, start, start, buttons));
    view.mouseDrag(mouseEvent(view, end, start, buttons));
    view.mouseUp(mouseEvent(view, end, start, modifiers));
}

/// Returns the x coordinate of a frequency in the view's plot on the logarithmic axis.
float xOf(
    const SpatialSpectrogramView& view,
    const double frequency,
    const FrequencyScale scale = FrequencyScale::logarithmic
)
{
    const auto plot = view.getPlotBounds();
    return plot.getX() + static_cast<float>(frequencyToShare(frequency, scale)) * plot.getWidth();
}

/// Sets the real pointer-hover target without taking keyboard focus away from another editor control.
void hoverFrequency(
    SpatialSpectrogramView& view,
    const double frequency,
    const FrequencyScale scale = FrequencyScale::logarithmic
)
{
    const juce::Point<float> position {xOf(view, frequency, scale), view.getPlotBounds().getCentreY()};
    view.mouseMove(mouseEvent(view, position, position, {}));
}

/// Returns the frequency an octave count above another.
double octavesAbove(const double frequency, const double octaves)
{
    return frequency * std::exp2(octaves);
}

/// Time one analyzed frame stands for in the tests, in seconds: a hop of 512 samples at 48 kHz.
constexpr double FRAME_SECONDS {512.0 / 48'000.0};

/// Returns an analyzed point at a frequency, a stereo position, and a level.
SpectralPoint point(const float frequency, const float position, const float level)
{
    return {
        .frequency = frequency,
        .reassignedFrequency = frequency,
        .spacing = 10.0F,
        .position = position,
        .level = level,
        .coherence = 1.0F,
    };
}

/// Returns the level in power of a point that is a number of dB below full scale.
float levelFromDecibels(const float decibels)
{
    return std::pow(10.0F, decibels / 10.0F);
}

/// Returns the summed colour channels of a particle, which follow its weight.
float brightness(const SpectrogramParticle& particle)
{
    return particle.red + particle.green + particle.blue;
}
}  // namespace

TEST(SpectrogramView, OverlayEqualityTracksEveryVisibleValue)
{
    const SpectrogramOverlayState original;
    EXPECT_EQ(original, SpectrogramOverlayState {});

    const auto expectChanged = [&original](const SpectrogramOverlayState& changed) {
        EXPECT_NE(original, changed);
        EXPECT_NE(changed, original);
        EXPECT_EQ(changed, changed);
    };
    auto changed = original;
    changed.positionCurve = StereoPositionCurve(13.0, 32.0);
    expectChanged(changed);
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        changed = original;
        changed.bandHighlights.at(band) = 0.1F;
        expectChanged(changed);
        changed = original;
        changed.bandWidths.at(band) = 0.1;
        expectChanged(changed);
        changed = original;
        changed.mutedBands.at(band) = true;
        expectChanged(changed);
    }
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        changed = original;
        changed.crossoverHighlights.at(crossover) = 0.1F;
        expectChanged(changed);
        changed = original;
        changed.crossovers.at(crossover) = 100.0F;
        expectChanged(changed);
    }
    changed = original;
    changed.positionCurveOpacity = 0.1F;
    expectChanged(changed);
    changed = original;
    changed.renderingBypassed = true;
    expectChanged(changed);
    changed = original;
    changed.renderingUnavailable = true;
    expectChanged(changed);
}

TEST(SpectrogramView, DraggingAMiddleBandMovesBothItsCrossovers)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    // The Low Mid band spans 100 Hz to 500 Hz. Dragging from its middle up by one octave moves both edges.
    const float centreY = view.getPlotBounds().getCentreY();
    const float start = xOf(view, 220.0);
    const float end = xOf(view, octavesAbove(220.0, 1.0));
    dragOnView(view, {start, centreY}, {end, centreY});

    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 200.0F, 2.0F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), 1000.0F, 10.0F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(2)), 5000.0F, 1.0F);
}

TEST(SpectrogramView, DraggingTheLowBandMovesOnlyItsUpperCrossover)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const float centreY = view.getPlotBounds().getCentreY();
    dragOnView(view, {xOf(view, 50.0), centreY}, {xOf(view, octavesAbove(50.0, 0.5)), centreY});

    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), octavesAbove(100.0, 0.5), 2.0F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), 500.0F, 1.0F);
}

TEST(SpectrogramView, BandMoveStopsAtTheMinimumRatioToTheCrossoverThatStays)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    // The High Mid band spans 500 Hz to 5 kHz.
    // A drag far to the left stops when 500 Hz reaches a third of an octave above the low crossover at 100 Hz,
    // and the band keeps its width on the axis.
    const float centreY = view.getPlotBounds().getCentreY();
    dragOnView(view, {xOf(view, 1500.0), centreY}, {xOf(view, 25.0), centreY});

    const float lowest = 100.0F * MIN_CROSSOVER_RATIO;
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 100.0F, 0.5F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), lowest, 1.0F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(2)), lowest * 10.0F, 10.0F);
}

/// Boundary keys use ten percent ordinarily and one percent with Shift, clamped in Hz in either projection.
TEST(SpectrogramView, HoveredCrossoverKeysUseFrequencyRelativeStepsOnEveryAxis)
{
    for (const auto scale : {FrequencyScale::logarithmic, FrequencyScale::mel}) {
        for (const auto index : {std::size_t {0}, std::size_t {2}}) {
            for (const bool fine : {false, true}) {
                ImageControlProcessor processor;
                SpatialSpectrogramView view(
                    processor.getParameters(), processor.getEditorState(), processor.getHistory()
                );
                view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
                view.setFrequencyScale(scale);
                const float before = parameterValue(processor, crossoverParameterId(index));
                hoverFrequency(view, static_cast<double>(before), scale);
                const float step = (index == 0 ? 10.0F : 500.0F) * (fine ? 0.1F : 1.0F);
                const auto modifiers = fine ? juce::ModifierKeys::shiftModifier : juce::ModifierKeys::noModifiers;
                EXPECT_TRUE(view.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, modifiers, 0)));
                EXPECT_NEAR(parameterValue(processor, crossoverParameterId(index)), before + step, 0.005F);
                hoverFrequency(view, static_cast<double>(before + step), scale);
                EXPECT_TRUE(view.getTooltip().contains("Shift+Left/Right"));
            }
        }
    }
}

/// Both coarse and fine steps apply their own absolute clamp, so Shift cannot bypass the minimum one-Hz step.
TEST(SpectrogramView, KeyboardFrequencyStepsClampAfterSelectingCoarseOrFineShare)
{
    for (const bool fine : {false, true}) {
        for (const bool high : {false, true}) {
            ImageControlProcessor processor;
            SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
            view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
            const std::size_t edge = high ? 2 : 0;
            const float frequency = high ? 15'000.0F : 50.0F;
            setParameterInGesture(*processor.getParameters().getParameter(crossoverParameterId(edge)), frequency);
            hoverFrequency(view, static_cast<double>(frequency));
            const float highStep = fine ? 150.0F : 1000.0F;
            const float lowStep = fine ? 1.0F : 5.0F;
            const float step = high ? highStep : lowStep;
            EXPECT_TRUE(view.keyPressed(
                juce::KeyPress(
                    juce::KeyPress::rightKey,
                    fine ? juce::ModifierKeys::shiftModifier : juce::ModifierKeys::noModifiers,
                    0
                )
            ));
            EXPECT_NEAR(parameterValue(processor, crossoverParameterId(edge)), frequency + step, 0.01F);
        }
    }
}

/// Whole-band keyboard changes preserve the edge ratio in both projections, unlike projection-dependent pointer drags.
TEST(SpectrogramView, HoveredMiddleBandKeysMoveBothEdgesProportionally)
{
    for (const auto scale : {FrequencyScale::logarithmic, FrequencyScale::mel}) {
        for (const bool fine : {false, true}) {
            ImageControlProcessor processor;
            SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
            view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
            view.setFrequencyScale(scale);
            hoverFrequency(view, std::sqrt(100.0 * 500.0), scale);
            const auto modifiers = fine ? juce::ModifierKeys::shiftModifier : juce::ModifierKeys::noModifiers;
            EXPECT_TRUE(view.keyPressed(juce::KeyPress(juce::KeyPress::leftKey, modifiers, 0)));
            const float ratio = fine ? 0.99F : 0.9F;
            EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 100.0F * ratio, 0.001F);
            EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), 500.0F * ratio, 0.002F);
            EXPECT_NEAR(parameterValue(processor, crossoverParameterId(2)), 5000.0F, 0.005F);
            EXPECT_NEAR(
                parameterValue(processor, crossoverParameterId(1)) / parameterValue(processor, crossoverParameterId(0)),
                5.0F,
                0.0001F
            );
        }
    }
}

/// The Low and High bands use their geometric centre for the step but move only their inner crossover.
TEST(SpectrogramView, HoveredOuterBandKeysMoveOnlyTheirInnerEdge)
{
    for (const bool high : {false, true}) {
        ImageControlProcessor processor;
        SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
        view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
        hoverFrequency(view, high ? 10'000.0 : 40.0);
        EXPECT_TRUE(view.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        const float expectedLow = high ? 100.0F : 110.0F;
        const float expectedHigh = high ? 5500.0F : 5000.0F;
        EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), expectedLow, 0.002F);
        EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), 500.0F, 0.002F);
        EXPECT_NEAR(parameterValue(processor, crossoverParameterId(2)), expectedHigh, 0.005F);
    }
}

/// Repeated keys stop at the absolute frequency bounds and at the same minimum neighbour ratio as pointer input.
TEST(SpectrogramView, HoveredKeysClampToFrequencyAndNeighbourLimits)
{
    for (const bool band : {false, true}) {
        for (const bool high : {false, true}) {
            ImageControlProcessor processor;
            SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
            // A wide hit-test-only view leaves room to hover the narrow outer bands without hitting their boundary.
            view.setSize(band ? 100'000 : VIEW_WIDTH, VIEW_HEIGHT);
            const auto edge = high ? std::size_t {2} : std::size_t {0};
            setParameterInGesture(
                *processor.getParameters().getParameter(crossoverParameterId(edge)),
                high ? MAX_FREQUENCY - 100.0F : MIN_FREQUENCY + 0.5F
            );
            const double bandHoverFrequency = high ? 19'980.0 : 20.1;
            const double hover = band ? bandHoverFrequency
                                      : static_cast<double>(parameterValue(processor, crossoverParameterId(edge)));
            hoverFrequency(view, hover);
            if (band) {
                ASSERT_TRUE(view.getHoveredBand().has_value());
            }
            EXPECT_TRUE(view.keyPressed(juce::KeyPress(high ? juce::KeyPress::rightKey : juce::KeyPress::leftKey)));
            EXPECT_NEAR(
                parameterValue(processor, crossoverParameterId(edge)), high ? MAX_FREQUENCY : MIN_FREQUENCY, 0.005F
            );
        }
    }
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const float lower = 100.0F * MIN_CROSSOVER_RATIO;
    setParameterInGesture(*processor.getParameters().getParameter(crossoverParameterId(1)), lower + 0.1F);
    hoverFrequency(view, static_cast<double>(lower + 0.1F));
    EXPECT_TRUE(view.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)));
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), lower, 0.001F);
}

/// A stopped middle band preserves its edge ratio rather than clamping each boundary independently.
TEST(SpectrogramView, MiddleBandKeysClampTheirCommonRatioToExternalNeighbours)
{
    for (const bool right : {false, true}) {
        ImageControlProcessor processor;
        SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
        view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
        hoverFrequency(view, 1500.0);
        ASSERT_EQ(view.getHoveredBand(), std::optional<std::size_t>(2));
        for (int press = 0; press < 2000; ++press) {
            EXPECT_TRUE(view.keyPressed(juce::KeyPress(right ? juce::KeyPress::rightKey : juce::KeyPress::leftKey)));
        }
        const float low = parameterValue(processor, crossoverParameterId(1));
        const float high = parameterValue(processor, crossoverParameterId(2));
        EXPECT_NEAR(low, right ? MAX_FREQUENCY * 0.1F : 100.0F * MIN_CROSSOVER_RATIO, 0.02F);
        EXPECT_NEAR(high, low * 10.0F, 0.1F);
        EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 100.0F, 0.002F);
    }
}

/// Spectrum hover never captures non-horizontal keys, command modifiers, disabled input, or absent hover.
TEST(SpectrogramView, HoveredKeysExcludeModifiersDisabledViewAndPointerDrags)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    EXPECT_FALSE(view.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    hoverFrequency(view, 100.0);
    for (const int modifier :
         {juce::ModifierKeys::commandModifier, juce::ModifierKeys::ctrlModifier, juce::ModifierKeys::altModifier})
    {
        EXPECT_FALSE(view.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, modifier, 0)));
    }
    EXPECT_FALSE(view.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    view.setEnabled(false);
    EXPECT_FALSE(view.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    view.setEnabled(true);
    const juce::Point<float> position {xOf(view, 100.0), view.getPlotBounds().getCentreY()};
    view.mouseDown(mouseEvent(view, position, position, juce::ModifierKeys::leftButtonModifier));
    EXPECT_TRUE(view.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 100.0F, 0.001F);
    view.endDrag();
}

TEST(SpectrogramView, CommandDragChangesTheMidGainAndLinkedSide)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const float x = xOf(view, 2000.0);
    const float centreY = view.getPlotBounds().getCentreY();
    // 50 pixels up at 0.12 dB per pixel raises the High Mid mid gain by 6 dB, and Link, on by default, the side too.
    dragOnView(view, {x, centreY}, {x, centreY - 50.0F}, juce::ModifierKeys::commandModifier);

    EXPECT_NEAR(parameterValue(processor, midGainParameterId(2)), 6.0F, 0.05F);
    EXPECT_NEAR(parameterValue(processor, sideGainParameterId(2)), 6.0F, 0.05F);
    EXPECT_FLOAT_EQ(parameterValue(processor, widthParameterId(2)), 1.0F);
    EXPECT_FLOAT_EQ(parameterValue(processor, recoverParameterId(2)), 0.0F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), 500.0F, 1.0F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(2)), 5000.0F, 1.0F);
}

TEST(SpectrogramView, CommandDragLeavesTheSideAloneWithoutLink)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(linkPropertyName(2), false, nullptr);
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const float x = xOf(view, 2000.0);
    const float centreY = view.getPlotBounds().getCentreY();
    dragOnView(view, {x, centreY}, {x, centreY + 50.0F}, juce::ModifierKeys::commandModifier);

    EXPECT_NEAR(parameterValue(processor, midGainParameterId(2)), -6.0F, 0.05F);
    EXPECT_NEAR(parameterValue(processor, sideGainParameterId(2)), 0.0F, 0.01F);
}

TEST(SpectrogramView, AltDragChangesTheSideGain)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const float x = xOf(view, 2000.0);
    const float centreY = view.getPlotBounds().getCentreY();
    dragOnView(view, {x, centreY}, {x, centreY - 50.0F}, juce::ModifierKeys::altModifier);

    EXPECT_NEAR(parameterValue(processor, sideGainParameterId(2)), 6.0F, 0.05F);
    EXPECT_NEAR(parameterValue(processor, midGainParameterId(2)), 0.0F, 0.01F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), 500.0F, 1.0F);
}

TEST(SpectrogramView, WheelStepsChangeTheGainsByATenthOfADecibel)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const juce::Point<float> position {xOf(view, 2000.0), view.getPlotBounds().getCentreY()};

    view.mouseWheelMove(mouseEvent(view, position, position, {}), notch(1.0F));
    EXPECT_NEAR(parameterValue(processor, sideGainParameterId(2)), 0.1F, 0.001F);
    EXPECT_NEAR(parameterValue(processor, midGainParameterId(2)), 0.0F, 0.001F);

    view.mouseWheelMove(mouseEvent(view, position, position, juce::ModifierKeys::shiftModifier), notch(1.0F));
    EXPECT_NEAR(parameterValue(processor, sideGainParameterId(2)), 0.11F, 0.001F);

    view.mouseWheelMove(mouseEvent(view, position, position, juce::ModifierKeys::commandModifier), notch(-1.0F));
    EXPECT_NEAR(parameterValue(processor, midGainParameterId(2)), -0.1F, 0.001F);
    EXPECT_NEAR(parameterValue(processor, sideGainParameterId(2)), 0.01F, 0.001F);
}

TEST(ControlSlider, WheelNotchMovesATenthOfADecibel)
{
    ControlSlider slider;
    slider.setRange(static_cast<double>(BAND_GAIN_MIN_DB), static_cast<double>(BAND_GAIN_MAX_DB), 0.01);
    slider.setValue(0.0);
    const juce::Point<float> position {1.0F, 1.0F};

    slider.mouseWheelMove(mouseEvent(slider, position, position, {}), notch(1.0F));
    EXPECT_NEAR(slider.getValue(), 0.1, 1e-9);
    slider.mouseWheelMove(mouseEvent(slider, position, position, {}), notch(-3.0F));
    EXPECT_NEAR(slider.getValue(), -0.2, 1e-9);
    slider.mouseWheelMove(mouseEvent(slider, position, position, juce::ModifierKeys::shiftModifier), notch(1.0F));
    EXPECT_NEAR(slider.getValue(), -0.19, 1e-9);
}

TEST(ControlSlider, SmoothWheelMovementAddsUpToWholeSteps)
{
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = 0.03F;
    wheel.isSmooth = true;
    float remainder = 0.0F;
    // A step takes 0.05 of smooth movement, so the first event takes none and the second one.
    EXPECT_EQ(wheelSteps(wheel, remainder), 0);
    EXPECT_EQ(wheelSteps(wheel, remainder), 1);
    wheel.isReversed = true;
    EXPECT_EQ(wheelSteps(wheel, remainder), 0);
    EXPECT_EQ(wheelSteps(wheel, remainder), -1);
}

TEST(ControlSlider, WheelAndKeysDoNotNestInsideAPointerGesture)
{
    ImageControlProcessor processor;
    auto& parameter = *processor.getParameters().getParameter(OUTPUT_GAIN_ID);
    GestureCounter gestures;
    parameter.addListener(&gestures);
    {
        ControlSlider slider;
        slider.setSize(200, 26);
        const juce::SliderParameterAttachment attachment(parameter, slider);
        const juce::Point<float> position {80.0F, 13.0F};
        slider.mouseDown(mouseEvent(slider, position, position, juce::ModifierKeys::leftButtonModifier));
        slider.mouseWheelMove(mouseEvent(slider, position, position, {}), notch(1.0F));
        EXPECT_FALSE(slider.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        EXPECT_EQ(gestures.events, (std::vector<bool> {true}));
        slider.endDrag();
        EXPECT_EQ(gestures.events, (std::vector<bool> {true, false}));
        slider.mouseWheelMove(mouseEvent(slider, position, position, {}), notch(1.0F));
        EXPECT_TRUE(slider.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        EXPECT_EQ(gestures.events, (std::vector<bool> {true, false, true, false, true, false}));
    }
    parameter.removeListener(&gestures);
}

TEST(ControlSlider, DoubleClickResetDoesNotNestInsideTheSecondPointerDown)
{
    ImageControlProcessor processor;
    auto& parameter = *processor.getParameters().getParameter(OUTPUT_GAIN_ID);
    GestureCounter gestures;
    parameter.addListener(&gestures);
    {
        ControlSlider slider;
        slider.setSize(200, 26);
        const juce::SliderParameterAttachment attachment(parameter, slider);
        const juce::Point<float> position {80.0F, 13.0F};
        const auto event = mouseEvent(slider, position, position, juce::ModifierKeys::leftButtonModifier);
        slider.mouseDown(event);
        slider.mouseDoubleClick(event);
        slider.mouseUp(event);
        EXPECT_EQ(gestures.events, (std::vector<bool> {true, false, true, false}));
    }
    parameter.removeListener(&gestures);
}

TEST(SpectrogramView, TeardownClosesAnInterruptedLinkedGainGesture)
{
    ImageControlProcessor processor;
    auto& mid = *processor.getParameters().getParameter(midGainParameterId(2));
    auto& side = *processor.getParameters().getParameter(sideGainParameterId(2));
    GestureCounter midGestures;
    GestureCounter sideGestures;
    mid.addListener(&midGestures);
    side.addListener(&sideGestures);
    {
        SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
        view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
        const juce::Point<float> position {xOf(view, 2000.0), view.getPlotBounds().getCentreY()};
        view.mouseDown(mouseEvent(
            view, position, position, juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::commandModifier
        ));
        processor.getEditorState().setProperty(linkPropertyName(2), false, nullptr);
        EXPECT_EQ(midGestures.events, (std::vector<bool> {true}));
        EXPECT_EQ(sideGestures.events, (std::vector<bool> {true}));
    }
    EXPECT_EQ(midGestures.events, (std::vector<bool> {true, false}));
    EXPECT_EQ(sideGestures.events, (std::vector<bool> {true, false}));
    mid.removeListener(&midGestures);
    side.removeListener(&sideGestures);
}

TEST(SpectrogramView, DoubleClickResetDoesNotOverlapThePointerGesture)
{
    ImageControlProcessor processor;
    auto& parameter = *processor.getParameters().getParameter(crossoverParameterId(0));
    GestureCounter gestures;
    parameter.addListener(&gestures);
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const juce::Point<float> position {xOf(view, 100.0), view.getPlotBounds().getCentreY()};
    const auto event = mouseEvent(view, position, position, juce::ModifierKeys::leftButtonModifier);
    view.mouseDown(event);
    view.mouseDoubleClick(event);
    view.mouseUp(event);
    EXPECT_EQ(gestures.events, (std::vector<bool> {true, false, true, false}));
    parameter.removeListener(&gestures);
}

/// Inside axis text remains paint-only: the reclaimed gutter accepts the same crossover drags in either scale.
TEST(SpectrogramView, MaximizedPlotAcceptsCrossoverEditingAcrossTheReclaimedBottomGutter)
{
    for (const auto scale : {FrequencyScale::logarithmic, FrequencyScale::mel}) {
        ImageControlProcessor processor;
        SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
        view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
        view.setFrequencyScale(scale);
        view.setMaximized(true);
        const float y = view.getPlotBounds().getBottom() - 6.0F;
        const auto start = juce::Point<float> {xOf(view, 100.0, scale), y};
        const auto end = juce::Point<float> {xOf(view, 150.0, scale), y};
        dragOnView(view, start, end);
        EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 150.0F, 0.01F);
        EXPECT_TRUE(processor.getHistory().undo());
        EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 100.0F, 0.01F);
    }
}

/// A live geometry switch closes its existing host gesture once and cannot revive it from stale pointer movement.
TEST(SpectrogramView, MaximizingFinishesAnActiveDragWithoutFurtherParameterWrites)
{
    ImageControlProcessor processor;
    auto& parameter = *processor.getParameters().getParameter(crossoverParameterId(0));
    GestureCounter gestures;
    parameter.addListener(&gestures);
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const juce::Point<float> start {xOf(view, 100.0), view.getPlotBounds().getCentreY()};
    const auto down = mouseEvent(view, start, start, juce::ModifierKeys::leftButtonModifier);
    view.mouseDown(down);
    EXPECT_EQ(gestures.events, (std::vector<bool> {true}));
    view.setMaximized(true);
    EXPECT_EQ(gestures.events, (std::vector<bool> {true, false}));
    view.mouseDrag(down.withNewPosition(juce::Point<float> {xOf(view, 200.0), start.y}));
    view.mouseUp(down);
    EXPECT_NEAR(plainValue(parameter), 100.0F, 0.01F);
    EXPECT_FALSE(processor.getHistory().isEditing());
    EXPECT_FALSE(processor.getHistory().canUndo());
    parameter.removeListener(&gestures);
}

TEST(SpectrogramView, AxisAndSecondaryClicksDoNotOpenGestures)
{
    ImageControlProcessor processor;
    auto& parameter = *processor.getParameters().getParameter(crossoverParameterId(0));
    GestureCounter gestures;
    parameter.addListener(&gestures);
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const juce::Point<float> axis {xOf(view, 100.0), static_cast<float>(VIEW_HEIGHT - 1)};
    view.mouseDown(mouseEvent(view, axis, axis, juce::ModifierKeys::leftButtonModifier));
    const juce::Point<float> plot {axis.x, view.getPlotBounds().getCentreY()};
    view.mouseDown(mouseEvent(view, plot, plot, juce::ModifierKeys::rightButtonModifier));
    view.endDrag();
    EXPECT_TRUE(gestures.events.empty());
    parameter.removeListener(&gestures);
}

TEST(BandStrip, LinkChangesDuringDragKeepTheOriginalGestureBalanced)
{
    ImageControlProcessor processor;
    auto& side = *processor.getParameters().getParameter(sideGainParameterId(0));
    GestureCounter gestures;
    side.addListener(&gestures);
    {
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
        strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
        auto* mid = dynamic_cast<ControlSlider*>(childWithTitle(strip, "Low Mid"));
        ASSERT_NE(mid, nullptr);
        const juce::Point<float> position {40.0F, 13.0F};
        mid->mouseDown(mouseEvent(*mid, position, position, juce::ModifierKeys::leftButtonModifier));
        processor.getEditorState().setProperty(linkPropertyName(0), false, nullptr);
        mid->setValue(3.0, juce::sendNotificationSync);
        EXPECT_NEAR(plainValue(side), 3.0F, 0.01F);
    }
    EXPECT_EQ(gestures.events, (std::vector<bool> {true, false}));
    side.removeListener(&gestures);
}

TEST(BandStrip, EnablingLinkDuringAnUnlinkedDragDoesNotOpenOrEndSideGesture)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(linkPropertyName(0), false, nullptr);
    auto& side = *processor.getParameters().getParameter(sideGainParameterId(0));
    GestureCounter gestures;
    side.addListener(&gestures);
    {
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
        strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
        auto* mid = dynamic_cast<ControlSlider*>(childWithTitle(strip, "Low Mid"));
        ASSERT_NE(mid, nullptr);
        const juce::Point<float> position {40.0F, 13.0F};
        mid->mouseDown(mouseEvent(*mid, position, position, juce::ModifierKeys::leftButtonModifier));
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        mid->setValue(3.0, juce::sendNotificationSync);
        EXPECT_NEAR(plainValue(side), 0.0F, 0.01F);
    }
    EXPECT_TRUE(gestures.events.empty());
    side.removeListener(&gestures);
}

TEST(BandStrip, WheelAndKeysMoveLinkedSideInBalancedGestures)
{
    ImageControlProcessor processor;
    setParameterInGesture(*processor.getParameters().getParameter(widthParameterId(1)), 0.4F);
    setParameterInGesture(*processor.getParameters().getParameter(recoverParameterId(1)), 75.0F);
    auto& side = *processor.getParameters().getParameter(sideGainParameterId(1));
    GestureCounter gestures;
    side.addListener(&gestures);
    {
        const BandStrip strip(processor.getParameters(), processor.getEditorState(), 1, processor.getHistory());
        auto* mid = dynamic_cast<ControlSlider*>(childWithTitle(strip, "Low Mid Mid"));
        ASSERT_NE(mid, nullptr);
        const juce::Point<float> position {40.0F, 13.0F};
        mid->mouseWheelMove(mouseEvent(*mid, position, position, {}), notch(1.0F));
        EXPECT_NEAR(plainValue(side), 0.1F, 0.001F);
        EXPECT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        EXPECT_NEAR(plainValue(side), 0.82F, 0.001F);
        EXPECT_NEAR(parameterValue(processor, widthParameterId(1)), 0.4F, 0.001F);
        EXPECT_NEAR(parameterValue(processor, recoverParameterId(1)), 75.0F, 0.001F);
        EXPECT_EQ(gestures.events, (std::vector<bool> {true, false, true, false}));
    }
    side.removeListener(&gestures);
}

TEST(BandStrip, InterruptedWidthAndRecoverDragsCloseOnlyTheirOwnGestures)
{
    for (const bool recovering : {false, true}) {
        ImageControlProcessor processor;
        auto& parameter
            = *processor.getParameters().getParameter(recovering ? recoverParameterId(0) : widthParameterId(0));
        auto& side = *processor.getParameters().getParameter(sideGainParameterId(0));
        GestureCounter gestures;
        GestureCounter sideGestures;
        parameter.addListener(&gestures);
        side.addListener(&sideGestures);
        {
            BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
            strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
            auto* slider
                = dynamic_cast<ControlSlider*>(childWithTitle(strip, recovering ? "Low Recover" : "Low Width"));
            ASSERT_NE(slider, nullptr);
            const juce::Point<float> position {40.0F, 13.0F};
            slider->mouseDown(mouseEvent(*slider, position, position, juce::ModifierKeys::leftButtonModifier));
            slider->setValue(recovering ? 50.0 : 0.5, juce::sendNotificationSync);
            EXPECT_NEAR(plainValue(parameter), recovering ? 50.0F : 0.5F, 0.001F);
            EXPECT_NEAR(plainValue(side), 0.0F, 0.01F);
        }
        EXPECT_EQ(gestures.events, (std::vector<bool> {true, false}));
        EXPECT_TRUE(sideGestures.events.empty());
        parameter.removeListener(&gestures);
        side.removeListener(&sideGestures);
    }
}

TEST(BandStrip, WidthAndRecoverExposeIndependentAccessibleValuesAndTextAttachments)
{
    ImageControlProcessor processor;
    setParameterInGesture(*processor.getParameters().getParameter(midGainParameterId(0)), -12.0F);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    strip.addToDesktop(0);
    for (const bool recovering : {false, true}) {
        auto* slider = dynamic_cast<ControlSlider*>(childWithTitle(strip, recovering ? "Low Recover" : "Low Width"));
        ASSERT_NE(slider, nullptr);
        auto* handler = slider->getAccessibilityHandler();
        ASSERT_NE(handler, nullptr);
        EXPECT_EQ(handler->getRole(), juce::AccessibilityRole::slider);
        EXPECT_EQ(handler->getTitle(), recovering ? "Low Recover" : "Low Width");
        auto* value = handler->getValueInterface();
        ASSERT_NE(value, nullptr);
        EXPECT_FALSE(value->isReadOnly());
        EXPECT_EQ(value->getCurrentValueAsString(), recovering ? "0%" : "1.00");
        EXPECT_NEAR(value->getCurrentValue(), recovering ? 0.0 : 1.0, 0.001);
        juce::Label* valueBox = nullptr;
        for (auto* child : slider->getChildren()) {
            if (auto* label = dynamic_cast<juce::Label*>(child)) {
                valueBox = label;
                break;
            }
        }
        ASSERT_NE(valueBox, nullptr);
        EXPECT_EQ(valueBox->getText(), recovering ? "0%" : "1.00");
        // Deliver text through the production value-box callback and its parameter attachment.
        valueBox->setText(recovering ? "37.5 %" : "150 %", juce::sendNotificationSync);
        EXPECT_NEAR(
            parameterValue(processor, recovering ? recoverParameterId(0) : widthParameterId(0)),
            recovering ? 38.0F : 1.5F,
            0.001F
        );
        EXPECT_NEAR(value->getCurrentValue(), recovering ? 38.0 : 1.5, 0.001);
        EXPECT_EQ(value->getCurrentValueAsString(), recovering ? "38%" : "1.50");
        EXPECT_EQ(valueBox->getText(), recovering ? "38%" : "1.50");
        if (recovering) {
            value->setValueAsString("37.4 %");
            EXPECT_DOUBLE_EQ(value->getCurrentValue(), 37.0);
            EXPECT_FLOAT_EQ(parameterValue(processor, recoverParameterId(0)), 37.0F);
            EXPECT_EQ(value->getCurrentValueAsString(), "37%");
            EXPECT_EQ(valueBox->getText(), "37%");
            value->setValue(37.5);
            EXPECT_DOUBLE_EQ(value->getCurrentValue(), 38.0);
            EXPECT_EQ(value->getCurrentValueAsString(), "38%");
            value->setValueAsString("0.1 %");
            EXPECT_DOUBLE_EQ(value->getCurrentValue(), 0.0);
            EXPECT_FLOAT_EQ(parameterValue(processor, recoverParameterId(0)), 0.0F);
            EXPECT_EQ(value->getCurrentValueAsString(), "0%");
        }
        value->setValueAsString(recovering ? "Off" : "2");
        EXPECT_NEAR(
            parameterValue(processor, recovering ? recoverParameterId(0) : widthParameterId(0)),
            recovering ? 0.0F : WIDTH_MAX,
            0.001F
        );
        EXPECT_NEAR(parameterValue(processor, midGainParameterId(0)), -12.0F, 0.01F);
        EXPECT_NEAR(parameterValue(processor, sideGainParameterId(0)), 0.0F, 0.01F);
    }
}

TEST(BandStrip, WidthAndRecoverStayEnabledWithMidOffAndFollowOnlyTheirOwnAutomation)
{
    ImageControlProcessor processor;
    const BandStrip strip(processor.getParameters(), processor.getEditorState(), 2, processor.getHistory());
    auto* width = dynamic_cast<ControlSlider*>(childWithTitle(strip, "High Mid Width"));
    auto* recover = dynamic_cast<ControlSlider*>(childWithTitle(strip, "High Mid Recover"));
    ASSERT_NE(width, nullptr);
    ASSERT_NE(recover, nullptr);
    auto& mid = *processor.getParameters().getParameter(midGainParameterId(2));
    auto& side = *processor.getParameters().getParameter(sideGainParameterId(2));
    setParameterInGesture(mid, BAND_GAIN_MIN_DB);
    EXPECT_TRUE(width->isEnabled());
    EXPECT_TRUE(recover->isEnabled());
    EXPECT_TRUE(width->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_NEAR(width->getValue(), 1.02, 0.001);
    setParameterInGesture(*processor.getParameters().getParameter(widthParameterId(2)), 0.4F);
    setParameterInGesture(*processor.getParameters().getParameter(recoverParameterId(2)), 75.0F);
    setParameterInGesture(mid, -12.0F);
    setParameterInGesture(side, 6.0F);
    EXPECT_NEAR(width->getValue(), 0.4, 0.001);
    EXPECT_NEAR(recover->getValue(), 75.0, 0.001);
    auto& recovery = *processor.getParameters().getParameter(recoverParameterId(2));
    recovery.setValueNotifyingHost(37.5F / 200.0F);
    EXPECT_FLOAT_EQ(plainValue(recovery), 38.0F);
    EXPECT_DOUBLE_EQ(recover->getValue(), 38.0);
    EXPECT_EQ(recover->getTextFromValue(recover->getValue()), "38%");
    EXPECT_NEAR(width->getValue(), 0.4, 0.001);
    recovery.setValueNotifyingHost(0.1F / 200.0F);
    EXPECT_FLOAT_EQ(plainValue(recovery), 0.0F);
    EXPECT_DOUBLE_EQ(recover->getValue(), 0.0);
    EXPECT_EQ(recover->getTextFromValue(recover->getValue()), "0%");
    EXPECT_NEAR(plainValue(mid), -12.0F, 0.001F);
    EXPECT_NEAR(plainValue(side), 6.0F, 0.001F);
}

TEST(BandStrip, WidthAndRecoverWheelKeysAndResetUseBalancedIndependentGestures)
{
    for (const bool recovering : {false, true}) {
        ImageControlProcessor processor;
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
        strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
        auto* slider = dynamic_cast<ControlSlider*>(childWithTitle(strip, recovering ? "Low Recover" : "Low Width"));
        ASSERT_NE(slider, nullptr);
        auto& parameter
            = *processor.getParameters().getParameter(recovering ? recoverParameterId(0) : widthParameterId(0));
        GestureCounter gestures;
        parameter.addListener(&gestures);
        const juce::Point<float> position {40.0F, 13.0F};
        slider->mouseWheelMove(mouseEvent(*slider, position, position, {}), notch(1.0F));
        EXPECT_NEAR(plainValue(parameter), recovering ? 1.0F : 1.01F, 0.001F);
        EXPECT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        EXPECT_NEAR(plainValue(parameter), recovering ? 3.0F : 1.03F, 0.001F);
        const auto event = mouseEvent(*slider, position, position, juce::ModifierKeys::leftButtonModifier);
        slider->mouseDown(event);
        slider->mouseDoubleClick(event);
        slider->mouseUp(event);
        EXPECT_NEAR(plainValue(parameter), recovering ? 0.0F : 1.0F, 0.001F);
        EXPECT_EQ(gestures.events, (std::vector<bool> {true, false, true, false, true, false, true, false}));
        parameter.removeListener(&gestures);
    }
}

TEST(BandStrip, FiveRowsFitAtMinimumHeightWithRecoveryBelowWidth)
{
    ImageControlProcessor processor;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    int previousBottom = 0;
    for (const char* title : {"Low Mid", "Low Side", "Low Width", "Low Recover", "Low Pan"}) {
        auto* slider = dynamic_cast<ControlSlider*>(childWithTitle(strip, title));
        ASSERT_NE(slider, nullptr);
        EXPECT_GE(slider->getY(), previousBottom);
        EXPECT_EQ(slider->getHeight(), 26);
        EXPECT_TRUE(strip.getLocalBounds().contains(slider->getBounds()));
        previousBottom = slider->getBottom();
    }
}

TEST(BandCorrelationMeter, ExposesNamedReadOnlyValuesAndSilenceIsUnavailable)
{
    BandCorrelationMeter meter("High Mid");
    meter.setSize(200, BAND_CORRELATION_HEIGHT);
    meter.addToDesktop(0);
    auto* handler = meter.getAccessibilityHandler();
    ASSERT_NE(handler, nullptr);
    EXPECT_EQ(handler->getTitle(), "High Mid correlation");
    auto* value = handler->getValueInterface();
    ASSERT_NE(value, nullptr);
    EXPECT_TRUE(value->isReadOnly());
    EXPECT_EQ(value->getCurrentValueAsString(), "Unavailable");
    meter.setCorrelation(-0.75F);
    EXPECT_EQ(value->getCurrentValueAsString(), "-0.75");
    value->setValueAsString("0.5");
    EXPECT_EQ(value->getCurrentValueAsString(), "-0.75");
    meter.setCorrelation(std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(value->getCurrentValueAsString(), "Unavailable");
}

/// Native accessibility exposes the smoothed default and follows immediate readout-mode changes.
TEST(BandCorrelationMeter, AccessibleValueUsesTheSmoothedDefaultAndCanSwitchToLive)
{
    constexpr double persistenceSeconds {0.6};
    BandCorrelationMeter meter("Low");
    meter.addToDesktop(0);
    auto* handler = meter.getAccessibilityHandler();
    ASSERT_NE(handler, nullptr);
    auto* value = handler->getValueInterface();
    ASSERT_NE(value, nullptr);
    meter.setCorrelation(-0.75F);
    meter.setCorrelation(0.75F, persistenceSeconds, persistenceSeconds);
    EXPECT_EQ(value->getCurrentValueAsString(), "+0.20");
    EXPECT_EQ(meter.readoutText(), "+0.20");
    meter.setSmoothedReadout(false);
    EXPECT_EQ(value->getCurrentValueAsString(), "+0.75");
    meter.setSmoothedReadout(true);
    EXPECT_EQ(value->getCurrentValueAsString(), "+0.20");
    value->setValueAsString("1.00");
    EXPECT_EQ(value->getCurrentValueAsString(), "+0.20");
}

TEST(BandCorrelationMeter, ReadoutIsBlankForUnavailableMeasurements)
{
    BandCorrelationMeter meter("Low");
    meter.setSmoothedReadout(false);
    EXPECT_TRUE(meter.readoutText().isEmpty());
    EXPECT_EQ(meter.valueText(), "Unavailable");

    meter.setCorrelation(-0.75F);
    EXPECT_EQ(meter.readoutText(), "-0.75");
    meter.setCorrelation(0.0F);
    EXPECT_EQ(meter.readoutText(), "+0.00");
    meter.setCorrelation(0.75F);
    EXPECT_EQ(meter.readoutText(), "+0.75");

    constexpr std::array unavailableValues {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    for (const float value : unavailableValues) {
        meter.setCorrelation(value);
        EXPECT_TRUE(meter.readoutText().isEmpty());
        EXPECT_EQ(meter.valueText(), "Unavailable");
    }
}

/// Native peer dispatch must consult the hover listener before the focused production slider consumes arrows.
TEST(ImageControlEditor, SpectrumHoverArrowsPrecedeFocusedSliderAndExcludeTextAndModals)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    SpatialSpectrogramView* view = nullptr;
    for (auto* child : editor.getChildren()) {
        if (auto* candidate = dynamic_cast<SpatialSpectrogramView*>(child)) {
            view = candidate;
        }
    }
    auto* slider = dynamic_cast<ControlSlider*>(childWithTitle(editor, "Low Width"));
    ASSERT_NE(view, nullptr);
    ASSERT_NE(slider, nullptr);
    auto* peer = editor.getPeer();
    ASSERT_NE(peer, nullptr);
    slider->grabKeyboardFocus();
    ASSERT_TRUE(slider->hasKeyboardFocus(false));
    hoverFrequency(*view, 100.0);
    EXPECT_TRUE(peer->handleKeyPress(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 110.0F, 0.001F);
    EXPECT_NEAR(parameterValue(processor, widthParameterId(0)), 1.0F, 0.001F);
    EXPECT_TRUE(slider->hasKeyboardFocus(false));

    hoverFrequency(*view, 200.0);
    EXPECT_TRUE(peer->handleKeyPress(juce::KeyPress(juce::KeyPress::leftKey)));
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 99.0F, 0.002F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), 450.0F, 0.002F);
    view->mouseExit(mouseEvent(*view, {}, {}, {}));
    EXPECT_TRUE(peer->handleKeyPress(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_NEAR(parameterValue(processor, widthParameterId(0)), 1.02F, 0.001F);

    hoverFrequency(*view, 100.0);
    ASSERT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    juce::TextEditor* text = nullptr;
    for (auto* child : slider->getChildren()) {
        if (const auto* label = dynamic_cast<juce::Label*>(child)) {
            text = label->getCurrentTextEditor();
        }
    }
    ASSERT_NE(text, nullptr);
    text->grabKeyboardFocus();
    const auto before = processor.getCrossovers();
    peer->handleKeyPress(juce::KeyPress(juce::KeyPress::rightKey));
    EXPECT_EQ(processor.getCrossovers(), before);
    EXPECT_TRUE(text->hasKeyboardFocus(false));
    text->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component modal;
    modal.setSize(80, 40);
    modal.setWantsKeyboardFocus(true);
    editor.addAndMakeVisible(modal);
    modal.enterModalState(false);
    peer->handleKeyPress(juce::KeyPress(juce::KeyPress::rightKey));
    EXPECT_EQ(processor.getCrossovers(), before);
    modal.exitModalState(0);
}

TEST(ImageControlEditor, CoreProfileRequested)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    const auto* context = juce::OpenGLContext::getContextAttachedTo(editor);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->getPreferredAPI(), juce::OpenGLAPI::openGL);
    EXPECT_EQ(context->getPreferredVersion(), (juce::OpenGLVersion {4, 1}));
    EXPECT_EQ(context->getPreferredProfile(), juce::OpenGLProfile::core);
}

TEST(ImageControlEditor, MonoIsComposedBeforeDeltaAndBypassAtMinimumWidthAndRetainedOnReopen)
{
    ImageControlProcessor processor;
    auto& parameter = *processor.getParameters().getParameter(MONO_ID);
    GestureCounter gestures;
    {
        ImageControlEditor editor(processor);
        editor.setSize(760, 560);
        auto* mono = buttonWithText(editor, "Mono");
        auto* delta = buttonWithText(editor, "Delta");
        auto* bypass = buttonWithText(editor, "Bypass");
        ASSERT_NE(mono, nullptr);
        ASSERT_NE(delta, nullptr);
        ASSERT_NE(bypass, nullptr);
        EXPECT_LE(mono->getRight(), delta->getX());
        EXPECT_LE(delta->getRight(), bypass->getX());
        EXPECT_TRUE(editor.getLocalBounds().contains(mono->getBounds()));
        for (auto* child : editor.getChildren()) {
            if (child != mono && dynamic_cast<juce::Button*>(child) != nullptr) {
                EXPECT_FALSE(mono->getBounds().intersects(child->getBounds()));
            }
        }
        editor.addToDesktop(0);
        auto* handler = mono->getAccessibilityHandler();
        ASSERT_NE(handler, nullptr);
        parameter.addListener(&gestures);
        const bool toggled = handler->getActions().invoke(juce::AccessibilityActionType::toggle);
        parameter.removeListener(&gestures);
        ASSERT_TRUE(toggled);
        EXPECT_FLOAT_EQ(parameter.getValue(), 1.0F);
        EXPECT_EQ(gestures.events, (std::vector<bool> {true, false}));
        parameter.setValueNotifyingHost(0.0F);
        EXPECT_FALSE(mono->getToggleState());
        parameter.setValueNotifyingHost(1.0F);
        EXPECT_TRUE(mono->getToggleState());
        EXPECT_FALSE(delta->getToggleState());
        EXPECT_FALSE(bypass->getToggleState());
    }
    const ImageControlEditor reopened(processor);
    auto* mono = buttonWithText(reopened, "Mono");
    ASSERT_NE(mono, nullptr);
    EXPECT_TRUE(mono->getToggleState());
}

/// Extreme crossover positions retain the complete frequency readout inside the visible plot in both projections.
TEST(SpatialSpectrogramView, CrossoverFrequencyLabelsStayInsidePlot)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    auto& parameter = *processor.getParameters().getParameter(crossoverParameterId(CROSSOVER_COUNT - 1));
    parameter.setValueNotifyingHost(1.0F);
    for (const int width : {300, 760, 1000}) {
        view.setSize(width, 200);
        for (const auto scale : {FrequencyScale::logarithmic, FrequencyScale::mel}) {
            view.setFrequencyScale(scale);
            for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
                const auto label = view.crossoverLabelBounds(crossover);
                EXPECT_FALSE(label.isEmpty());
                EXPECT_TRUE(view.getPlotBounds().contains(label));
            }
        }
    }
}

TEST(ImageControlEditor, CompactCorrelationRowKeepsExpandedHeightAndAlignsWithStrips)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    state.setProperty(EDITOR_HEIGHT_PROPERTY, 900, nullptr);
    state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 260, nullptr);
    state.setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
    const ImageControlEditor editor(processor);
    const int withCorrelation = editor.getHeight();
    EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
    const SettingsPanel settings(state);
    auto* show = buttonWithText(settings, "Show band correlation");
    ASSERT_NE(show, nullptr);
    auto* post = buttonWithText(editor, "Post");
    ASSERT_NE(post, nullptr);
    show->setToggleState(false, juce::sendNotificationSync);
    // Analyzer tap actions apply all display settings through the real editor composition.
    post->setToggleState(!post->getToggleState(), juce::sendNotificationSync);
    // Hidden correlation retains an 18-pixel labelled restore handle in the same section.
    EXPECT_EQ(withCorrelation - editor.getHeight(), BAND_CORRELATION_HEIGHT - 18);
    EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
    show->setToggleState(true, juce::sendNotificationSync);
    post->setToggleState(!post->getToggleState(), juce::sendNotificationSync);
    EXPECT_EQ(editor.getHeight(), withCorrelation);
    auto* meter = childWithTitle(editor, "Low correlation");
    ASSERT_NE(meter, nullptr);
    auto* mid = childWithTitle(editor, "Low Mid");
    ASSERT_NE(mid, nullptr);
    auto* strip = mid->getParentComponent();
    ASSERT_NE(strip, nullptr);
    EXPECT_EQ(meter->getX(), strip->getX());
    EXPECT_EQ(meter->getWidth(), strip->getWidth());
    EXPECT_LE(meter->getBottom(), strip->getY());
    EXPECT_GE(strip->getHeight(), BAND_STRIP_MIN_HEIGHT);
    auto* width = childWithTitle(editor, "Low Width");
    auto* recover = childWithTitle(editor, "Low Recover");
    auto* pan = childWithTitle(editor, "Low Pan");
    ASSERT_NE(width, nullptr);
    ASSERT_NE(recover, nullptr);
    ASSERT_NE(pan, nullptr);
    EXPECT_LE(width->getBottom(), recover->getY());
    EXPECT_LE(recover->getBottom(), pan->getY());
    EXPECT_TRUE(strip->getLocalBounds().contains(pan->getBounds()));
    state.setProperty(HIDE_SPECTROGRAM_PROPERTY, false, nullptr);
    post->setToggleState(!post->getToggleState(), juce::sendNotificationSync);
    EXPECT_EQ(editor.getHeight(), 900);
    EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
}

/// The display toggle defaults on, writes the saved property, and observes external setting changes.
TEST(SettingsPanel, SmoothCorrelationNumbersDefaultsOnAndBindsBothDirections)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    EXPECT_TRUE(static_cast<bool>(state.getProperty(SMOOTH_CORRELATION_READOUT_PROPERTY)));
    const SettingsPanel settings(state);
    auto* smooth = buttonWithText(settings, "Smooth correlation numbers");
    ASSERT_NE(smooth, nullptr);
    EXPECT_TRUE(smooth->getToggleState());
    smooth->setToggleState(false, juce::sendNotificationSync);
    EXPECT_FALSE(static_cast<bool>(state.getProperty(SMOOTH_CORRELATION_READOUT_PROPERTY)));
    state.setProperty(SMOOTH_CORRELATION_READOUT_PROPERTY, true, nullptr);
    EXPECT_TRUE(smooth->getToggleState());
}

/// Applying settings through the real Post action updates all meters without clearing their histories.
TEST(ImageControlEditor, SmoothCorrelationNumbersPropagatesToEveryMeterAndRestoresOnReopen)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    {
        ImageControlEditor editor(processor);
        editor.addToDesktop(0);
        editor.setVisible(true);
        const SettingsPanel settings(state);
        auto* smooth = buttonWithText(settings, "Smooth correlation numbers");
        auto* post = buttonWithText(editor, "Post");
        ASSERT_NE(smooth, nullptr);
        ASSERT_NE(post, nullptr);
        std::array<BandCorrelationMeter*, BAND_COUNT> meters {};
        constexpr std::array titles {
            "Low correlation",
            "Low Mid correlation",
            "High Mid correlation",
            "High correlation",
        };
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            auto* meter = dynamic_cast<BandCorrelationMeter*>(childWithTitle(editor, titles.at(band)));
            ASSERT_NE(meter, nullptr);
            meters.at(band) = meter;
            meter->setCorrelation(-0.75F);
            meter->setCorrelation(0.75F);
            EXPECT_EQ(meter->readoutText(), "-0.75");
        }
        for (const bool enabled : {false, true, false}) {
            smooth->setToggleState(enabled, juce::sendNotificationSync);
            // Analyzer tap actions apply all display settings through the real editor composition.
            post->setToggleState(!post->getToggleState(), juce::sendNotificationSync);
            for (const auto* meter : meters) {
                EXPECT_EQ(meter->readoutText(), enabled ? "-0.75" : "+0.75");
                const auto trend = meter->trendValue();
                const auto range = meter->recentRange();
                if (!trend.has_value() || !range.has_value()) {
                    FAIL() << "Changing readout mode must preserve correlation history";
                }
                EXPECT_FLOAT_EQ(trend.value(), -0.75F);
                EXPECT_FLOAT_EQ(range.value().getStart(), -0.75F);
                EXPECT_FLOAT_EQ(range.value().getEnd(), 0.75F);
            }
        }
    }
    const ImageControlEditor reopened(processor);
    auto* meter = dynamic_cast<BandCorrelationMeter*>(childWithTitle(reopened, "Low correlation"));
    ASSERT_NE(meter, nullptr);
    meter->setCorrelation(-0.75F);
    meter->setCorrelation(0.75F);
    EXPECT_EQ(meter->readoutText(), "+0.75");
}

TEST(ImageControlEditor, HiddenAndClosedMetersStopPublishingMeasurements)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(48'000.0, 256);
    const auto render = [&processor] {
        juce::AudioBuffer<float> block(2, 256);
        juce::MidiBuffer midi;
        for (int pass = 0; pass < 200; ++pass) {
            for (int sample = 0; sample < 256; ++sample) {
                const float value = sample % 2 == 0 ? 0.25F : -0.25F;
                block.setSample(0, sample, value);
                block.setSample(1, sample, value);
            }
            processor.processBlock(block, midi);
        }
    };
    const auto expectUnavailable = [&processor] {
        for (const float value : processor.getBandCorrelations()) {
            EXPECT_TRUE(std::isnan(value));
        }
    };
    {
        ImageControlEditor editor(processor);
        editor.addToDesktop(0);
        editor.setVisible(true);
        render();
        EXPECT_NEAR(processor.getBandCorrelations().back(), 1.0F, 1.0e-6F);
        auto* meter = dynamic_cast<BandCorrelationMeter*>(childWithTitle(editor, "High correlation"));
        ASSERT_NE(meter, nullptr);
        meter->setCorrelation(-0.8F);
        meter->setCorrelation(0.8F, 0.05, 0.3);
        ASSERT_TRUE(meter->trendValue().has_value());
        editor.setVisible(false);
        EXPECT_FALSE(meter->trendValue().has_value());
        EXPECT_FALSE(meter->recentRange().has_value());
        EXPECT_EQ(meter->valueText(), "Unavailable");
        render();
        expectUnavailable();
        editor.setVisible(true);
        meter->setCorrelation(0.5F);
        processor.getEditorState().setProperty(SHOW_BAND_CORRELATION_PROPERTY, false, nullptr);
        auto* post = buttonWithText(editor, "Post");
        ASSERT_NE(post, nullptr);
        post->setToggleState(!post->getToggleState(), juce::sendNotificationSync);
        EXPECT_FALSE(meter->trendValue().has_value());
        EXPECT_FALSE(meter->recentRange().has_value());
        render();
        expectUnavailable();
    }
    render();
    expectUnavailable();
    processor.releaseResources();
}

TEST(SpectrogramView, PositionCurveOverlayShowsAfterAChangeAndFades)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    // Opening the editor sets the saved curve, which does not show the overlay.
    view.setPositionCurve(StereoPositionCurve());
    view.advance(0.1);
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 0.0F);
    view.setPositionCurve(StereoPositionCurve());
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 0.0F);

    view.setPositionCurve(StereoPositionCurve(10.0, 32.0));
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 1.0F);
    for (int refresh = 0; refresh < 24; ++refresh) {
        view.advance(0.1);
    }
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 1.0F);
    view.advance(0.35);
    EXPECT_GT(view.positionCurveOpacity(), 0.0F);
    EXPECT_LT(view.positionCurveOpacity(), 1.0F);
    view.advance(0.5);
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 0.0F);

    // Another change shows it again in full, and painting it works at any opacity.
    view.setPositionCurve(StereoPositionCurve(10.0, 40.0));
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 1.0F);
    const juce::Image canvas(juce::Image::ARGB, VIEW_WIDTH, VIEW_HEIGHT, true);
    juce::Graphics graphics(canvas);
    view.paintEntireComponent(graphics, false);
}

TEST(SpectrogramParticles, QuietPointsAreDroppedAndLouderPointsAreBrighter)
{
    SpectrogramParticleBuilder builder;
    // The loudest point sets the auto-scale reference to 0 dB.
    // -90 dB is below the display range under it, and -20 dB and 0 dB are inside it.
    const std::array points {point(1000.0F, 0.0F, 1.0e-9F), point(1000.0F, 0.0F, 1.0e-2F), point(1000.0F, 0.0F, 1.0F)};
    builder.addFrame(points, FRAME_SECONDS);

    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), 2U);
    EXPECT_GT(brightness(particles[0]), 0.0F);
    EXPECT_GT(brightness(particles[1]), brightness(particles[0]));
}

TEST(SpectrogramParticles, ParticlesArePlacedInSharesOfThePlot)
{
    SpectrogramParticleBuilder builder;
    // Full left is at the top of the plot, and full right at the bottom.
    const std::array points {point(1000.0F, 1.0F, 1.0F), point(100.0F, -1.0F, 1.0F), point(5000.0F, 0.0F, 1.0F)};
    builder.addFrame(points, FRAME_SECONDS);

    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), 3U);
    EXPECT_NEAR(particles[0].x, frequencyToShare(1000.0, FrequencyScale::logarithmic), 1.0e-6);
    EXPECT_FLOAT_EQ(particles[0].y, 0.0F);
    EXPECT_NEAR(particles[1].x, frequencyToShare(100.0, FrequencyScale::logarithmic), 1.0e-6);
    EXPECT_FLOAT_EQ(particles[1].y, 1.0F);
    EXPECT_FLOAT_EQ(particles[2].y, 0.5F);
    // A bin is wider on the axis at low frequencies, where the same spacing covers more of an octave.
    EXPECT_GT(particles[1].spread, particles[2].spread);
}

TEST(SpectrogramParticles, ParticlesFollowTheFrequencyScale)
{
    SpectrogramParticleBuilder builder;
    builder.setFrequencyScale(FrequencyScale::mel);
    builder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    ASSERT_EQ(builder.particles().size(), 1U);
    EXPECT_NEAR(builder.particles()[0].x, frequencyToShare(1000.0, FrequencyScale::mel), 1.0e-6);
}

TEST(SpectrogramParticles, LongerPersistenceDrawsFainterParticles)
{
    // Long persistence lets steady content build up for longer, so each frame adds less.
    SpectrogramParticleBuilder shortBuilder;
    shortBuilder.setPersistence(0.1);
    shortBuilder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    SpectrogramParticleBuilder longBuilder;
    longBuilder.setPersistence(2.0);
    longBuilder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    EXPECT_GT(brightness(shortBuilder.particles()[0]), brightness(longBuilder.particles()[0]));
}

TEST(SpectrogramParticles, ClearDropsTheParticles)
{
    SpectrogramParticleBuilder builder;
    builder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    builder.clear();
    EXPECT_TRUE(builder.particles().empty());
}

TEST(SpectrogramParticles, TiltShowsContentFallingThreeDecibelsPerOctaveEvenly)
{
    // Music falls by about 3 dB per octave, which the tilt evens out, so the highs are as bright as the bass.
    SpectrogramParticleBuilder builder;
    std::vector<SpectralPoint> points;
    for (const float frequency : {50.0F, 200.0F, 1000.0F, 4000.0F, 10'000.0F}) {
        const float decibels = -20.0F - 3.0F * std::log2(frequency / 1000.0F);
        points.push_back(point(frequency, 0.0F, levelFromDecibels(decibels)));
    }
    // A louder point sets the reference, so the evened out points sit inside the display range.
    points.push_back(point(1000.0F, 0.0F, levelFromDecibels(-5.0F)));
    builder.addFrame(points, FRAME_SECONDS);

    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), points.size());
    // Every evened out point is 21 dB above the bottom of the 36 dB range under the -5 dB reference,
    // so each one's emphasis is that share through the contrast curve at its frequency,
    // which differs from the bass only by the extra contrast of the highs.
    constexpr float share = 21.0F / 36.0F;
    const auto contrast = static_cast<float>(DEFAULT_CONTRAST_PERCENT / MAX_CONTRAST_PERCENT);
    for (std::size_t index = 0; index + 1 < particles.size(); ++index) {
        const float frequency = points[index].frequency;
        EXPECT_NEAR(particles[index].emphasis, contrastCurve(share, frequencyContrast(frequency, contrast)), 1.0e-4F)
            << frequency;
    }
    EXPECT_NEAR(particles[0].emphasis, particles[2].emphasis, 1.0e-5F);
}

TEST(SpectrogramParticles, ContrastCurvePassesThroughItsEndsAndPivot)
{
    for (const float contrast : {0.0F, 0.3F, 0.6F, 1.0F, 1.25F}) {
        SCOPED_TRACE(contrast);
        EXPECT_FLOAT_EQ(contrastCurve(0.0F, contrast), 0.0F);
        EXPECT_FLOAT_EQ(contrastCurve(1.0F, contrast), 1.0F);
        EXPECT_NEAR(contrastCurve(0.5F + 0.2F * contrast, contrast), 0.5F, 1.0e-5F);
        float previous = 0.0F;
        for (int step = 1; step <= 100; ++step) {
            const float value = contrastCurve(static_cast<float>(step) / 100.0F, contrast);
            EXPECT_GT(value, previous) << step;
            previous = value;
        }
    }
    // No contrast leaves the share as it is, and more contrast darkens quiet content and brightens loud content.
    EXPECT_NEAR(contrastCurve(0.3F, 0.0F), 0.3F, 1.0e-6F);
    EXPECT_LT(contrastCurve(0.4F, 1.0F), contrastCurve(0.4F, 0.3F));
    EXPECT_GT(contrastCurve(0.85F, 1.0F), contrastCurve(0.85F, 0.3F));
}

TEST(SpectrogramParticles, HighsGetExtraContrast)
{
    EXPECT_FLOAT_EQ(frequencyContrast(100.0F, 0.6F), 0.6F);
    EXPECT_FLOAT_EQ(frequencyContrast(1000.0F, 0.6F), 0.6F);
    EXPECT_GT(frequencyContrast(4000.0F, 0.6F), 0.6F);
    EXPECT_LT(frequencyContrast(4000.0F, 0.6F), frequencyContrast(16'000.0F, 0.6F));
    EXPECT_FLOAT_EQ(frequencyContrast(16'000.0F, 0.6F), frequencyContrast(20'000.0F, 0.6F));

    // A quiet point is fainter in the highs than in the mids, and the halo follows the emphasis.
    SpectrogramParticleBuilder builder;
    const std::array points {
        point(1000.0F, 0.0F, levelFromDecibels(-20.0F)),
        point(1000.0F, 0.0F, 1.0F),
        point(1000.0F * 16.0F, 0.0F, levelFromDecibels(-20.0F - 12.0F)),
    };
    builder.addFrame(points, FRAME_SECONDS);
    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), 3U);
    EXPECT_LT(particles[2].emphasis, particles[0].emphasis);
    EXPECT_FLOAT_EQ(particles[1].emphasis, 1.0F);
}

TEST(SpectrogramParticles, ParticlesSitAtTheReassignedFrequency)
{
    SpectrogramParticleBuilder builder;
    auto reassigned = point(1000.0F, 0.0F, 1.0F);
    reassigned.reassignedFrequency = 1010.0F;
    builder.addFrame(std::array {reassigned}, FRAME_SECONDS);
    ASSERT_EQ(builder.particles().size(), 1U);
    EXPECT_NEAR(builder.particles()[0].x, frequencyToShare(1010.0, FrequencyScale::logarithmic), 1.0e-6);
}

TEST(SpectrogramParticles, AutoScaleDrawsQuietAndLoudMaterialAlike)
{
    SpectrogramParticleBuilder quietBuilder;
    quietBuilder.addFrame(
        std::array {point(1000.0F, 0.0F, levelFromDecibels(-40.0F)), point(1000.0F, 0.0F, levelFromDecibels(-50.0F))},
        FRAME_SECONDS
    );
    SpectrogramParticleBuilder loudBuilder;
    loudBuilder.addFrame(
        std::array {point(1000.0F, 0.0F, levelFromDecibels(-10.0F)), point(1000.0F, 0.0F, levelFromDecibels(-20.0F))},
        FRAME_SECONDS
    );

    ASSERT_EQ(quietBuilder.particles().size(), 2U);
    ASSERT_EQ(loudBuilder.particles().size(), 2U);
    for (std::size_t index = 0; index < 2; ++index) {
        EXPECT_NEAR(brightness(quietBuilder.particles()[index]), brightness(loudBuilder.particles()[index]), 1.0e-5F);
    }
}

TEST(SpectrogramParticles, AutoScaleRisesAtOnceAndFallsSlowly)
{
    SpectrogramParticleBuilder builder;
    EXPECT_FLOAT_EQ(builder.referenceLevel(), MIN_REFERENCE_DB);
    builder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    EXPECT_NEAR(builder.referenceLevel(), 0.0F, 1.0e-4F);

    // After the signal drops by 30 dB, the reference falls gradually instead of jumping down.
    const std::array quiet {point(1000.0F, 0.0F, levelFromDecibels(-30.0F))};
    builder.addFrame(quiet, FRAME_SECONDS);
    EXPECT_LT(builder.referenceLevel(), 0.0F);
    EXPECT_GT(builder.referenceLevel(), -1.0F);
    // Five seconds later it has reached the quieter level and stays there.
    for (int frame = 0; frame < static_cast<int>(5.0 / FRAME_SECONDS); ++frame) {
        builder.addFrame(quiet, FRAME_SECONDS);
    }
    EXPECT_NEAR(builder.referenceLevel(), -30.0F, 1.0e-3F);

    builder.resetLevel();
    EXPECT_FLOAT_EQ(builder.referenceLevel(), MIN_REFERENCE_DB);
}

TEST(SpectrogramParticles, SharedReleaseUsesAudioTimeOnceAcrossInterleavedTiers)
{
    SpectrogramParticleBuilder builder;
    const std::array loud {point(1000.0F, 0.0F, 1.0F)};
    const auto frame = [](const std::size_t tier, const double end, const bool last) {
        return SpectralFrameInfo {.tierIndex = tier, .frameSeconds = 0.01, .endSeconds = end, .lastAtTime = last};
    };
    builder.addFrame(loud, frame(0, 0.0, true));
    // A quieter replacement releases the retained loud tier. All other tiers also report silence.
    for (std::size_t tier = 0; tier < ANALYSIS_TIER_COUNT; ++tier) {
        builder.addFrame({}, frame(tier, 0.0, tier + 1 == ANALYSIS_TIER_COUNT));
    }
    for (int step = 1; step <= 100; ++step) {
        const double end = static_cast<double>(step) * 0.01;
        builder.addFrame({}, frame(4, end, false));
        builder.addFrame({}, frame(5, end, true));
    }
    EXPECT_NEAR(builder.referenceLevel(), -12.0F, 1.0e-4F);
}

TEST(SpectrogramParticles, LoudTierPeakIsRetainedBetweenItsDeadlines)
{
    SpectrogramParticleBuilder builder;
    const std::array loud {point(1000.0F, 0.0F, 1.0F)};
    builder.addFrame(loud, SpectralFrameInfo {.tierIndex = 0, .frameSeconds = 0.04, .endSeconds = 0.04});
    for (int step = 5; step <= 20; ++step) {
        builder.addFrame({}, SpectralFrameInfo {.tierIndex = 5, .frameSeconds = 0.01, .endSeconds = step * 0.01});
        EXPECT_FLOAT_EQ(builder.referenceLevel(), 0.0F);
    }
    builder.addFrame({}, SpectralFrameInfo {.tierIndex = 0, .frameSeconds = 0.04, .endSeconds = 0.24});
    EXPECT_NEAR(builder.referenceLevel(), -0.48F, 1.0e-5F);
}

TEST(SpectrogramParticles, SimultaneousTierFramesShareOneReferenceRegardlessOfOrder)
{
    const std::array quiet {point(1000.0F, 0.0F, levelFromDecibels(-20.0F))};
    const std::array loud {point(1000.0F, 0.0F, 1.0F)};
    SpectrogramParticleBuilder quietFirst;
    quietFirst.addFrame(
        quiet, SpectralFrameInfo {.tierIndex = 0, .frameSeconds = 0.04, .endSeconds = 0.04, .lastAtTime = false}
    );
    EXPECT_TRUE(quietFirst.particles().empty());
    quietFirst.addFrame(loud, SpectralFrameInfo {.tierIndex = 5, .frameSeconds = 0.01, .endSeconds = 0.04});
    SpectrogramParticleBuilder loudFirst;
    loudFirst.addFrame(
        loud, SpectralFrameInfo {.tierIndex = 5, .frameSeconds = 0.01, .endSeconds = 0.04, .lastAtTime = false}
    );
    loudFirst.addFrame(quiet, SpectralFrameInfo {.tierIndex = 0, .frameSeconds = 0.04, .endSeconds = 0.04});
    ASSERT_EQ(quietFirst.particles().size(), 2U);
    ASSERT_EQ(loudFirst.particles().size(), 2U);
    EXPECT_FLOAT_EQ(quietFirst.referenceLevel(), loudFirst.referenceLevel());
    EXPECT_FLOAT_EQ(quietFirst.particles()[0].emphasis, loudFirst.particles()[1].emphasis);
    EXPECT_FLOAT_EQ(brightness(quietFirst.particles()[0]), brightness(loudFirst.particles()[1]));
    EXPECT_FLOAT_EQ(brightness(quietFirst.particles()[1]), brightness(loudFirst.particles()[0]));
    // The same peak and emphasis still receive the weight of their own tier's hop.
    SpectrogramParticleBuilder hopWeights;
    hopWeights.addFrame(
        loud, SpectralFrameInfo {.tierIndex = 0, .frameSeconds = 0.04, .endSeconds = 0.04, .lastAtTime = false}
    );
    hopWeights.addFrame(loud, SpectralFrameInfo {.tierIndex = 5, .frameSeconds = 0.01, .endSeconds = 0.04});
    ASSERT_EQ(hopWeights.particles().size(), 2U);
    EXPECT_NEAR(
        brightness(hopWeights.particles()[0]) / brightness(hopWeights.particles()[1]),
        (1.0 - std::exp(-0.04 / 0.04)) / (1.0 - std::exp(-0.01 / 0.04)),
        1.0e-5
    );
}

TEST(SpectrogramParticles, PhaseColoursCoverBothEndsTheMidpointAndClamp)
{
    SpectrogramParticleBuilder builder;
    builder.setColours(SpectrogramColours::phase);
    std::vector<SpectralPoint> points;
    for (const float coherence : {-2.0F, -1.0F, 0.0F, 1.0F, 2.0F}) {
        auto value = point(1000.0F, 0.0F, 1.0F);
        value.coherence = coherence;
        points.push_back(value);
    }
    builder.addFrame(points, FRAME_SECONDS);
    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), 5U);
    const auto flash = static_cast<float>(1.0 - std::exp(-FRAME_SECONDS / 0.04));
    constexpr std::array red {1.0F, 1.0F, 0.55F, 0.1F, 0.1F};
    constexpr std::array green {0.07F, 0.07F, 0.26F, 0.45F, 0.45F};
    constexpr std::array blue {0.1F, 0.1F, 0.55F, 1.0F, 1.0F};
    for (std::size_t index = 0; index < particles.size(); ++index) {
        EXPECT_NEAR(particles[index].red / flash, red[index], 1.0e-6F);
        EXPECT_NEAR(particles[index].green / flash, green[index], 1.0e-6F);
        EXPECT_NEAR(particles[index].blue / flash, blue[index], 1.0e-6F);
    }
}

TEST(SpectrogramParticles, NonFiniteResultsAndInvalidGeometryAreDroppedWithoutPoisoningTheReference)
{
    SpectrogramParticleBuilder builder;
    std::vector<SpectralPoint> invalid;
    constexpr std::array members {
        &SpectralPoint::frequency,
        &SpectralPoint::reassignedFrequency,
        &SpectralPoint::spacing,
        &SpectralPoint::position,
        &SpectralPoint::level,
        &SpectralPoint::coherence,
    };
    for (const auto member : members) {
        for (const float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            auto result = point(1000.0F, 0.0F, 1.0F);
            result.*member = value;
            invalid.push_back(result);
        }
    }
    invalid.push_back(point(0.0F, 0.0F, 1.0F));
    invalid.push_back(point(1000.0F, 0.0F, -1.0F));
    builder.addFrame(invalid, FRAME_SECONDS);
    EXPECT_TRUE(builder.particles().empty());
    EXPECT_FLOAT_EQ(builder.referenceLevel(), MIN_REFERENCE_DB);
    builder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    ASSERT_EQ(builder.particles().size(), 1U);
    EXPECT_FLOAT_EQ(builder.referenceLevel(), 0.0F);
    EXPECT_TRUE(std::isfinite(brightness(builder.particles()[0])));
}

TEST(SpectrogramParticles, InvalidFrameTimesAndSettingsCannotPoisonLaterParticles)
{
    SpectrogramParticleBuilder builder;
    const std::array points {point(1000.0F, 0.0F, 1.0F)};
    for (const double invalid :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -1.0})
    {
        builder.addFrame(points, SpectralFrameInfo {.frameSeconds = invalid, .endSeconds = 0.1});
        builder.addFrame(points, SpectralFrameInfo {.frameSeconds = 0.01, .endSeconds = invalid});
    }
    EXPECT_TRUE(builder.particles().empty());
    EXPECT_FLOAT_EQ(builder.referenceLevel(), MIN_REFERENCE_DB);
    builder.setContrast(std::numeric_limits<double>::quiet_NaN());
    builder.setPersistence(std::numeric_limits<double>::quiet_NaN());
    builder.addFrame(points, FRAME_SECONDS);
    ASSERT_EQ(builder.particles().size(), 1U);
    EXPECT_TRUE(std::isfinite(brightness(builder.particles()[0])));
    EXPECT_FLOAT_EQ(builder.referenceLevel(), 0.0F);
    builder.resetLevel();
    EXPECT_FLOAT_EQ(builder.referenceLevel(), MIN_REFERENCE_DB);
    builder.addFrame({}, SpectralFrameInfo {.tierIndex = 5, .frameSeconds = 0.01, .endSeconds = 0.01});
    EXPECT_FLOAT_EQ(builder.referenceLevel(), MIN_REFERENCE_DB);
}

TEST(SpectrogramParticles, LouderParticlesAreLarger)
{
    SpectrogramParticleBuilder builder;
    const std::array points {
        point(1000.0F, 0.0F, levelFromDecibels(-30.0F)),
        point(1000.0F, 0.0F, levelFromDecibels(-15.0F)),
        point(1000.0F, 0.0F, 1.0F),
    };
    builder.addFrame(points, FRAME_SECONDS);

    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), 3U);
    EXPECT_LT(particles[0].size, particles[1].size);
    EXPECT_LT(particles[1].size, particles[2].size);
    EXPECT_FLOAT_EQ(particles[2].size, 1.0F);
    EXPECT_GE(particles[0].size, 0.5F);
}

TEST(SpectrogramParticles, FrequencyColoursRunFromRedThroughGreenToBlueAndViolet)
{
    const auto colourAt = [](const double frequency) {
        return frequencyColour(static_cast<float>(frequencyToShare(frequency, FrequencyScale::logarithmic)));
    };
    const LinearColour bass = colourAt(40.0);
    EXPECT_GT(bass.red, bass.green);
    EXPECT_GT(bass.red, bass.blue);
    const LinearColour mids = colourAt(1000.0);
    EXPECT_GT(mids.green, mids.red);
    EXPECT_GT(mids.green, mids.blue);
    const LinearColour highs = colourAt(8000.0);
    EXPECT_GT(highs.blue, highs.red);
    EXPECT_GT(highs.blue, highs.green);
    const LinearColour air = colourAt(16'000.0);
    EXPECT_GT(air.red, air.green);
    EXPECT_GT(air.blue, air.green);

    // The sRGB encoding for drawing with JUCE brightens the linear values and keeps the ends fixed.
    const LinearColour encoded = toSrgb({.red = 0.0F, .green = 0.2F, .blue = 1.0F});
    EXPECT_FLOAT_EQ(encoded.red, 0.0F);
    EXPECT_GT(encoded.green, 0.2F);
    EXPECT_NEAR(encoded.blue, 1.0F, 1.0e-5F);
}
}  // namespace esgrove::image_control::test
