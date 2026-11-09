/// Implements the mouse controls of SpatialSpectrogramView:
/// dragging crossovers and bands, the mid and side gain drags and wheel steps with Link,
/// the cursor and the hover state, and the tooltip that explains what a drag does.

#include "ParameterHistory.h"
#include "Parameters.h"
#include "ui/ControlSlider.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Change of a band's mid or side gain per pixel of vertical drag with Ctrl, Cmd, Alt, or Option held, in dB.
constexpr float GAIN_DECIBELS_PER_PIXEL {0.12F};
/// Share of the gain drag used while Shift is held, for fine adjustment.
constexpr float FINE_DRAG_SHARE {0.2F};

/// Returns the crossovers bounding a band, omitting the outer edge of the Low and High bands.
std::vector<std::size_t> bandEdges(const std::size_t band)
{
    std::vector<std::size_t> edges;
    if (band > 0) {
        edges.push_back(band - 1);
    }
    if (band < CROSSOVER_COUNT) {
        edges.push_back(band);
    }
    return edges;
}

/// Returns whether Ctrl or Cmd selects mid gain for a band drag or wheel gesture.
bool isMidModifier(const juce::ModifierKeys& modifiers)
{
    return modifiers.isCommandDown() || modifiers.isCtrlDown();
}

/// Returns whether Alt or Option selects side gain for a band drag. Ctrl or Cmd takes precedence.
bool isSideModifier(const juce::ModifierKeys& modifiers)
{
    return modifiers.isAltDown() && !isMidModifier(modifiers);
}
}  // namespace

/// Resets a double-clicked crossover to its default frequency.
void SpatialSpectrogramView::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (!isEnabled() || !event.mods.isLeftButtonDown() || !getPlotBounds().contains(event.position)) {
        return;
    }
    endDrag();
    if (const auto crossover = crossoverNear(event.position.x)) {
        auto& parameter = *crossoverParameters.at(*crossover);
        beginHistoryEdit("Reset crossover");
        parameter.beginChangeGesture();
        setLocalValue(
            parameter, crossoverParameterId(*crossover), parameter.convertFrom0to1(parameter.getDefaultValue())
        );
        parameter.endChangeGesture();
        endHistoryEdit();
    }
}

/// Starts a drag as one host gesture: of the crossover line under the pointer,
/// of the band's mid gain with Ctrl or Cmd held, of its side gain with Alt or Option held,
/// or of the whole band under the pointer.
/// A mid gain drag also moves Side, in the same gesture, while the band's Link is on and its side is not off.
void SpatialSpectrogramView::mouseDown(const juce::MouseEvent& event)
{
    endDrag();
    if (!isEnabled() || !event.mods.isLeftButtonDown() || !getPlotBounds().contains(event.position)) {
        return;
    }
    dragStartCrossovers = currentCrossovers();
    if (const auto crossover = crossoverNear(event.position.x)) {
        drag = SpectrogramDrag::crossover;
        draggedCrossover = crossover;
        beginHistoryEdit("Move crossover");
        crossoverParameters.at(*crossover)->beginChangeGesture();
        return;
    }
    const auto band = bandAt(event.position.x);
    if (!band.has_value()) {
        return;
    }
    draggedBand = band;
    if (isMidModifier(event.mods) || isSideModifier(event.mods)) {
        const bool mid = isMidModifier(event.mods);
        drag = mid ? SpectrogramDrag::midGain : SpectrogramDrag::sideGain;
        dragStartMid = plainValue(*midParameters.at(*band));
        dragStartSide = plainValue(*sideParameters.at(*band));
        dragLinked = mid && isLinked(*band);
        beginHistoryEdit(bandName(*band) + (mid ? " Mid" : " Side"));
        if (mid) {
            midParameters.at(*band)->beginChangeGesture();
        }
        if (!mid || dragLinked) {
            sideParameters.at(*band)->beginChangeGesture();
        }
        return;
    }
    drag = SpectrogramDrag::band;
    beginHistoryEdit("Move " + bandName(*band) + " band");
    for (const auto edge : bandEdges(*band)) {
        crossoverParameters.at(edge)->beginChangeGesture();
    }
}

