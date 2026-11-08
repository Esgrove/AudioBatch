/// Implements SettingsPanel: binding its controls to the editor settings, and its layout.

#include "ui/SettingsPanel.h"

#include "Parameters.h"
#include "ui/ImageControlLookAndFeel.h"

#include <array>
#include <utility>

namespace esgrove::image_control
{
namespace
{
/// Width of the panel, in pixels.
constexpr int PANEL_WIDTH {340};
/// Height of the panel, in pixels.
constexpr int PANEL_HEIGHT {390};
/// Inner margin of the panel, in pixels.
constexpr int MARGIN {12};
/// Height of each row, in pixels.
constexpr int ROW_HEIGHT {26};
/// Height of the labels above the sliders, in pixels.
constexpr int SLIDER_LABEL_HEIGHT {18};
/// Width of the value boxes of the sliders, in pixels.
constexpr int VALUE_BOX_WIDTH {70};
/// Height of the value boxes of the sliders, in pixels.
constexpr int VALUE_BOX_HEIGHT {20};
/// Font height of the panel title.
constexpr float TITLE_FONT_HEIGHT {15.0F};
/// Font height of the labels.
constexpr float LABEL_FONT_HEIGHT {14.0F};
/// Step of the persistence slider, in seconds.
constexpr double PERSISTENCE_STEP {0.01};
/// Step of the contrast slider, in percent.
constexpr double CONTRAST_STEP_PERCENT {1.0};

/// Sets up a label above a slider.
void configureLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(textFont(LABEL_FONT_HEIGHT));
    label.setBorderSize({});
    label.setJustificationType(juce::Justification::bottomLeft);
}

/// Sets up a slider of a pan curve setting in whole dB, bound to its editor setting.
void configureDecibelSlider(
    juce::Slider& slider,
    const juce::Value& value,
    const double minimum,
    const double maximum,
    const double defaultValue
)
{
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, VALUE_BOX_WIDTH, VALUE_BOX_HEIGHT);
    slider.setRange(minimum, maximum, PAN_CURVE_STEP_DB);
    slider.setTextValueSuffix(" dB");
    slider.setDoubleClickReturnValue(true, defaultValue);
    slider.getValueObject().referTo(value);
}
}  // namespace

