/// Implements BandStrip: the slider setup and layout,
/// and the Width and Link behaviour that writes the Side parameter inside host change gestures.

#include "ui/BandStrip.h"

#include "Parameters.h"

#include <algorithm>
#include <cstddef>
#include <memory>

namespace esgrove::image_control
{
namespace
{
/// Height of the name and range rows, in pixels.
constexpr int HEADER_ROW_HEIGHT {18};
/// Height of the Width and Link row, in pixels.
constexpr int WIDTH_ROW_HEIGHT {24};
/// Height of the labels above the gain sliders, in pixels.
constexpr int SLIDER_LABEL_HEIGHT {16};
/// Size of the value boxes of the sliders, in pixels.
constexpr int TEXT_BOX_WIDTH {64};
/// Height of the value boxes of the sliders, in pixels.
constexpr int TEXT_BOX_HEIGHT {18};
/// Width of the Link toggle, in pixels.
constexpr int LINK_WIDTH {56};
/// Width of the Width label, in pixels.
constexpr int WIDTH_LABEL_WIDTH {44};
/// Inner margin of the strip, in pixels.
constexpr int MARGIN {6};
/// Corner radius of the strip background, in pixels.
constexpr float CORNER_RADIUS {6.0F};
/// Width below which Side is turned off rather than set to a very low gain.
constexpr double MONO_WIDTH {0.001};
/// Width of the value box of the Width slider, in pixels.
constexpr int WIDTH_TEXT_BOX_WIDTH {48};
/// Font height of the band name.
constexpr float NAME_FONT_HEIGHT {15.0F};
/// Opacity of the secondary text, such as the band range.
constexpr float SECONDARY_TEXT_ALPHA {0.6F};
/// Step of the Width slider.
constexpr double WIDTH_STEP {0.01};
/// Background of the strip, as ARGB.
constexpr juce::uint32 STRIP_BACKGROUND {0xff1a1e27};

/// Returns the linear Width ratio of a band from its Mid and Side gains in dB: 0 when Side is off.
double widthFromGains(const double midDecibels, const double sideDecibels)
{
    if (sideDecibels <= static_cast<double>(BAND_GAIN_MIN_DB)) {
        return 0.0;
    }
    return juce::Decibels::decibelsToGain(sideDecibels - midDecibels);
}

/// Sets a parameter from a plain value in one host change gesture, unless a gesture is already open.
void setParameterValue(juce::RangedAudioParameter& parameter, const float value, const bool inGesture)
{
    if (!inGesture) {
        parameter.beginChangeGesture();
    }
    parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
    if (!inGesture) {
        parameter.endChangeGesture();
    }
}
}  // namespace

/// Creates the controls of a band, attaches Mid and Side to their parameters, and binds Link to the editor settings.
BandStrip::BandStrip(
    juce::AudioProcessorValueTreeState& parameters,
    juce::ValueTree& editorState,
    const std::size_t band
) :
    sideParameter(*parameters.getParameter(sideGainParameterId(band)))
{
    nameLabel.setText(bandName(band), juce::dontSendNotification);
    nameLabel.setFont(juce::FontOptions(NAME_FONT_HEIGHT, juce::Font::bold));
    nameLabel.setJustificationType(juce::Justification::centredLeft);
    rangeLabel.setJustificationType(juce::Justification::centredLeft);
    rangeLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(SECONDARY_TEXT_ALPHA));
    addAndMakeVisible(nameLabel);
    addAndMakeVisible(rangeLabel);

    configureGainSlider(midSlider, midLabel, "Mid");
    configureGainSlider(sideSlider, sideLabel, "Side");
    midAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        parameters, midGainParameterId(band), midSlider
    );
    sideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        parameters, sideGainParameterId(band), sideSlider
    );

    widthLabel.setText("Width", juce::dontSendNotification);
    addAndMakeVisible(widthLabel);
    widthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    widthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, WIDTH_TEXT_BOX_WIDTH, TEXT_BOX_HEIGHT);
    widthSlider.setRange(0.0, MAX_WIDTH, WIDTH_STEP);
    widthSlider.setDoubleClickReturnValue(true, 1.0);
    widthSlider.onDragStart = [this] { sideParameter.beginChangeGesture(); };
    widthSlider.onDragEnd = [this] { sideParameter.endChangeGesture(); };
    widthSlider.onValueChange = [this] { applyWidth(); };
    addAndMakeVisible(widthSlider);

    linkButton.getToggleStateValue().referTo(editorState.getPropertyAsValue(linkPropertyName(band), nullptr));
    linkButton.setTooltip("Dragging Mid moves Side by the same amount, keeping the width");
    addAndMakeVisible(linkButton);

    midSlider.onDragStart = [this] {
        draggingMid = true;
        linkedMidStart = midSlider.getValue();
        linkedSideStart = sideSlider.getValue();
        if (linkButton.getToggleState()) {
            sideParameter.beginChangeGesture();
        }
    };
    midSlider.onDragEnd = [this] {
        if (linkButton.getToggleState()) {
            sideParameter.endChangeGesture();
        }
        draggingMid = false;
    };
    midSlider.onValueChange = [this] {
        if (draggingMid && linkButton.getToggleState() && linkedSideStart > static_cast<double>(BAND_GAIN_MIN_DB)) {
            const double side = std::clamp(
                linkedSideStart + midSlider.getValue() - linkedMidStart,
                static_cast<double>(BAND_GAIN_MIN_DB),
                static_cast<double>(BAND_GAIN_MAX_DB)
            );
            setParameterValue(sideParameter, static_cast<float>(side), true);
        }
        updateWidthFromGains();
    };
    sideSlider.onValueChange = [this] { updateWidthFromGains(); };

    updateWidthFromGains();
}

