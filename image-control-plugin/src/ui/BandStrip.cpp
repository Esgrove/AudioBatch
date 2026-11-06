/// Implements BandStrip: the slider setup and layout,
/// and the Width and Link behaviour that writes the Side parameter inside host change gestures.

#include "ui/BandStrip.h"

#include "Parameters.h"
#include "ui/ImageControlLookAndFeel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <utility>

namespace esgrove::image_control
{
namespace
{
/// Height of the name and range rows, in pixels.
constexpr int HEADER_ROW_HEIGHT {20};
/// Height of the labels above the sliders, in pixels.
constexpr int SLIDER_LABEL_HEIGHT {18};
/// Height of the sliders, in pixels.
constexpr int SLIDER_HEIGHT {26};
/// Width of the value boxes of the sliders, in pixels.
constexpr int TEXT_BOX_WIDTH {78};
/// Height of the value boxes of the sliders, in pixels.
constexpr int TEXT_BOX_HEIGHT {22};
/// Width of the Link toggle, in pixels.
constexpr int LINK_WIDTH {60};
/// Inner margin of the strip, in pixels.
constexpr int MARGIN {10};
/// Corner radius of the strip background, in pixels.
constexpr float CORNER_RADIUS {6.0F};
/// Number of slider rows: Mid, Side, and Width.
constexpr int SLIDER_ROWS {3};
/// Width below which Side is turned off rather than set to a very low gain.
constexpr double MONO_WIDTH {0.001};
/// Font height of the band name.
constexpr float NAME_FONT_HEIGHT {16.0F};
/// Font height of the band range and the slider labels.
constexpr float LABEL_FONT_HEIGHT {14.0F};
/// Step of the Width slider.
constexpr double WIDTH_STEP {0.01};

/// Sets a parameter to a plain value, inside the gesture already open, or in a gesture of its own.
void setSideValue(juce::RangedAudioParameter& parameter, const float value, const bool inGesture)
{
    if (inGesture) {
        parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
    } else {
        setParameterInGesture(parameter, value);
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
    nameLabel.setFont(textFont(NAME_FONT_HEIGHT).boldened());
    nameLabel.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_SUPER_LIGHT));
    nameLabel.setJustificationType(juce::Justification::centredLeft);
    rangeLabel.setFont(textFont(LABEL_FONT_HEIGHT));
    rangeLabel.setJustificationType(juce::Justification::centredLeft);
    rangeLabel.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_MIDDLE_LIGHT));
    addAndMakeVisible(nameLabel);
    addAndMakeVisible(rangeLabel);

    configureSlider(midSlider, midLabel, "Mid");
    configureSlider(sideSlider, sideLabel, "Side");
    midSlider.setDoubleClickReturnValue(true, 0.0);
    sideSlider.setDoubleClickReturnValue(true, 0.0);
    midAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        parameters, midGainParameterId(band), midSlider
    );
    sideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        parameters, sideGainParameterId(band), sideSlider
    );

    configureSlider(widthSlider, widthLabel, "Width");
    widthSlider.setRange(0.0, MAX_STEREO_WIDTH, WIDTH_STEP);
    widthSlider.setDoubleClickReturnValue(true, 1.0);
    widthSlider.onDragStart = [this] {
        draggingWidth = true;
        sideParameter.beginChangeGesture();
    };
    widthSlider.onDragEnd = [this] {
        sideParameter.endChangeGesture();
        draggingWidth = false;
    };
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
            setSideValue(sideParameter, static_cast<float>(side), true);
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
    graphics.setColour(juce::Colour(palette::GREY_SEMI_DARK));
    graphics.fillRoundedRectangle(getLocalBounds().toFloat(), CORNER_RADIUS);
}

/// Lays out the name and Link in the header row, the range below them,
/// and the Mid, Side, and Width sliders below it at full width, each with its label above it.
/// The slider rows share the remaining height evenly.
void BandStrip::resized()
{
    auto area = getLocalBounds().reduced(MARGIN);
    auto header = area.removeFromTop(HEADER_ROW_HEIGHT);
    linkButton.setBounds(header.removeFromRight(LINK_WIDTH));
    nameLabel.setBounds(header);
    rangeLabel.setBounds(area.removeFromTop(HEADER_ROW_HEIGHT));

    const int rowHeight = area.getHeight() / SLIDER_ROWS;
    const std::array rows {
        std::pair {&midLabel, &midSlider},
        std::pair {&sideLabel, &sideSlider},
        std::pair {&widthLabel, &widthSlider},
    };
    static_assert(rows.size() == SLIDER_ROWS);
    for (const auto& [label, slider] : rows) {
        auto row = area.removeFromTop(rowHeight);
        label->setBounds(row.removeFromTop(SLIDER_LABEL_HEIGHT));
        slider->setBounds(row.removeFromTop(SLIDER_HEIGHT));
    }
}

/// Shows the frequency range of the band under its name.
void BandStrip::setRangeText(const juce::String& text)
{
    rangeLabel.setText(text, juce::dontSendNotification);
}

/// Sets Side from the Width macro: Mid plus 20 · log10(width) dB, or off at width 0.
/// A drag or key nudge of the Width slider is one host gesture, and any other change, such as a double-click reset,
/// is its own.
void BandStrip::applyWidth()
{
    const double width = widthSlider.getValue();
    const double side = width < MONO_WIDTH ? static_cast<double>(BAND_GAIN_MIN_DB)
                                           : std::clamp(
                                                 midSlider.getValue() + juce::Decibels::gainToDecibels(width),
                                                 static_cast<double>(BAND_GAIN_MIN_DB),
                                                 static_cast<double>(BAND_GAIN_MAX_DB)
                                             );
    setSideValue(sideParameter, static_cast<float>(side), draggingWidth);
}

/// Sets up a full-width horizontal slider with its value box on the right and its label above it.
void BandStrip::configureSlider(ControlSlider& slider, juce::Label& label, const juce::String& text)
{
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, TEXT_BOX_WIDTH, TEXT_BOX_HEIGHT);
    addAndMakeVisible(slider);
    label.setText(text, juce::dontSendNotification);
    label.setFont(textFont(LABEL_FONT_HEIGHT));
    label.setJustificationType(juce::Justification::bottomLeft);
    label.setBorderSize({});
    addAndMakeVisible(label);
}

/// Shows the width that the Mid and Side gains give, without writing anything back.
/// Width cannot be set while Mid is off, so the slider is disabled then.
void BandStrip::updateWidthFromGains()
{
    const double mid = midSlider.getValue();
    const bool midOff = mid <= static_cast<double>(BAND_GAIN_MIN_DB);
    widthSlider.setEnabled(!midOff);
    const double width = midOff ? 0.0 : bandWidthFromGains(mid, sideSlider.getValue());
    widthSlider.setValue(std::min(width, MAX_STEREO_WIDTH), juce::dontSendNotification);
}
}  // namespace esgrove::image_control
