/// Implements ControlSlider's pointer gestures, keyboard nudges, and stable numeric-only value editor.

#include "ui/ControlSlider.h"

#include "ui/ImageControlLookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

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
/// Restrained selection opacity shared with the editor's ordinary text fields.
constexpr float NUMERIC_SELECTION_ALPHA {0.24F};

/// A numeric editor that sends vertical arrows through the slider's established gesture and history path.
class NumericValueEditor final : public juce::TextEditor
{
public:
    /// Keeps a weak observer because a nudge replaces JUCE's active label editor synchronously.
    explicit NumericValueEditor(ControlSlider& owner) : slider(&owner) {}

    /// Preserves ordinary editing shortcuts and horizontal selection while vertical arrows nudge the parameter.
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::downKey)) {
            const auto modifiers = key.getModifiers();
            if (modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown()) {
                return false;
            }
            // Slider::setValue closes its editor. Retain only the slider across that ownership boundary.
            const auto owner = slider;
            if (owner != nullptr && owner->keyPressed(key)) {
                if (owner != nullptr) {
                    owner->showTextBox();
                }
                return true;
            }
            return false;
        }
        return juce::TextEditor::keyPressed(key);
    }

    /// Leaves the label's existing background and outline visible without JUCE's focused editor bevel.
    void paint(juce::Graphics& /*graphics*/) override {}

    /// The native text holder paints selection and caret without adding an editing-only field outline.
    void paintOverChildren(juce::Graphics& /*graphics*/) override {}

private:
    /// Slider-owned label may delete this editor during a parameter write.
    juce::Component::SafePointer<ControlSlider> slider;
};

/// Keeps units and compact identity outside the editable text while retaining JUCE's label lifecycle.
class NumericValueLabel final : public juce::Label
{
public:
    /// The slider owns this label through JUCE and therefore outlives it.
    explicit NumericValueLabel(ControlSlider& owner) : slider(owner) {}

    /// JUCE releases the native editor before the owning slider is destroyed.
    ~NumericValueLabel() override = default;

    NumericValueLabel(const NumericValueLabel&) = delete;
    NumericValueLabel(NumericValueLabel&&) = delete;
    NumericValueLabel& operator=(const NumericValueLabel&) = delete;
    NumericValueLabel& operator=(NumericValueLabel&&) = delete;

    /// Leaves the slider as the accessible parameter, matching JUCE's native slider value label.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return createIgnoredAccessibilityHandler(*this);
    }

    /// Prevents the native label from consuming wheel events, matching JUCE's slider value label.
    void mouseWheelMove(const juce::MouseEvent& /*event*/, const juce::MouseWheelDetails& /*wheel*/) override {}

    /// Reflows a live editor when compact identity or font geometry changes without changing label bounds.
    void updateEditorBounds()
    {
        resized();
    }

