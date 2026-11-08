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
/// Gap between the Solo button and the Link toggle, in pixels.
constexpr int SOLO_GAP {6};
/// How much brighter the headphone icon draws while the pointer is over the Solo button.
constexpr float SOLO_HOVER_BRIGHTNESS {0.3F};
/// Largest vertical gap between the range label and the first slider row, in pixels.
/// The gap only takes height the slider rows do not need, so it is gone at the minimum strip height.
constexpr int HEADER_SLIDER_GAP {16};
/// Inner margin of the strip, in pixels.
constexpr int MARGIN {10};
/// Corner radius of the strip background, in pixels.
constexpr float CORNER_RADIUS {6.0F};
/// Number of slider rows: Mid, Side, Width, and Pan.
constexpr int SLIDER_ROWS {4};
/// Width below which Side is turned off rather than set to a very low gain.
constexpr double MONO_WIDTH {0.001};
/// Font height of the band name.
constexpr float NAME_FONT_HEIGHT {16.0F};
/// Font height of the band range and the slider labels.
constexpr float LABEL_FONT_HEIGHT {14.0F};
/// Step of the Width slider.
constexpr double WIDTH_STEP {0.01};
/// Change of the Pan slider per mouse wheel step, in percent.
constexpr double PAN_WHEEL_STEP {1.0};
/// Side of the square box the headphone icon of the Solo button is drawn in, in icon units.
constexpr float HEADPHONE_BOX {24.0F};
/// Thickness of the headband of the headphone icon, in icon units.
constexpr float HEADBAND_THICKNESS {2.0F};
/// Corner radius of the ear cups of the headphone icon, in icon units.
constexpr float EAR_CUP_CORNER {1.5F};

static_assert(
    BAND_STRIP_MIN_HEIGHT == MARGIN * 2 + HEADER_ROW_HEIGHT * 2 + SLIDER_ROWS * (SLIDER_LABEL_HEIGHT + SLIDER_HEIGHT),
    "The minimum strip height must fit the margins, the header rows, and the slider rows"
);

