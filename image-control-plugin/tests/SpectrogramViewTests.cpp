/// Tests SpatialSpectrogramView's crossover and gain pointer gestures, hover keyboard editing,
/// balanced host gestures, maximized plot geometry, frequency labels, and fading position-curve overlay.

#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "tests/ViewInteractionTestSupport.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"
#include <initializer_list>

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace esgrove::image_control::test
{
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
    const float bandCentreX = xOf(view, 2000.0);
    const float centreY = view.getPlotBounds().getCentreY();
    // 50 pixels up at 0.12 dB per pixel raises the High Mid mid gain by 6 dB, and Link, on by default, the side too.
    dragOnView(view, {bandCentreX, centreY}, {bandCentreX, centreY - 50.0F}, juce::ModifierKeys::commandModifier);

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
    const float bandCentreX = xOf(view, 2000.0);
    const float centreY = view.getPlotBounds().getCentreY();
    dragOnView(view, {bandCentreX, centreY}, {bandCentreX, centreY + 50.0F}, juce::ModifierKeys::commandModifier);

    EXPECT_NEAR(parameterValue(processor, midGainParameterId(2)), -6.0F, 0.05F);
    EXPECT_NEAR(parameterValue(processor, sideGainParameterId(2)), 0.0F, 0.01F);
}

TEST(SpectrogramView, AltDragChangesTheSideGain)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const float bandCentreX = xOf(view, 2000.0);
    const float centreY = view.getPlotBounds().getCentreY();
    dragOnView(view, {bandCentreX, centreY}, {bandCentreX, centreY - 50.0F}, juce::ModifierKeys::altModifier);

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
            view,
            position,
            position,
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier).withFlags(juce::ModifierKeys::commandModifier)
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
        const float gutterY = view.getPlotBounds().getBottom() - 6.0F;
        const auto start = juce::Point<float> {xOf(view, 100.0, scale), gutterY};
        const auto end = juce::Point<float> {xOf(view, 150.0, scale), gutterY};
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
}  // namespace esgrove::image_control::test