/// Creates the panel and binds every control to its editor setting.
SettingsPanel::SettingsPanel(juce::ValueTree& editorState)
{
    titleLabel.setText("Display settings", juce::dontSendNotification);
    titleLabel.setFont(textFont(TITLE_FONT_HEIGHT).boldened());
    titleLabel.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_SUPER_LIGHT));
    titleLabel.setBorderSize({});
    addAndMakeVisible(titleLabel);

    const std::array toggles {
        std::pair<juce::ToggleButton*, const char*> {&phaseColoursButton, PHASE_COLOURS_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&melScaleButton, MEL_SCALE_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&solidRenderingButton, SOLID_RENDERING_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&showFpsButton, SHOW_FPS_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&bypassRenderingButton, BYPASS_RENDERING_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&hideSpectrogramButton, HIDE_SPECTROGRAM_PROPERTY},
    };
    phaseColoursButton.setTooltip(
        "Colour particles by phase, blue for in-phase and red for anti-phase sound, instead of red bass to blue highs"
    );
    melScaleButton.setTooltip(
        "Space the frequencies like the mel scale of AudioBatch's spectrogram, with less room for the bass"
    );
    solidRenderingButton.setTooltip(
        "Draw the spectrogram as stacked translucent layers with outlines, "
        "with only the strongest sound as bright traces on top"
    );
    showFpsButton.setTooltip("Show how often the display refreshes, in the top-left corner of the spectrogram");
    bypassRenderingButton.setTooltip(
        "Stop analyzing and drawing the spectrogram to save CPU and GPU time. The audio is still processed"
    );
    hideSpectrogramButton.setTooltip("Hide the spectrogram and shrink the window to the band controls");
    for (const auto& [button, property] : toggles) {
        button->getToggleStateValue().referTo(editorState.getPropertyAsValue(property, nullptr));
        addAndMakeVisible(*button);
    }

    configureLabel(persistenceLabel, "Persistence");
    addAndMakeVisible(persistenceLabel);
    persistenceSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, VALUE_BOX_WIDTH, VALUE_BOX_HEIGHT);
    persistenceSlider.setRange(MIN_PERSISTENCE_SECONDS, MAX_PERSISTENCE_SECONDS, PERSISTENCE_STEP);
    persistenceSlider.setSkewFactorFromMidPoint(DEFAULT_PERSISTENCE_SECONDS);
    persistenceSlider.setTextValueSuffix(" s");
    persistenceSlider.setDoubleClickReturnValue(true, DEFAULT_PERSISTENCE_SECONDS);
    persistenceSlider.getValueObject().referTo(editorState.getPropertyAsValue(PERSISTENCE_PROPERTY, nullptr));
    persistenceSlider.setTooltip(
        "How long particles take to fade. Short values follow the music closely, long values build a calmer picture"
    );
    addAndMakeVisible(persistenceSlider);

    configureLabel(contrastLabel, "Contrast");
    addAndMakeVisible(contrastLabel);
    contrastSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, VALUE_BOX_WIDTH, VALUE_BOX_HEIGHT);
    contrastSlider.setRange(0.0, MAX_CONTRAST_PERCENT, CONTRAST_STEP_PERCENT);
    contrastSlider.setTextValueSuffix(" %");
    contrastSlider.setDoubleClickReturnValue(true, DEFAULT_CONTRAST_PERCENT);
    contrastSlider.getValueObject().referTo(editorState.getPropertyAsValue(CONTRAST_PROPERTY, nullptr));
    contrastSlider.setTooltip(
        "How strongly loud sound stands out from quiet sound. "
        "Higher values darken the background and noise, most in the highs, so the main elements stand out"
    );
    addAndMakeVisible(contrastSlider);

    configureLabel(panMidpointLabel, "Pan midpoint");
    addAndMakeVisible(panMidpointLabel);
    configureDecibelSlider(
        panMidpointSlider,
        editorState.getPropertyAsValue(PAN_MIDPOINT_PROPERTY, nullptr),
        MIN_PAN_MIDPOINT_DB,
        MAX_PAN_MIDPOINT_DB,
        DEFAULT_PAN_MIDPOINT_DB
    );
    panMidpointSlider.setTooltip(
        "The level difference between the left and right channels that is drawn half way from the centre to the edge. "
        "Lower values spread small differences wider"
    );
    addAndMakeVisible(panMidpointSlider);

    configureLabel(panEdgeLabel, "Pan edge");
    addAndMakeVisible(panEdgeLabel);
    configureDecibelSlider(
        panEdgeSlider,
        editorState.getPropertyAsValue(PAN_EDGE_PROPERTY, nullptr),
        MIN_PAN_EDGE_DB,
        MAX_PAN_EDGE_DB,
        DEFAULT_PAN_EDGE_DB
    );
    panEdgeSlider.setTooltip(
        "The level difference between the channels from which sound is drawn at the edge. "
        "Between the midpoint and this, the curve flattens, so only nearly fully panned sound reaches the edge"
    );
    addAndMakeVisible(panEdgeSlider);

    // The edge stays above the midpoint, so moving one into the other pushes the other along.
    panMidpointSlider.onValueChange = [this] {
        const double lowestEdge = panMidpointSlider.getValue() + MIN_PAN_CURVE_GAP_DB;
        if (panEdgeSlider.getValue() < lowestEdge) {
            panEdgeSlider.setValue(lowestEdge);
        }
    };
    panEdgeSlider.onValueChange = [this] {
        const double highestMidpoint = panEdgeSlider.getValue() - MIN_PAN_CURVE_GAP_DB;
        if (panMidpointSlider.getValue() > highestMidpoint) {
            panMidpointSlider.setValue(highestMidpoint);
        }
    };

    setSize(PANEL_WIDTH, PANEL_HEIGHT);
}

/// Fills the panel background.
void SettingsPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(palette::GREY_SEMI_DARK));
}

/// Stacks the title, the toggles, and the labelled sliders.
void SettingsPanel::resized()
{
    auto area = getLocalBounds().reduced(MARGIN);
    titleLabel.setBounds(area.removeFromTop(ROW_HEIGHT));
    for (auto* button :
         {
             &phaseColoursButton,
             &melScaleButton,
             &solidRenderingButton,
             &showFpsButton,
             &bypassRenderingButton,
             &hideSpectrogramButton,
         })
    {
        button->setBounds(area.removeFromTop(ROW_HEIGHT));
    }
    persistenceLabel.setBounds(area.removeFromTop(SLIDER_LABEL_HEIGHT));
    persistenceSlider.setBounds(area.removeFromTop(ROW_HEIGHT));
    contrastLabel.setBounds(area.removeFromTop(SLIDER_LABEL_HEIGHT));
    contrastSlider.setBounds(area.removeFromTop(ROW_HEIGHT));
    panMidpointLabel.setBounds(area.removeFromTop(SLIDER_LABEL_HEIGHT));
    panMidpointSlider.setBounds(area.removeFromTop(ROW_HEIGHT));
    panEdgeLabel.setBounds(area.removeFromTop(SLIDER_LABEL_HEIGHT));
    panEdgeSlider.setBounds(area.removeFromTop(ROW_HEIGHT));
}
}  // namespace esgrove::image_control