/// Follows the pointer with the drag that mouseDown started.
void SpatialSpectrogramView::mouseDrag(const juce::MouseEvent& event)
{
    if (history != nullptr && historyEditing && historyGeneration != history->generation()) {
        endDrag();
        return;
    }
    switch (drag) {
        case SpectrogramDrag::none:
            return;
        case SpectrogramDrag::crossover:
            moveCrossover(event);
            return;
        case SpectrogramDrag::band:
            moveBand(event);
            return;
        case SpectrogramDrag::midGain:
        case SpectrogramDrag::sideGain:
            changeBandGain(event);
            return;
    }
}

/// Clears the hover highlights when the pointer leaves the view.
void SpatialSpectrogramView::mouseExit(const juce::MouseEvent& /*event*/)
{
    hoveredBand.reset();
    hoveredCrossover.reset();
}

/// Highlights the band and the crossover under the pointer, with a resize cursor over the crossover lines.
void SpatialSpectrogramView::mouseMove(const juce::MouseEvent& event)
{
    updateHover(event.position);
}

/// Ends the drag and its host gesture.
void SpatialSpectrogramView::mouseUp(const juce::MouseEvent& event)
{
    endDrag();
    updateHover(event.position);
}

/// Closes the exact gestures opened at pointer-down, independently of later Link or modifier changes.
void SpatialSpectrogramView::endDrag()
{
    switch (drag) {
        case SpectrogramDrag::none:
            break;
        case SpectrogramDrag::crossover:
            if (draggedCrossover.has_value()) {
                crossoverParameters.at(*draggedCrossover)->endChangeGesture();
            }
            break;
        case SpectrogramDrag::band:
            if (draggedBand.has_value()) {
                for (const auto edge : bandEdges(*draggedBand)) {
                    crossoverParameters.at(edge)->endChangeGesture();
                }
            }
            break;
        case SpectrogramDrag::midGain:
            if (draggedBand.has_value()) {
                midParameters.at(*draggedBand)->endChangeGesture();
                if (dragLinked) {
                    sideParameters.at(*draggedBand)->endChangeGesture();
                }
            }
            break;
        case SpectrogramDrag::sideGain:
            if (draggedBand.has_value()) {
                sideParameters.at(*draggedBand)->endChangeGesture();
            }
            break;
    }
    endHistoryEdit();
    drag = SpectrogramDrag::none;
    dragLinked = false;
    draggedCrossover.reset();
    draggedBand.reset();
    hoveredBand.reset();
    hoveredCrossover.reset();
}

/// Changes a gain of the band under the pointer by 0.1 dB per wheel step, or 0.01 dB with Shift:
/// its mid gain with Ctrl or Cmd held, following the band's Link like a mid gain drag,
/// and otherwise its side gain, which changes the band's width.
/// Each wheel event is one host gesture of the gains it changes.
/// The wheel does nothing during a drag, which already changes the band.
void SpatialSpectrogramView::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    const auto band = bandAt(event.position.x);
    if (!isEnabled() || !getPlotBounds().contains(event.position) || !band.has_value() || drag != SpectrogramDrag::none)
    {
        return;
    }
    const int steps = wheelSteps(wheel, wheelRemainder);
    if (steps == 0) {
        return;
    }
    const bool mid = isMidModifier(event.mods);
    const auto step = static_cast<float>(DEFAULT_WHEEL_STEP * (event.mods.isShiftDown() ? FINE_WHEEL_SHARE : 1.0));
    dragStartMid = plainValue(*midParameters.at(*band));
    dragStartSide = plainValue(*sideParameters.at(*band));
    dragLinked = mid && isLinked(*band);
    beginHistoryEdit(bandName(*band) + (mid ? " Mid" : " Side"));
    auto* const midParameter = midParameters.at(*band);
    auto* const sideParameter = sideParameters.at(*band);
    if (mid) {
        midParameter->beginChangeGesture();
    }
    if (!mid || dragLinked) {
        sideParameter->beginChangeGesture();
    }
    applyGainChange(*band, mid, static_cast<float>(steps) * step);
    if (mid) {
        midParameter->endChangeGesture();
    }
    if (!mid || dragLinked) {
        sideParameter->endChangeGesture();
    }
    dragLinked = false;
    endHistoryEdit();
}

