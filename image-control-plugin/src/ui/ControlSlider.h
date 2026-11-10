/// Declares ControlSlider, the slider of the ImageControl editor.
/// It drags relative to where it is grabbed instead of jumping to the pointer,
/// drags finely while Shift is held, resets on double-click, nudges with the arrow keys once clicked,
/// and moves in fixed steps with the mouse wheel.
/// Also declares wheelSteps, which turns mouse wheel movement into whole steps for every wheel control of the editor.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace esgrove::image_control
{
/// Default change of a slider per mouse wheel step, which is 0.1 dB on the gain sliders.
constexpr double DEFAULT_WHEEL_STEP {0.1};
/// Share of a mouse wheel step used while Shift is held.
constexpr double FINE_WHEEL_SHARE {0.1};

[[nodiscard]] int wheelSteps(const juce::MouseWheelDetails& wheel, float& remainder);

/// A horizontal slider with relative dragging, fine adjustment, keyboard nudging, and fixed mouse wheel steps.
class ControlSlider final : public juce::Slider
{
public:
    ControlSlider();

    ~ControlSlider() override;

    ControlSlider(const ControlSlider&) = delete;
    ControlSlider(ControlSlider&&) = delete;
    ControlSlider& operator=(const ControlSlider&) = delete;
    ControlSlider& operator=(ControlSlider&&) = delete;

    [[nodiscard]] juce::String compactValueText();
    [[nodiscard]] int compactValueRequiredWidth(bool fullNames) const;
    void endDrag();
    void setCompactValue(const juce::String& identity);
    void setCompactValueFullNames(bool fullNames);
    void setWheelStep(double step);

    juce::String getTooltip() override;
    double getValueFromText(const juce::String& text) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    /// Compact value identity, or empty for the ordinary track and value-box presentation. Pan uses P internally.
    juce::String compactIdentity;
    /// Whether painted values use inline full names instead of single-letter suffixes and direction-only pan.
    bool compactValueFullNames {false};
    /// Native text-box placement restored after compact presentation.
    juce::Slider::TextEntryBoxPosition expandedTextBoxPosition {juce::Slider::TextBoxRight};
    /// Native text-box width restored after compact presentation.
    int expandedTextBoxWidth {0};
    /// Native text-box height restored after compact presentation.
    int expandedTextBoxHeight {0};
    /// Native text-box editability restored after compact presentation.
    bool expandedTextBoxReadOnly {false};
    /// Last pointer-down event, retained only until the pointer gesture ends or the control is torn down.
    std::optional<juce::MouseEvent> dragStartEvent;
    /// Change of the value per mouse wheel step.
    double wheelStep {DEFAULT_WHEEL_STEP};
    /// Smooth wheel movement not yet turned into a whole step, carried to the next wheel event.
    float wheelRemainder {0.0F};
};
}  // namespace esgrove::image_control
