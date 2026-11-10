/// Implements ControlSlider: the drag behaviour, the arrow key nudges, and the mouse wheel steps.

#include "ui/ControlSlider.h"

#include "ui/ImageControlLookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace esgrove::image_control
{
namespace
{
/// Drag sensitivity while Shift is held, relative to a normal drag.
constexpr double FINE_DRAG_SENSITIVITY {0.15};
/// Smallest pointer movement in pixels that moves the slider while dragging finely.
constexpr int FINE_DRAG_THRESHOLD {1};
/// Number of nudges that cross the whole range with the arrow keys.
constexpr double NUDGES_PER_RANGE {100.0};
/// Share of a nudge used while Shift is held.
constexpr double FINE_NUDGE_SHARE {0.1};
/// Wheel movement of one notch of a mouse wheel on Windows, as JUCE reports it.
/// A notched wheel event of at least this much takes one step per notch.
constexpr float WHEEL_NOTCH_DELTA {60.0F / 256.0F};
/// Smooth wheel movement, such as from a trackpad, that takes one step.
constexpr float SMOOTH_WHEEL_DELTA_PER_STEP {0.05F};
/// Readable mono text that fits signed gain suffixes in a 49-pixel field and 100L/100R in a 32-pixel field.
constexpr float COMPACT_FONT_HEIGHT {11.0F};
/// Padding around each compact value, in pixels.
constexpr int COMPACT_VALUE_PADDING {2};
/// Radius of the compact numeric field, in pixels.
constexpr float COMPACT_CORNER_RADIUS {3.0F};
}  // namespace

/// Turns a mouse wheel event into a signed number of whole steps, positive for scrolling up or away.
/// A notched wheel takes at least one step per event, and one per Windows notch,
/// since the movement of one notch differs between the platforms and the mice.
/// Smooth movement, such as from a trackpad, adds up in the remainder,
/// which keeps what is left over for the next event, so slow scrolling still moves.
int wheelSteps(const juce::MouseWheelDetails& wheel, float& remainder)
{
    float delta = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? -wheel.deltaX : wheel.deltaY;
    if (wheel.isReversed) {
        delta = -delta;
    }
    if (!wheel.isSmooth) {
        remainder = 0.0F;
        if (juce::exactlyEqual(delta, 0.0F)) {
            return 0;
        }
        const int notches = std::max(1, static_cast<int>(std::lround(std::abs(delta) / WHEEL_NOTCH_DELTA)));
        return delta > 0.0F ? notches : -notches;
    }
    remainder += delta / SMOOTH_WHEEL_DELTA_PER_STEP;
    const float steps = std::trunc(remainder);
    remainder -= steps;
    return static_cast<int>(steps);
}

/// Creates a horizontal slider that drags relative to its grab point, with Shift switching to a slow drag.
ControlSlider::ControlSlider()
{
    setSliderStyle(juce::Slider::LinearHorizontal);
    setSliderSnapsToMousePosition(false);
    setVelocityModeParameters(FINE_DRAG_SENSITIVITY, FINE_DRAG_THRESHOLD, 0.0, true, juce::ModifierKeys::shiftModifier);
    setWantsKeyboardFocus(true);
    setHasFocusOutline(true);
    setMouseClickGrabsKeyboardFocus(true);
}

/// Closes a pointer gesture while its listeners and parameter attachments still exist.
ControlSlider::~ControlSlider()
{
    hideTextBox(true);
    endDrag();
    if (hasKeyboardFocus(true)) {
        giveAwayKeyboardFocus();
    }
}

/// Ends a pointer drag through JUCE, which balances both slider listeners and user callbacks.
void ControlSlider::endDrag()
{
    if (dragStartEvent.has_value()) {
        juce::Slider::mouseUp(*dragStartEvent);
        dragStartEvent.reset();
    }
}

/// Formats only the compact painted value. Native text entry and accessibility retain the parameter's full units.
juce::String ControlSlider::compactValueText()
{
    auto text = getTextFromValue(getValue());
    if (compactIdentity == "M" || compactIdentity == "S") {
        text = text.upToFirstOccurrenceOf(" dB", false, true);
        if (!text.startsWithChar('-')) {
            text = "+" + text;
        }
    } else if (compactIdentity == "P") {
        return text.removeCharacters(" %");
    }
    return text + (compactValueLabelled ? juce::String {} : compactIdentity);
}

/// Switches presentation without replacing the slider, parser, native editor, or parameter attachment.
/// An empty identity restores the original value box and track after finishing any local edit.
void ControlSlider::setCompactValue(const juce::String& identity)
{
    if (compactIdentity == identity) {
        return;
    }
    const bool restoreFocus = hasKeyboardFocus(true);
    hideTextBox(false);
    endDrag();
    if (compactIdentity.isEmpty()) {
        expandedTextBoxPosition = getTextBoxPosition();
        expandedTextBoxWidth = getTextBoxWidth();
        expandedTextBoxHeight = getTextBoxHeight();
        expandedTextBoxReadOnly = !isTextBoxEditable();
    }
    compactIdentity = identity;
    wheelRemainder = 0.0F;
    if (compactIdentity.isNotEmpty()) {
        // JUCE retains its own editor and range geometry, but the compact field paints the readout itself.
        setTextBoxStyle(juce::Slider::TextBoxRight, false, 0, 0);
    } else {
        setTextBoxStyle(expandedTextBoxPosition, expandedTextBoxReadOnly, expandedTextBoxWidth, expandedTextBoxHeight);
    }
    resized();
    repaint();
    if (restoreFocus && isShowing()) {
        grabKeyboardFocus();
    }
}

/// Changes only painted suffixes, leaving active gestures and JUCE's numeric editor intact during resizing.
void ControlSlider::setCompactValueLabelled(const bool labelled)
{
    if (compactValueLabelled != labelled) {
        compactValueLabelled = labelled;
        repaint();
    }
}

/// Adds discoverable numeric entry and explicit units while leaving the full-size tooltip unchanged.
juce::String ControlSlider::getTooltip()
{
    auto tooltip = juce::Slider::getTooltip();
    if (compactIdentity.isEmpty()) {
        return tooltip;
    }
    juce::String unit;
    if (compactIdentity == "W") {
        unit = " multiplier";
    } else if (compactIdentity == "P") {
        unit = " (percent pan)";
    }
    return tooltip + "\n" + getTitle() + ": " + getTextFromValue(getValue()) + unit
        + ". Press Return to enter an exact value. Shift gives fine adjustment";
}

/// Paints a readable numeric field without a track, keeping the native slider's keyboard and pointer semantics.
void ControlSlider::paint(juce::Graphics& graphics)
{
    if (compactIdentity.isEmpty()) {
        juce::Slider::paint(graphics);
        return;
    }
    const auto bounds = getLocalBounds().toFloat().reduced(0.5F);
    graphics.setColour(findColour(juce::Slider::textBoxBackgroundColourId));
    graphics.fillRoundedRectangle(bounds, COMPACT_CORNER_RADIUS);
    graphics.setColour(findColour(juce::Slider::textBoxTextColourId).withMultipliedAlpha(isEnabled() ? 1.0F : 0.5F));
    graphics.setFont(monoFont(COMPACT_FONT_HEIGHT));
    graphics.drawText(
        compactValueText(), getLocalBounds().reduced(COMPACT_VALUE_PADDING, 0), juce::Justification::centred
    );
}

/// Leaves JUCE's label available as the exact-value editor, but routes ordinary field input to the slider itself.
void ControlSlider::resized()
{
    juce::Slider::resized();
    if (compactIdentity.isEmpty()) {
        return;
    }
    for (auto* child : getChildren()) {
        if (auto* label = dynamic_cast<juce::Label*>(child)) {
            label->setBounds(getLocalBounds());
            label->setFont(monoFont(COMPACT_FONT_HEIGHT));
            label->setBorderSize({0, COMPACT_VALUE_PADDING, 0, COMPACT_VALUE_PADDING});
            label->setColour(juce::Label::textColourId, juce::Colours::transparentBlack);
            label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
            label->setInterceptsMouseClicks(false, true);
        }
    }
}

/// Ends the pointer gesture before JUCE opens the separate double-click reset gesture.
void ControlSlider::mouseDoubleClick(const juce::MouseEvent& event)
{
    endDrag();
    juce::Slider::mouseDoubleClick(event);
}

/// Starts a pointer gesture, retaining its event so owners can close it before detaching listeners.
void ControlSlider::mouseDown(const juce::MouseEvent& event)
{
    endDrag();
    if (!isEnabled() || !event.mods.isLeftButtonDown()) {
        return;
    }
    dragStartEvent.emplace(event);
    juce::Slider::mouseDown(event);
}

/// Ignores stale movement after layout has explicitly ended a gesture, including a compact-mode switch.
void ControlSlider::mouseDrag(const juce::MouseEvent& event)
{
    if (dragStartEvent.has_value()) {
        juce::Slider::mouseDrag(event);
    }
}

/// Ends JUCE's pointer gesture once and forgets the retained event.
void ControlSlider::mouseUp(const juce::MouseEvent& event)
{
    if (dragStartEvent.has_value()) {
        juce::Slider::mouseUp(event);
        dragStartEvent.reset();
    }
}

/// Sets the change of the value per mouse wheel step, such as 0.1 dB for a gain or one percent for a pan.
void ControlSlider::setWheelStep(const double step)
{
    wheelStep = step;
}

/// Gives a custom parser the complete original token instead of letting JUCE pre-strip a second unit suffix.
/// Parameter attachments and settings parsers own their unit validation. Uncustomised sliders retain JUCE's parser.
double ControlSlider::getValueFromText(const juce::String& text)
{
    return valueFromTextFunction != nullptr ? valueFromTextFunction(text) : juce::Slider::getValueFromText(text);
}

/// Nudges the value with the arrow keys: right and up increase it, left and down decrease it,
/// a hundredth of the range per press, or a tenth of that with Shift, but never less than one legal interval.
/// Host shortcuts using Command, Control, or Alt are not consumed.
/// Each accepted nudge is its own host gesture, which the slider's attachment passes on.
bool ControlSlider::keyPressed(const juce::KeyPress& key)
{
    if (!isEnabled() || dragStartEvent.has_value()) {
        return false;
    }
    const int keyCode = key.getKeyCode();
    if (keyCode == juce::KeyPress::returnKey && !key.getModifiers().isAnyModifierKeyDown()
        && getTextBoxPosition() != juce::Slider::NoTextBox && isTextBoxEditable())
    {
        showTextBox();
        return true;
    }
    double direction = 0.0;
    if (keyCode == juce::KeyPress::rightKey || keyCode == juce::KeyPress::upKey) {
        direction = 1.0;
    } else if (keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::downKey) {
        direction = -1.0;
    } else {
        return juce::Slider::keyPressed(key);
    }
    const auto modifiers = key.getModifiers();
    if (modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown()) {
        return false;
    }
    const juce::Range<double> range {getMinimum(), getMaximum()};
    const double nudge = std::max(
        range.getLength() / NUDGES_PER_RANGE * (modifiers.isShiftDown() ? FINE_NUDGE_SHARE : 1.0), getInterval()
    );
    const ScopedDragNotification gesture(*this);
    setValue(std::clamp(getValue() + direction * nudge, range.getStart(), range.getEnd()), juce::sendNotificationSync);
    return true;
}

/// Moves the value by the wheel step per wheel step, such as 0.1 dB on a gain slider,
/// or a tenth of that with Shift, but never less than the slider's interval.
/// Each wheel event is its own host gesture, like a key nudge,
/// so Link moves Side with Mid as it does while Mid is dragged.
/// A disabled slider leaves the wheel to juce::Slider, which passes it on to its parent.
void ControlSlider::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (!isEnabled()) {
        juce::Slider::mouseWheelMove(event, wheel);
        return;
    }
    if (dragStartEvent.has_value()) {
        return;
    }
    const int steps = wheelSteps(wheel, wheelRemainder);
    if (steps == 0) {
        return;
    }
    const double step = std::max(wheelStep * (event.mods.isShiftDown() ? FINE_WHEEL_SHARE : 1.0), getInterval());
    const juce::Range<double> range {getMinimum(), getMaximum()};
    const ScopedDragNotification gesture(*this);
    setValue(
        std::clamp(getValue() + static_cast<double>(steps) * step, range.getStart(), range.getEnd()),
        juce::sendNotificationSync
    );
}
}  // namespace esgrove::image_control