/// Explains what a drag does where the pointer is: on a crossover line, on a band, or nowhere.
juce::String SpatialSpectrogramView::getTooltip()
{
#if JUCE_MAC
    const juce::String midModifier = "Cmd";
    const juce::String sideModifier = "Option";
#else
    const juce::String midModifier = "Ctrl";
    const juce::String sideModifier = "Alt";
#endif
    if (hoveredCrossover.has_value()) {
        return "Drag to move the crossover between two bands. Double-click to reset it";
    }
    if (hoveredBand.has_value()) {
        return "Drag sideways to move the band. " + midModifier + "-drag up or down, or " + midModifier
            + "-scroll, to change its Mid. " + sideModifier
            + "-drag, or scroll, to change its Side. Hold Shift for finer steps. Press S to solo the band";
    }
    return {};
}

/// Moves the dragged crossover to the pointer, kept the minimum ratio away from its neighbours.
void SpatialSpectrogramView::moveCrossover(const juce::MouseEvent& event)
{
    if (!draggedCrossover.has_value()) {
        return;
    }
    const std::size_t index = *draggedCrossover;
    const Crossovers crossovers = currentCrossovers();
    const float lowerLimit = index == 0 ? MIN_FREQUENCY : crossovers.at(index - 1) * MIN_CROSSOVER_RATIO;
    const float upperLimit
        = index + 1 == CROSSOVER_COUNT ? MAX_FREQUENCY : crossovers.at(index + 1) / MIN_CROSSOVER_RATIO;
    const auto frequency
        = std::clamp(static_cast<float>(xToFrequency(event.position.x)), lowerLimit, std::max(lowerLimit, upperLimit));
    auto* parameter = crossoverParameters.at(index);
    setLocalValue(*parameter, crossoverParameterId(index), frequency);
}

/// Moves the dragged band with the pointer by moving its crossovers the same distance on the frequency axis,
/// so a middle band keeps its width on the axis, and the Low and High bands move only their inner crossover.
/// The move stops where a moved crossover would come closer than the minimum ratio to one that stays,
/// or would leave the frequency range.
void SpatialSpectrogramView::moveBand(const juce::MouseEvent& event)
{
    if (!draggedBand.has_value()) {
        return;
    }
    const auto edges = bandEdges(*draggedBand);
    const auto isMoved = [&edges](const std::size_t crossover) { return std::ranges::contains(edges, crossover); };
    const auto shareOf
        = [this](const float frequency) { return frequencyToShare(static_cast<double>(frequency), scale); };

    double lowestOffset = -1.0;
    double highestOffset = 1.0;
    for (const auto edge : edges) {
        const double start = shareOf(dragStartCrossovers.at(edge));
        if (edge == 0) {
            lowestOffset = std::max(lowestOffset, shareOf(MIN_FREQUENCY) - start);
        } else if (!isMoved(edge - 1)) {
            lowestOffset
                = std::max(lowestOffset, shareOf(dragStartCrossovers.at(edge - 1) * MIN_CROSSOVER_RATIO) - start);
        }
        if (edge + 1 == CROSSOVER_COUNT) {
            highestOffset = std::min(highestOffset, shareOf(MAX_FREQUENCY) - start);
        } else if (!isMoved(edge + 1)) {
            highestOffset
                = std::min(highestOffset, shareOf(dragStartCrossovers.at(edge + 1) / MIN_CROSSOVER_RATIO) - start);
        }
    }

    const auto plot = getPlotBounds();
    const auto pointerOffset = static_cast<double>((event.position.x - event.mouseDownPosition.x) / plot.getWidth());
    const double offset = lowestOffset > highestOffset ? 0.0 : std::clamp(pointerOffset, lowestOffset, highestOffset);
    for (const auto edge : edges) {
        const auto frequency
            = static_cast<float>(shareToFrequency(shareOf(dragStartCrossovers.at(edge)) + offset, scale));
        auto* parameter = crossoverParameters.at(edge);
        setLocalValue(*parameter, crossoverParameterId(edge), frequency);
    }
}

/// Changes the mid or side gain of the dragged band with the pointer's vertical movement:
/// moving up raises it, and Shift makes it finer.
void SpatialSpectrogramView::changeBandGain(const juce::MouseEvent& event)
{
    if (!draggedBand.has_value()) {
        return;
    }
    const float distance = event.mouseDownPosition.y - event.position.y;
    const float step = GAIN_DECIBELS_PER_PIXEL * (event.mods.isShiftDown() ? FINE_DRAG_SHARE : 1.0F);
    applyGainChange(*draggedBand, drag == SpectrogramDrag::midGain, distance * step);
}

