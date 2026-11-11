/// Declares and implements GestureCounter
/// and the shared component lookup, parameter, and mouse helpers for ImageControl view interaction tests.

#pragma once

#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <vector>

namespace esgrove::image_control::test
{
/// Records the host gesture boundaries of one parameter without changing its values.
class GestureCounter final : public juce::AudioProcessorParameter::Listener
{
public:
    /// Counts the exact start/end sequence, including overlapping starts.
    void parameterGestureChanged(int /*parameterIndex*/, const bool starting) override
    {
        events.push_back(starting);
    }
    /// Accepts value notifications without using them for the gesture contract.
    void parameterValueChanged(int /*parameterIndex*/, float /*value*/) override {}

    /// Ordered host gesture boundaries, true for begin and false for end.
    std::vector<bool> events;
};

/// Width of the view in the tests, in pixels.
inline constexpr int VIEW_WIDTH {1000};
/// Height of the view in the tests, in pixels.
inline constexpr int VIEW_HEIGHT {400};

/// Finds a button by its visible text, or null when absent, without relying on private editor fields.
/// The parent owns the result. Check it before use and use it only while the parent retains the button.
inline juce::Button* buttonWithText(const juce::Component& parent, const juce::String& text)
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

/// Finds a real child control by its accessible title, or null when absent.
/// The component hierarchy owns the result, which must be checked and used only while that hierarchy retains it.
inline juce::Component* childWithTitle(const juce::Component& parent, const juce::String& title)
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

/// Returns a mouse event on a component at a position, for a drag that started at another position.
inline juce::MouseEvent mouseEvent(
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
inline juce::MouseWheelDetails notch(const float notches)
{
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = notches * 60.0F / 256.0F;
    return wheel;
}

/// Returns the plain value of a parameter of a processor.
inline float parameterValue(ImageControlProcessor& processor, const juce::String& parameterId)
{
    return plainValue(*processor.getParameters().getParameter(parameterId));
}

/// Returns the x coordinate of a frequency in the view's plot on the logarithmic axis.
inline float xOf(
    const SpatialSpectrogramView& view,
    const double frequency,
    const FrequencyScale scale = FrequencyScale::logarithmic
)
{
    const auto plot = view.getPlotBounds();
    return plot.getX() + static_cast<float>(frequencyToShare(frequency, scale)) * plot.getWidth();
}

/// Returns the frequency an octave count above another.
inline double octavesAbove(const double frequency, const double octaves)
{
    return frequency * std::exp2(octaves);
}

/// Drags on the view from a start to an end position with modifier keys held, through its mouse handlers.
inline void dragOnView(
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

/// Sets the real pointer-hover target without taking keyboard focus away from another editor control.
inline void hoverFrequency(
    SpatialSpectrogramView& view,
    const double frequency,
    const FrequencyScale scale = FrequencyScale::logarithmic
)
{
    const juce::Point<float> position {xOf(view, frequency, scale), view.getPlotBounds().getCentreY()};
    view.mouseMove(mouseEvent(view, position, position, {}));
}
}  // namespace esgrove::image_control::test
