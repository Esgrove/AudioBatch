/// Implements local-only slider and button history using JUCE's parameter delivery and range conversions.
/// Host values update controls silently, so linked-control callbacks never interpret automation as user input.

#include "ui/HistoryAttachments.h"

#include "ParameterHistory.h"
#include "ui/ControlSlider.h"

#include <functional>
#include <optional>
#include <utility>

namespace esgrove::image_control
{
namespace
{
/// Copies JUCE SliderParameterAttachment's range adapter, including custom mappings and snapping.
void configureSliderRange(juce::Slider& slider, juce::RangedAudioParameter& parameter)
{
    slider.valueFromTextFunction = [&parameter](const juce::String& text) {
        return static_cast<double>(parameter.convertFrom0to1(parameter.getValueForText(text)));
    };
    slider.textFromValueFunction = [&parameter](const double value) {
        return parameter.getText(parameter.convertTo0to1(static_cast<float>(value)), 0);
    };
    slider.setDoubleClickReturnValue(true, static_cast<double>(parameter.convertFrom0to1(parameter.getDefaultValue())));
    auto range = parameter.getNormalisableRange();
    auto fromNormalised = [range](const double start, const double end, const double value) mutable {
        range.start = static_cast<float>(start);
        range.end = static_cast<float>(end);
        return static_cast<double>(range.convertFrom0to1(static_cast<float>(value)));
    };
    auto toNormalised = [range](const double start, const double end, const double value) mutable {
        range.start = static_cast<float>(start);
        range.end = static_cast<float>(end);
        return static_cast<double>(range.convertTo0to1(static_cast<float>(value)));
    };
    auto snap = [range](const double start, const double end, const double value) mutable {
        range.start = static_cast<float>(start);
        range.end = static_cast<float>(end);
        return static_cast<double>(range.snapToLegalValue(static_cast<float>(value)));
    };
    juce::NormalisableRange<double> sliderRange {
        static_cast<double>(range.start),
        static_cast<double>(range.end),
        std::move(fromNormalised),
        std::move(toNormalised),
        std::move(snap)
    };
    sliderRange.interval = static_cast<double>(range.interval);
    sliderRange.skew = static_cast<double>(range.skew);
    sliderRange.symmetricSkew = range.symmetricSkew;
    slider.setNormalisableRange(sliderRange);
}
}  // namespace

/// Initializes JUCE-compatible text, reset, and range behavior before installing the local-edit listener.
HistorySliderAttachment::HistorySliderAttachment(
    juce::AudioProcessorValueTreeState& parameters,
    const juce::String& parameterId,
    ControlSlider& control,
    std::optional<std::reference_wrapper<ParameterHistory>> localHistory
) :
    parameter(*parameters.getParameter(parameterId)),
    slider(control),
    id(parameterId),
    history(localHistory),
    attachment(parameter, [this](const float value) { setControlValue(value); }, nullptr)
{
    configureSliderRange(slider, parameter);
    attachment.sendInitialUpdate();
    slider.addListener(this);
}

/// Finishes a retained pointer gesture while callbacks still exist, then detaches before the control is destroyed.
HistorySliderAttachment::~HistorySliderAttachment()
{
    slider.endDrag();
    if (gestureActive) {
        sliderDragEnded(&slider);
    }
    slider.removeListener(this);
}

/// Updates the UI without running onValueChange, which may write linked parameters during an active drag.
void HistorySliderAttachment::setControlValue(const float value)
{
    const juce::ScopedValueSetter<bool> guard(ignoreCallbacks, true);
    slider.setValue(static_cast<double>(value), juce::dontSendNotification);
}

/// Opens the parameter gesture before local value notification and joins any explicit outer history command.
void HistorySliderAttachment::sliderDragStarted(juce::Slider* /*changed*/)
{
    if (ignoreCallbacks || gestureActive) {
        return;
    }
    gestureActive = true;
    if (history.has_value()) {
        gestureGeneration = history->get().generation();
        history->get().beginEdit(parameter.getName(128));
    }
    attachment.beginGesture();
}

/// Closes only this frame. Linked-control owners keep a nested frame open until their onDragEnd completes.
void HistorySliderAttachment::sliderDragEnded(juce::Slider* /*changed*/)
{
    if (!gestureActive) {
        return;
    }
    gestureActive = false;
    attachment.endGesture();
    if (history.has_value() && gestureGeneration == history->get().generation()) {
        history->get().endEdit();
    }
}

/// Captures the old parameter before notifying the host. Ungrouped accessibility or value edits are complete gestures.
void HistorySliderAttachment::sliderValueChanged(juce::Slider* /*changed*/)
{
    if (ignoreCallbacks) {
        return;
    }
    if (gestureActive && history.has_value() && gestureGeneration != history->get().generation()) {
        setControlValue(parameter.convertFrom0to1(parameter.getValue()));
        return;
    }
    const bool complete = !gestureActive;
    const auto generation = history.has_value() ? history->get().generation() : 0;
    if (history.has_value()) {
        if (complete) {
            history->get().beginEdit(parameter.getName(128));
        }
        history->get().touch(id);
    }
    const auto value = static_cast<float>(slider.getValue());
    if (complete) {
        attachment.beginGesture();
    }
    // A synchronous host listener may restore state from beginChangeGesture.
    if (!history.has_value() || generation == history->get().generation()) {
        attachment.setValueAsPartOfGesture(value);
    } else {
        setControlValue(parameter.convertFrom0to1(parameter.getValue()));
    }
    if (complete) {
        attachment.endGesture();
    }
    if (complete && history.has_value() && generation == history->get().generation()) {
        history->get().endEdit();
    }
}

/// Installs a silent host-to-button binding and a listener for explicit local clicks.
HistoryButtonAttachment::HistoryButtonAttachment(
    juce::AudioProcessorValueTreeState& parameters,
    const juce::String& parameterId,
    juce::Button& control,
    std::optional<std::reference_wrapper<ParameterHistory>> localHistory
) :
    button(control),
    parameter(*parameters.getParameter(parameterId)),
    id(parameterId),
    history(localHistory),
    attachment(parameter, [this](const float value) { setControlValue(value); }, nullptr)
{
    attachment.sendInitialUpdate();
    button.addListener(this);
}

/// Removes the local listener before JUCE unregisters and cancels pending host updates.
HistoryButtonAttachment::~HistoryButtonAttachment()
{
    button.removeListener(this);
}

/// Reflects host automation without clicks, history commands, or editor callbacks.
void HistoryButtonAttachment::setControlValue(const float value)
{
    const juce::ScopedValueSetter<bool> guard(ignoreCallbacks, true);
    button.setToggleState(value >= 0.5F, juce::dontSendNotification);
}

/// Records only the parameter affected by a local click, before JUCE writes its new toggle value.
void HistoryButtonAttachment::buttonClicked(juce::Button* /*changed*/)
{
    if (ignoreCallbacks) {
        return;
    }
    const auto generation = history.has_value() ? history->get().generation() : 0;
    if (history.has_value()) {
        history->get().beginEdit(parameter.getName(128));
        history->get().touch(id);
    }
    const float value = button.getToggleState() ? 1.0F : 0.0F;
    attachment.beginGesture();
    if (!history.has_value() || generation == history->get().generation()) {
        attachment.setValueAsPartOfGesture(value);
    } else {
        setControlValue(parameter.convertFrom0to1(parameter.getValue()));
    }
    attachment.endGesture();
    if (history.has_value() && generation == history->get().generation()) {
        history->get().endEdit();
    }
}
}  // namespace esgrove::image_control