/// Sets a band's mid or side gain to its value at the start of the gesture plus a change in dB,
/// within the gain range, inside the host gestures the caller opened.
/// With dragLinked, a mid change moves Side by the same amount, as Link does for the Mid slider,
/// unless Side was off at the start.
void SpatialSpectrogramView::applyGainChange(const std::size_t band, const bool mid, const float change)
{
    const auto setGain
        = [this](juce::RangedAudioParameter& parameter, const juce::String& parameterId, const float decibels) {
              setLocalValue(parameter, parameterId, std::clamp(decibels, BAND_GAIN_MIN_DB, BAND_GAIN_MAX_DB));
          };
    if (!mid) {
        setGain(
            *sideParameters.at(band), sideGainParameterId(band), std::max(dragStartSide, BAND_GAIN_MIN_DB) + change
        );
        return;
    }
    const float midDecibels = std::clamp(dragStartMid + change, BAND_GAIN_MIN_DB, BAND_GAIN_MAX_DB);
    setGain(*midParameters.at(band), midGainParameterId(band), midDecibels);
    if (dragLinked && dragStartSide > BAND_GAIN_MIN_DB) {
        setGain(*sideParameters.at(band), sideGainParameterId(band), dragStartSide + midDecibels - dragStartMid);
    }
}

/// Opens a local command and remembers the host-state token so restored state cannot inherit a stale drag.
void SpatialSpectrogramView::beginHistoryEdit(const juce::String& name)
{
    if (history != nullptr) {
        historyGeneration = history->generation();
        historyEditing = true;
        history->beginEdit(name);
    }
}

/// Closes only a current-generation frame. Restore has already invalidated frames from an older drag.
void SpatialSpectrogramView::endHistoryEdit()
{
    if (history != nullptr && historyEditing && historyGeneration == history->generation()) {
        history->endEdit();
    }
    historyEditing = false;
}

/// Captures only an affected parameter before writing it, stopping multi-parameter edits after a reentrant restore.
void SpatialSpectrogramView::setLocalValue(
    juce::RangedAudioParameter& parameter,
    const juce::String& parameterId,
    const float value
)
{
    if (history != nullptr) {
        if (!historyEditing || historyGeneration != history->generation()) {
            return;
        }
        history->touch(parameterId);
    }
    parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
}

/// Returns whether a band's Link is on, so changing its mid gain moves its side gain by the same amount.
bool SpatialSpectrogramView::isLinked(const std::size_t band) const
{
    return static_cast<bool>(editorSettings.getProperty(linkPropertyName(band)));
}

/// Updates the cursor for the pointer position and the modifier keys when a modifier key is pressed or released.
void SpatialSpectrogramView::modifierKeysChanged(const juce::ModifierKeys& modifiers)
{
    updateCursor(modifiers);
}

/// Shows what a drag from the pointer position would change:
/// a horizontal resize cursor over a crossover line,
/// a vertical one over a band with Ctrl, Cmd, Alt, or Option held, and a hand over a band, which moves it.
void SpatialSpectrogramView::updateCursor(const juce::ModifierKeys& modifiers)
{
    const bool gainDrag = drag == SpectrogramDrag::midGain || drag == SpectrogramDrag::sideGain;
    const bool gainModifier = isMidModifier(modifiers) || isSideModifier(modifiers);
    if (drag == SpectrogramDrag::crossover || hoveredCrossover.has_value()) {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    } else if (gainDrag || (hoveredBand.has_value() && gainModifier)) {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    } else if (drag == SpectrogramDrag::band || hoveredBand.has_value()) {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    } else {
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }
}

/// Updates which band and crossover the pointer is over, and the cursor.
/// During a drag, the dragged band or crossover stays highlighted wherever the pointer goes.
void SpatialSpectrogramView::updateHover(const juce::Point<float> position)
{
    if (drag == SpectrogramDrag::none) {
        hoveredCrossover = getPlotBounds().contains(position) ? crossoverNear(position.x) : std::nullopt;
        hoveredBand
            = getPlotBounds().contains(position) && !hoveredCrossover.has_value() ? bandAt(position.x) : std::nullopt;
    } else {
        hoveredCrossover = draggedCrossover;
        hoveredBand = draggedBand;
    }
    updateCursor(juce::ModifierKeys::getCurrentModifiers());
}
}  // namespace esgrove::image_control
