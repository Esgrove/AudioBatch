/// Declares ControlSlider, the slider of the ImageControl editor.
/// It drags relative to where it is grabbed instead of jumping to the pointer,
/// drags finely while Shift is held, resets on double-click, and nudges with the arrow keys once clicked.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace esgrove::image_control
{
/// A horizontal slider with relative dragging, fine adjustment, and keyboard nudging.
class ControlSlider final : public juce::Slider
{
public:
    ControlSlider();

    bool keyPressed(const juce::KeyPress& key) override;
};
}  // namespace esgrove::image_control