/// Returns a headphone icon in a HEADPHONE_BOX square: a headband arc over two ear cups.
juce::Path headphonePath()
{
    constexpr float centreX {HEADPHONE_BOX * 0.5F};
    constexpr float bandCentreY {13.0F};
    constexpr float bandRadius {8.0F};
    constexpr float cupTop {12.5F};
    constexpr float cupWidth {5.0F};
    constexpr float cupHeight {8.5F};
    juce::Path headband;
    headband.addCentredArc(
        centreX,
        bandCentreY,
        bandRadius,
        bandRadius,
        0.0F,
        -juce::MathConstants<float>::halfPi,
        juce::MathConstants<float>::halfPi,
        true
    );
    juce::Path icon;
    juce::PathStrokeType(HEADBAND_THICKNESS, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
        .createStrokedPath(icon, headband);
    icon.addRoundedRectangle(centreX - bandRadius - 1.0F, cupTop, cupWidth, cupHeight, EAR_CUP_CORNER);
    icon.addRoundedRectangle(centreX + bandRadius + 1.0F - cupWidth, cupTop, cupWidth, cupHeight, EAR_CUP_CORNER);
    return icon;
}

/// Returns the headphone icon filled with a colour.
/// Every state of the Solo button uses the same path, so the icon does not move between them.
std::unique_ptr<juce::Drawable> headphoneIcon(const juce::Colour colour)
{
    auto icon = std::make_unique<juce::DrawablePath>();
    icon->setPath(headphonePath());
    icon->setFill(colour);
    return icon;
}

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

/// Creates the controls of a band, attaches Mid, Side, Pan, and Solo to their parameters,
/// and binds Link to the editor settings.
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
    midSlider.setTooltip("Level of the band's mid, what both channels share. Double-click to reset");
    sideSlider.setTooltip("Level of the band's side, what differs between the channels. Off makes the band mono");
    midAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        parameters, midGainParameterId(band), midSlider
    );
    sideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        parameters, sideGainParameterId(band), sideSlider
    );

    configureSlider(widthSlider, widthLabel, "Width");
    widthSlider.setRange(0.0, MAX_STEREO_WIDTH, WIDTH_STEP);
    widthSlider.setWheelStep(WIDTH_STEP);
    widthSlider.setTooltip(
        "Side relative to mid: 0 is mono, 1 leaves the stereo image as it is, and 2 doubles the side"
    );
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

    configureSlider(panSlider, panLabel, "Pan");
    panSlider.setDoubleClickReturnValue(true, 0.0);
    panSlider.setWheelStep(PAN_WHEEL_STEP);
    panSlider.setTooltip("Moves the band toward the left or right channel after its width. Double-click to centre");
    panAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        parameters, panParameterId(band), panSlider
    );

    linkButton.getToggleStateValue().referTo(editorState.getPropertyAsValue(linkPropertyName(band), nullptr));
    linkButton.setTooltip("Dragging Mid moves Side by the same amount, keeping the width");
    addAndMakeVisible(linkButton);

    const auto offIcon = headphoneIcon(juce::Colour(palette::GREY_MIDDLE_LIGHT));
    const auto offHoverIcon = headphoneIcon(juce::Colour(palette::GREY_SUPER_LIGHT));
    const auto onIcon = headphoneIcon(juce::Colour(palette::YELLOW));
    const auto onHoverIcon = headphoneIcon(juce::Colour(palette::YELLOW).brighter(SOLO_HOVER_BRIGHTNESS));
    soloButton.setImages(
        offIcon.get(), offHoverIcon.get(), onIcon.get(), nullptr, onIcon.get(), onHoverIcon.get(), onHoverIcon.get()
    );
    soloButton.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    soloButton.setColour(juce::DrawableButton::backgroundOnColourId, juce::Colours::transparentBlack);
    soloButton.setEdgeIndent(0);
    soloButton.setClickingTogglesState(true);
    soloButton.setTooltip(
        "Solo: listen to this band only. Solo several bands to hear them together. Press S over a band to toggle it"
    );
    soloAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        parameters, soloParameterId(band), soloButton
    );
    addAndMakeVisible(soloButton);

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
    panAttachment.reset();
    soloAttachment.reset();
}

/// Paints the rounded background of the strip.
void BandStrip::paint(juce::Graphics& graphics)
{
    graphics.setColour(juce::Colour(palette::GREY_SEMI_DARK));
    graphics.fillRoundedRectangle(getLocalBounds().toFloat(), CORNER_RADIUS);
}

/// Lays out the name, Solo, and Link in the header row, the range below them,
/// a gap that uses the spare height up to 16 pixels, and the Mid, Side, Width, and Pan sliders below it at full width,
/// each with its label above it. The slider rows share the remaining height evenly.
void BandStrip::resized()
{
    auto area = getLocalBounds().reduced(MARGIN);
    auto header = area.removeFromTop(HEADER_ROW_HEIGHT);
    linkButton.setBounds(header.removeFromRight(LINK_WIDTH));
    header.removeFromRight(SOLO_GAP);
    soloButton.setBounds(header.removeFromRight(HEADER_ROW_HEIGHT));
    nameLabel.setBounds(header);
    rangeLabel.setBounds(area.removeFromTop(HEADER_ROW_HEIGHT));
    const int spareHeight = area.getHeight() - SLIDER_ROWS * (SLIDER_LABEL_HEIGHT + SLIDER_HEIGHT);
    area.removeFromTop(std::clamp(spareHeight, 0, HEADER_SLIDER_GAP));

    const int rowHeight = area.getHeight() / SLIDER_ROWS;
    const std::array rows {
        std::pair {&midLabel, &midSlider},
        std::pair {&sideLabel, &sideSlider},
        std::pair {&widthLabel, &widthSlider},
        std::pair {&panLabel, &panSlider},
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