/// Detaches the sliders before they are destroyed.
BandStrip::~BandStrip()
{
    midAttachment.reset();
    sideAttachment.reset();
}

/// Paints the rounded background of the strip.
void BandStrip::paint(juce::Graphics& graphics)
{
    graphics.setColour(juce::Colour(STRIP_BACKGROUND));
    graphics.fillRoundedRectangle(getLocalBounds().toFloat(), CORNER_RADIUS);
}

/// Lays out the name and range at the top, the Mid and Side sliders side by side, and Width and Link at the bottom.
void BandStrip::resized()
{
    auto area = getLocalBounds().reduced(MARGIN);
    nameLabel.setBounds(area.removeFromTop(HEADER_ROW_HEIGHT));
    rangeLabel.setBounds(area.removeFromTop(HEADER_ROW_HEIGHT));

    auto widthRow = area.removeFromBottom(WIDTH_ROW_HEIGHT);
    linkButton.setBounds(widthRow.removeFromRight(LINK_WIDTH));
    widthLabel.setBounds(widthRow.removeFromLeft(WIDTH_LABEL_WIDTH));
    widthSlider.setBounds(widthRow);

    area.removeFromBottom(MARGIN);
    auto midArea = area.removeFromLeft(area.getWidth() / 2);
    auto sideArea = area;
    midLabel.setBounds(midArea.removeFromTop(SLIDER_LABEL_HEIGHT));
    sideLabel.setBounds(sideArea.removeFromTop(SLIDER_LABEL_HEIGHT));
    midSlider.setBounds(midArea);
    sideSlider.setBounds(sideArea);
}

/// Shows the frequency range of the band under its name.
void BandStrip::setRangeText(const juce::String& text)
{
    rangeLabel.setText(text, juce::dontSendNotification);
}

/// Sets Side from the Width macro: Mid plus 20 · log10(width) dB, or off at width 0.
/// A drag of the Width slider is one host gesture, and any other change, such as a double-click reset, is its own.
void BandStrip::applyWidth()
{
    const double width = widthSlider.getValue();
    const double side = width < MONO_WIDTH ? static_cast<double>(BAND_GAIN_MIN_DB)
                                           : std::clamp(
                                                 midSlider.getValue() + juce::Decibels::gainToDecibels(width),
                                                 static_cast<double>(BAND_GAIN_MIN_DB),
                                                 static_cast<double>(BAND_GAIN_MAX_DB)
                                             );
    setParameterValue(sideParameter, static_cast<float>(side), widthSlider.isMouseButtonDown());
}

/// Sets up a vertical gain slider in dB with its label above it.
void BandStrip::configureGainSlider(juce::Slider& slider, juce::Label& label, const juce::String& text)
{
    slider.setSliderStyle(juce::Slider::LinearVertical);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, TEXT_BOX_WIDTH, TEXT_BOX_HEIGHT);
    slider.setDoubleClickReturnValue(true, 0.0);
    addAndMakeVisible(slider);
    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(label);
}

/// Shows the width that the Mid and Side gains give, without writing anything back.
/// Width cannot be set while Mid is off, so the slider is disabled then.
void BandStrip::updateWidthFromGains()
{
    const double mid = midSlider.getValue();
    const bool midOff = mid <= static_cast<double>(BAND_GAIN_MIN_DB);
    widthSlider.setEnabled(!midOff);
    const double width = midOff ? 0.0 : widthFromGains(mid, sideSlider.getValue());
    widthSlider.setValue(std::min(width, MAX_WIDTH), juce::dontSendNotification);
}
}  // namespace esgrove::image_control