protected:
    /// Paints one stable field and fixed suffix with the same font and text origin during editing.
    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(findColour(juce::Label::backgroundColourId));
        const auto font = getLookAndFeel().getLabelFont(*this);
        graphics.setFont(font);
        graphics.setColour(
            slider.findColour(juce::Slider::textBoxTextColourId).withMultipliedAlpha(isEnabled() ? 1.0F : 0.5F)
        );
        const auto [numberBounds, suffixBounds] = textBounds();
        if (!isBeingEdited()) {
            graphics.drawText(slider.numericValueText(), numberBounds, juce::Justification::centredLeft, false);
        }
        graphics.drawText(slider.numericValueSuffix(), suffixBounds, juce::Justification::centredLeft, false);
        graphics.setColour(findColour(juce::Label::outlineColourId));
        graphics.drawRect(getLocalBounds());
    }

    /// Reflows only the editable numeric region, leaving units and identities outside its selection bounds.
    void resized() override
    {
        if (auto* editor = getCurrentTextEditor()) {
            editor->setBounds(textBounds().first);
            // Fitting values share the resting text origin. Only overflowing drafts need caret scrolling.
            editor->setScrollToShowCursor(editor->getTextWidth() > editor->getWidth());
        }
        repaint();
    }

    /// Hands JUCE ownership of a transparent editor configured to share the painted value's font and alignment.
    juce::TextEditor* createEditorComponent() override
    {
        auto editor = std::make_unique<NumericValueEditor>(slider);
        editor->setFont(getLookAndFeel().getLabelFont(*this));
        editor->setBorder({});
        editor->setIndents(0, 0);
        editor->setJustification(juce::Justification::centredLeft);
        editor->setScrollbarsShown(false);
        editor->setScrollToShowCursor(true);
        editor->setEscapeAndReturnKeysConsumed(true);
        editor->setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        editor->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        editor->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        editor->setColour(juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
        const auto textColour = slider.findColour(juce::Slider::textBoxTextColourId);
        editor->setColour(juce::TextEditor::textColourId, textColour);
        editor->setColour(juce::TextEditor::highlightedTextColourId, textColour);
        editor->setColour(
            juce::TextEditor::highlightColourId, juce::Colour(palette::BLUE).withAlpha(NUMERIC_SELECTION_ALPHA)
        );
        return editor.release();
    }

    /// Normalizes the outgoing token for hideTextBox(false), without changing the host-facing label text.
    void editorAboutToBeHidden(juce::TextEditor* editor) override
    {
        prepareCommit(*editor);
        juce::Label::editorAboutToBeHidden(editor);
    }

    /// Removes only display-owned suffixes after JUCE has populated and focused its native label editor.
    void editorShown(juce::TextEditor* editor) override
    {
        initialNumber = slider.numericValueText();
        initialText = getText();
        inputSuffix = slider.numericValueSuffix();
        editor->setText(initialNumber, false);
        editor->selectAll();
        resized();
        juce::Label::editorShown(editor);
    }

    /// Updates the numeric region as text grows, preserving JUCE's ordinary focus-loss commit policy.
    void textEditorTextChanged(juce::TextEditor& editor) override
    {
        resized();
        juce::Label::textEditorTextChanged(editor);
    }

    /// Applies the same display-unit conversion before JUCE checks whether Return changed the label contents.
    void textEditorReturnKeyPressed(juce::TextEditor& editor) override
    {
        prepareCommit(editor);
        juce::Label::textEditorReturnKeyPressed(editor);
    }

private:
    /// Converts untouched values back to their exact original label token and bare kHz entries to displayed units.
    /// Explicit pasted units and malformed tokens still reach the original parameter parser unchanged.
    void prepareCommit(juce::TextEditor& editor)
    {
        const auto text = editor.getText();
        if (text == initialNumber) {
            editor.setText(initialText, false);
        } else if (inputSuffix == " kHz" && text.trim().containsOnly("0123456789+-.eE")) {
            editor.setText(text + inputSuffix, false);
        }
    }

    /// Measures the complete readout as one centred group, then splits it into editable and fixed regions.
    std::pair<juce::Rectangle<int>, juce::Rectangle<int>> textBounds()
    {
        const auto font = getLookAndFeel().getLabelFont(*this);
        const auto number = isBeingEdited() ? getCurrentTextEditor()->getText() : slider.numericValueText();
        const auto suffix = slider.numericValueSuffix();
        const auto area = getBorderSize().subtractedFrom(getLocalBounds());
        const int suffixWidth = static_cast<int>(std::ceil(juce::GlyphArrangement::getStringWidth(font, suffix)));
        const int numberWidth = std::min(
            std::max(1, static_cast<int>(std::ceil(juce::GlyphArrangement::getStringWidth(font, number)))),
            std::max(1, area.getWidth() - suffixWidth)
        );
        const int left = area.getX() + std::max(0, (area.getWidth() - numberWidth - suffixWidth) / 2);
        return {
            {left, area.getY(), numberWidth, area.getHeight()},
            {left + numberWidth, area.getY(), suffixWidth, area.getHeight()},
        };
    }

    /// Owning slider remains valid for this label's entire lifetime.
    ControlSlider& slider;
    /// Display-only numeric token at entry, used to avoid rounding untouched parameter values.
    juce::String initialNumber;
    /// Full host-facing label token at entry, restored for an untouched edit.
    juce::String initialText;
    /// Unit present at entry. A displayed kHz value needs the matching scale for a bare numeric edit.
    juce::String inputSuffix;
};

/// Identifies painted compact values inline without changing native entry or accessibility text.
juce::String compactValueSuffix(const juce::String& identity, const bool fullNames)
{
    if (fullNames) {
        if (identity == "M") {
            return " MID";
        }
        if (identity == "S") {
            return " SIDE";
        }
        if (identity == "W") {
            return " WIDTH";
        }
        if (identity == "P") {
            return " PAN";
        }
    }
    return identity == "P" ? juce::String {} : identity;
}
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

/// Creates the plugin-local label while preserving JUCE's ownership and native commit/cancel lifecycle.
std::unique_ptr<juce::Label> ControlSlider::createValueBox()
{
    auto label = std::make_unique<NumericValueLabel>(*this);
    label->setJustificationType(juce::Justification::centred);
    label->setKeyboardType(juce::TextInputTarget::decimalKeyboard);
    label->setColour(juce::Label::textColourId, findColour(juce::Slider::textBoxTextColourId));
    label->setColour(juce::Label::backgroundColourId, findColour(juce::Slider::textBoxBackgroundColourId));
    label->setColour(juce::Label::outlineColourId, findColour(juce::Slider::textBoxOutlineColourId));
    return label;
}

