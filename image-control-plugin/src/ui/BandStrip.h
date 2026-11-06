/// Declares BandStrip, the controls of one band in the ImageControl editor:
/// the Mid and Side gain sliders attached to their parameters,
/// the Width macro that sets Side relative to Mid, and the Link toggle.

#pragma once

#include "ui/ControlSlider.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <memory>

namespace esgrove::image_control
{
/// Highest value of the Width macro, where the side is twice as loud relative to the mid as in the input.
constexpr double MAX_WIDTH {2.0};

/// The controls of one band.
///
/// Width is the side gain relative to the mid gain as a linear ratio:
/// 0 is mono, 1 leaves the stereo image as it is, and 2 doubles the side relative to the mid.
/// Moving Width sets Side to Mid plus 20 · log10(width) dB, and width 0 turns Side off.
/// With Link on, dragging Mid moves Side by the same amount, keeping the width.
/// Width and Link are editor controls, not host parameters, so automation never has conflicting values.
class BandStrip final : public juce::Component
{
public:
    BandStrip(juce::AudioProcessorValueTreeState& parameters, juce::ValueTree& editorState, std::size_t band);

    ~BandStrip() override;

    BandStrip(const BandStrip&) = delete;
    BandStrip(BandStrip&&) = delete;
    BandStrip& operator=(const BandStrip&) = delete;
    BandStrip& operator=(BandStrip&&) = delete;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void setRangeText(const juce::String& text);

private:
    void applyWidth();
    void configureSlider(ControlSlider& slider, juce::Label& label, const juce::String& text);
    void updateWidthFromGains();

    juce::Label nameLabel;
    juce::Label rangeLabel;
    juce::Label midLabel;
    juce::Label sideLabel;
    juce::Label widthLabel;

    ControlSlider midSlider;
    ControlSlider sideSlider;
    ControlSlider widthSlider;
    juce::ToggleButton linkButton {"Link"};

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> midAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sideAttachment;

    /// Side gain parameter, owned by the parameter tree, which the Width and Link gestures change.
    juce::RangedAudioParameter& sideParameter;

    /// Side gain in dB when the current Mid drag started, for Link.
    double linkedSideStart {0.0};
    /// Mid gain in dB when the current Mid drag started, for Link.
    double linkedMidStart {0.0};
    /// Whether the user is dragging Mid, so only their own drags move Side through Link, not automation.
    bool draggingMid {false};
    /// Whether a Width gesture is open, so its changes join that gesture instead of opening their own.
    bool draggingWidth {false};
};
}  // namespace esgrove::image_control
