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
constexpr int PANEL_HEIGHT {302};
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
/// Step of the focus slider, in dB.
constexpr double FOCUS_STEP {0.5};

/// Sets up a label above a slider.
void configureLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(textFont(LABEL_FONT_HEIGHT));
    label.setBorderSize({});
    label.setJustificationType(juce::Justification::bottomLeft);
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
        std::pair<juce::ToggleButton*, const char*> {&analyzeOutputButton, ANALYZE_OUTPUT_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&phaseColoursButton, PHASE_COLOURS_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&melScaleButton, MEL_SCALE_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&showFpsButton, SHOW_FPS_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&bypassRenderingButton, BYPASS_RENDERING_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&hideSpectrogramButton, HIDE_SPECTROGRAM_PROPERTY},
    };
    analyzeOutputButton.setTooltip("Show the processed output, or the input before the bands are changed");
    phaseColoursButton.setTooltip(
        "Colour particles by phase, blue for in-phase and red for anti-phase sound, instead of red bass to blue highs"
    );
    melScaleButton.setTooltip(
        "Space the frequencies like the mel scale of AudioBatch's spectrogram, with less room for the bass"
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

    configureLabel(focusLabel, "Focus");
    addAndMakeVisible(focusLabel);
    focusSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, VALUE_BOX_WIDTH, VALUE_BOX_HEIGHT);
    focusSlider.setRange(MIN_FOCUS_DB, MAX_FOCUS_DB, FOCUS_STEP);
    focusSlider.setTextValueSuffix(" dB");
    focusSlider.setDoubleClickReturnValue(true, DEFAULT_FOCUS_DB);
    focusSlider.getValueObject().referTo(editorState.getPropertyAsValue(FOCUS_PROPERTY, nullptr));
    focusSlider.setTooltip(
        "How large a level difference between the left and right channels reaches the edges of the display. "
        "At 18 dB, a sound 18 dB louder on the left is drawn at the left edge, and one 9 dB louder halfway there. "
        "Lower values spread small differences wider, and higher values keep room for hard-panned sounds"
    );
    addAndMakeVisible(focusSlider);

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
             &analyzeOutputButton,
             &phaseColoursButton,
             &melScaleButton,
             &showFpsButton,
             &bypassRenderingButton,
             &hideSpectrogramButton,
         })
    {
        button->setBounds(area.removeFromTop(ROW_HEIGHT));
    }
    persistenceLabel.setBounds(area.removeFromTop(SLIDER_LABEL_HEIGHT));
    persistenceSlider.setBounds(area.removeFromTop(ROW_HEIGHT));
    focusLabel.setBounds(area.removeFromTop(SLIDER_LABEL_HEIGHT));
    focusSlider.setBounds(area.removeFromTop(ROW_HEIGHT));
}
}  // namespace esgrove::image_control