/// Returns the editable readout token without changing the parameter's host display or parser.
juce::String ControlSlider::numericValueText()
{
    if (compactIdentity.isNotEmpty()) {
        const auto suffix = compactValueSuffix(compactIdentity, compactValueFullNames);
        return compactValueText().dropLastCharacters(suffix.length());
    }
    const auto text = getTextFromValue(getValue());
    for (const auto* suffix : {" kHz", " Hz", " dB", "%"}) {
        if (text.endsWith(suffix)) {
            return text.dropLastCharacters(juce::String(suffix).length());
        }
    }
    const auto suffix = getTextValueSuffix();
    return suffix.isNotEmpty() && text.endsWith(suffix) ? text.dropLastCharacters(suffix.length()) : text;
}

/// Returns the noneditable unit or compact identity beside the numeric token.
juce::String ControlSlider::numericValueSuffix()
{
    if (compactIdentity.isNotEmpty()) {
        return compactValueSuffix(compactIdentity, compactValueFullNames);
    }
    const auto text = getTextFromValue(getValue());
    return text.substring(numericValueText().length());
}

/// Formats only the compact readout. Parameter parsing and accessibility retain the full host-facing units.
juce::String ControlSlider::compactValueText()
{
    auto text = getTextFromValue(getValue());
    if (compactIdentity == "M" || compactIdentity == "S") {
        text = text.upToFirstOccurrenceOf(" dB", false, true);
        if (!text.startsWithChar('-')) {
            text = "+" + text;
        }
    } else if (compactIdentity == "P") {
        text = text.removeCharacters(" %");
    }
    return text + compactValueSuffix(compactIdentity, compactValueFullNames);
}

/// Measures worst-case glyphs with the painted font and padding, independent of the current automated value.
int ControlSlider::compactValueRequiredWidth(const bool fullNames) const
{
    const auto font = monoFont(COMPACT_FONT_HEIGHT);
    const auto suffix = compactValueSuffix(compactIdentity, fullNames);
    const auto widthFor = [&font, &suffix](const juce::String& value) {
        return juce::GlyphArrangement::getStringWidth(font, value + suffix);
    };
    float width = 0.0F;
    if (compactIdentity == "M" || compactIdentity == "S") {
        width = std::max({widthFor("-60.0"), widthFor("+12.0"), widthFor("-inf")});
    } else if (compactIdentity == "W") {
        width = widthFor("2.00");
    } else if (compactIdentity == "P") {
        width = std::max({widthFor("100L"), widthFor("100R"), widthFor("C")});
    }
    return static_cast<int>(std::ceil(width)) + COMPACT_VALUE_PADDING * 2;
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

/// Changes only painted inline identities, leaving active gestures and JUCE's numeric editor intact during resizing.
void ControlSlider::setCompactValueFullNames(const bool fullNames)
{
    if (compactValueFullNames != fullNames) {
        compactValueFullNames = fullNames;
        resized();
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
    for (auto* child : getChildren()) {
        if (dynamic_cast<NumericValueLabel*>(child) != nullptr) {
            return;
        }
    }
    graphics.setColour(findColour(juce::Slider::textBoxTextColourId).withMultipliedAlpha(isEnabled() ? 1.0F : 0.5F));
    graphics.setFont(monoFont(COMPACT_FONT_HEIGHT));
    graphics.drawText(
        compactValueText(), getLocalBounds().reduced(COMPACT_VALUE_PADDING, 0), juce::Justification::centred
    );
}

/// Initializes the numeric label when a newly attached slider first inherits the plugin's look and feel.
/// JUCE constructs sliders before their parent is known and does not rebuild value boxes on hierarchy changes.
void ControlSlider::parentHierarchyChanged()
{
    juce::Slider::parentHierarchyChanged();
    if (dynamic_cast<ImageControlLookAndFeel*>(&getLookAndFeel()) == nullptr) {
        return;
    }
    for (auto* child : getChildren()) {
        if (dynamic_cast<juce::Label*>(child) != nullptr && dynamic_cast<NumericValueLabel*>(child) == nullptr) {
            lookAndFeelChanged();
            return;
        }
    }
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
            if (auto* numericLabel = dynamic_cast<NumericValueLabel*>(label)) {
                numericLabel->updateEditorBounds();
            }
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
