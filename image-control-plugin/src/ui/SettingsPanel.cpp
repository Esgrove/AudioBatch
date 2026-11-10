/// Implements grouped, scrollable display settings, strict numeric entry, scoped reset, and accessible controls help.

#include "ui/SettingsPanel.h"

#include "Parameters.h"
#include "ui/ControlSlider.h"
#include "ui/ImageControlLookAndFeel.h"
#include <system_error>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace esgrove::image_control
{
namespace
{
/// Preferred panel width and maximum natural height, before the caller's viewport limit.
constexpr int PANEL_WIDTH {390};
constexpr int PANEL_HEIGHT {640};
/// Full content stays scrollable even when the caller allows only a compact popup.
constexpr int CONTENT_HEIGHT {1240};
/// Insets and row sizes keep settings comfortably readable without widening the editor.
constexpr int MARGIN {12};
constexpr int ROW_HEIGHT {28};
constexpr int SECTION_HEIGHT {32};
constexpr int SLIDER_LABEL_HEIGHT {20};
constexpr int HELP_HEIGHT {560};
/// Value boxes leave enough room for the explicit units.
constexpr int VALUE_BOX_WIDTH {76};
constexpr int VALUE_BOX_HEIGHT {22};
/// Choice captions align while preserving space for both full option names.
constexpr int CHOICE_LABEL_WIDTH {112};
/// Display typography follows the ordinary editor controls.
constexpr float TITLE_FONT_HEIGHT {15.0F};
constexpr float LABEL_FONT_HEIGHT {14.0F};
/// Settings use their persisted precision rather than arbitrary keyboard-only increments.
constexpr double PERSISTENCE_STEP {0.01};
constexpr double CONTRAST_STEP_PERCENT {1.0};

/// Gives a control a stable automation-test identity and a complete accessible name.
void identifyControl(juce::Component& control, const juce::String& identifier, const juce::String& title)
{
    control.setComponentID(identifier);
    control.setName(title);
    control.setTitle(title);
    control.setHasFocusOutline(true);
}

/// Sets up a readable section heading or slider label without introducing another tab stop.
void configureLabel(juce::Label& label, const juce::String& text, const bool heading = false)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(heading ? textFont(LABEL_FONT_HEIGHT).boldened() : textFont(LABEL_FONT_HEIGHT));
    label.setBorderSize({});
    label.setJustificationType(juce::Justification::bottomLeft);
}

/// Accepts a finite, full numeric token and at most one matching optional suffix.
/// Returning the current value prevents malformed edits from silently becoming zero or a partial number.
double settingValueFromText(const juce::String& text, const juce::String& suffix, const double currentValue)
{
    auto token = text.trim();
    if (token.endsWithIgnoreCase(suffix)) {
        token = token.dropLastCharacters(suffix.length()).trimEnd();
    }
    auto number = token.toStdString();
    if (!number.empty() && number.front() == '+') {
        number.erase(0, 1);
        if (number.empty() || number.front() == '+' || number.front() == '-') {
            return currentValue;
        }
    }
    double result = 0.0;
    const std::span<const char> characters(number.data(), number.size());
    const auto* end = std::to_address(characters.end());
    const auto parsed = std::from_chars(characters.data(), end, result);
    if (parsed.ec != std::errc {} || parsed.ptr != end || !std::isfinite(result)) {
        return currentValue;
    }
    return result;
}

/// Binds a numeric setting while keeping JUCE's inline text editor strict and unit-aware.
void configureSlider(
    ControlSlider& slider,
    const juce::Value& value,
    const double minimum,
    const double maximum,
    const double interval,
    const double defaultValue,
    const juce::String& suffix
)
{
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, VALUE_BOX_WIDTH, VALUE_BOX_HEIGHT);
    slider.setRange(minimum, maximum, interval);
    slider.setTextValueSuffix(" " + suffix);
    slider.setDoubleClickReturnValue(true, defaultValue);
    slider.valueFromTextFunction
        = [&slider, suffix](const juce::String& text) { return settingValueFromText(text, suffix, slider.getValue()); };
    slider.getValueObject().referTo(value);
}

/// Offers both alternatives explicitly instead of making the default an unlabeled unchecked state.
void configureChoice(
    juce::ComboBox& choice,
    const juce::String& first,
    const juce::String& second,
    juce::ValueTree& state,
    const juce::Identifier& property
)
{
    choice.addItem(first, 1);
    choice.addItem(second, 2);
    choice.setSelectedId(static_cast<bool>(state.getProperty(property)) ? 2 : 1, juce::dontSendNotification);
    choice.onChange
        = [&choice, &state, property] { state.setProperty(property, choice.getSelectedId() == 2, nullptr); };
}
}  // namespace

/// Creates message-thread setting bindings and caps the visible height without clipping any controls.
SettingsPanel::SettingsPanel(juce::ValueTree& editorState, const std::optional<int> maximumHeight) : state(editorState)
{
    identifyControl(*this, "displaySettings", "Display settings");
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    identifyControl(content, "displaySettingsContent", "Display preferences and controls help");
    identifyControl(viewport, "displaySettingsViewport", "Scrollable display settings");
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never);
    addAndMakeVisible(viewport);

    configureLabel(titleLabel, "Display settings", true);
    titleLabel.setFont(textFont(TITLE_FONT_HEIGHT).boldened());
    configureLabel(sharedLabel, "Shared Persistence", true);
    configureLabel(spectrogramLabel, "Spectrogram", true);
    configureLabel(correlationLabel, "Correlation", true);
    configureLabel(advancedLabel, "Advanced", true);
    configureLabel(helpLabel, "Help / controls", true);
    configureLabel(persistenceLabel, "Persistence (spectrogram and correlation)");
    configureLabel(contrastLabel, "Contrast");
    configureLabel(panMidpointLabel, "Display position midpoint (50%)");
    configureLabel(panEdgeLabel, "Display position edge (99%)");
    configureLabel(colourLabel, "Colour");
    configureLabel(frequencyLabel, "Frequency scale");
    configureLabel(renderingLabel, "Rendering");
    for (auto* label :
         {
             &titleLabel,
             &sharedLabel,
             &spectrogramLabel,
             &correlationLabel,
             &advancedLabel,
             &helpLabel,
             &persistenceLabel,
             &contrastLabel,
             &panMidpointLabel,
             &panEdgeLabel,
             &colourLabel,
             &frequencyLabel,
             &renderingLabel,
         })
    {
        content.addAndMakeVisible(*label);
    }

    const std::array toggles {
        std::pair<juce::ToggleButton*, const char*> {&showFpsButton, SHOW_FPS_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&showBandCorrelationButton, SHOW_BAND_CORRELATION_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {
            &smoothCorrelationReadoutButton, SMOOTH_CORRELATION_READOUT_PROPERTY
        },
        std::pair<juce::ToggleButton*, const char*> {&bypassRenderingButton, BYPASS_RENDERING_PROPERTY},
        std::pair<juce::ToggleButton*, const char*> {&hideSpectrogramButton, HIDE_SPECTROGRAM_PROPERTY},
    };
    for (const auto& [button, property] : toggles) {
        identifyControl(*button, property, button->getButtonText());
        button->getToggleStateValue().referTo(state.getPropertyAsValue(property, nullptr));
        content.addAndMakeVisible(*button);
    }
    showFpsButton.setTooltip("Show the spectrogram refresh rate. This diagnostic does not measure audio performance.");
    showBandCorrelationButton.setTooltip("Show each processed band's correlation, independently of the spectrogram.");
    smoothCorrelationReadoutButton.setTooltip(
        "Use shared Persistence for correlation numbers. Off shows live numbers, without changing markers or audio."
    );
    bypassRenderingButton.setTooltip(
        "Stop spectrogram analysis and drawing to save CPU and GPU work. Audio processing continues. "
        "Use the main audio Bypass control to bypass the effect."
    );
    hideSpectrogramButton.setTooltip("Hide the spectrogram and shrink its section. Sound settings remain unchanged.");

    configureChoice(colourChoice, "Frequency", "Phase coherence", state, PHASE_COLOURS_PROPERTY);
    configureChoice(frequencyChoice, "Logarithmic", "Mel", state, MEL_SCALE_PROPERTY);
    configureChoice(renderingChoice, "Particles", "Solid layers", state, SOLID_RENDERING_PROPERTY);
    identifyControl(colourChoice, PHASE_COLOURS_PROPERTY, "Spectrogram colour");
    identifyControl(frequencyChoice, MEL_SCALE_PROPERTY, "Spectrogram frequency scale");
    identifyControl(renderingChoice, SOLID_RENDERING_PROPERTY, "Spectrogram rendering");
    colourChoice.setTooltip(
        "Frequency colours run from red bass to blue highs. Phase coherence runs from blue in-phase to red anti-phase."
    );
    frequencyChoice.setTooltip("Logarithmic gives equal room to octaves. Mel devotes relatively less room to bass.");
    renderingChoice.setTooltip("Choose individual particles or stacked translucent solid layers with outlines.");
    for (auto* choice : {&colourChoice, &frequencyChoice, &renderingChoice}) {
        content.addAndMakeVisible(*choice);
    }

    configureSlider(
        persistenceSlider,
        state.getPropertyAsValue(PERSISTENCE_PROPERTY, nullptr),
        MIN_PERSISTENCE_SECONDS,
        MAX_PERSISTENCE_SECONDS,
        PERSISTENCE_STEP,
        DEFAULT_PERSISTENCE_SECONDS,
        "s"
    );
    persistenceSlider.setSkewFactorFromMidPoint(DEFAULT_PERSISTENCE_SECONDS);
    configureSlider(
        contrastSlider,
        state.getPropertyAsValue(CONTRAST_PROPERTY, nullptr),
        0.0,
        MAX_CONTRAST_PERCENT,
        CONTRAST_STEP_PERCENT,
        DEFAULT_CONTRAST_PERCENT,
        "%"
    );
    configureSlider(
        panMidpointSlider,
        state.getPropertyAsValue(PAN_MIDPOINT_PROPERTY, nullptr),
        MIN_PAN_MIDPOINT_DB,
        MAX_PAN_MIDPOINT_DB,
        PAN_CURVE_STEP_DB,
        DEFAULT_PAN_MIDPOINT_DB,
        "dB"
    );
    configureSlider(
        panEdgeSlider,
        state.getPropertyAsValue(PAN_EDGE_PROPERTY, nullptr),
        MIN_PAN_EDGE_DB,
        MAX_PAN_EDGE_DB,
        PAN_CURVE_STEP_DB,
        DEFAULT_PAN_EDGE_DB,
        "dB"
    );
    identifyControl(persistenceSlider, PERSISTENCE_PROPERTY, "Shared Persistence");
    identifyControl(contrastSlider, CONTRAST_PROPERTY, "Spectrogram contrast");
    identifyControl(panMidpointSlider, PAN_MIDPOINT_PROPERTY, "Display position midpoint (50%)");
    identifyControl(panEdgeSlider, PAN_EDGE_PROPERTY, "Display position edge (99%)");
    persistenceSlider.setTooltip(
        "Particle fade time, correlation trend smoothing, and recent correlation range duration, in seconds."
    );
    contrastSlider.setTooltip(
        "Make stronger sound stand out against quieter sound, especially in the highs. Display only."
    );
    panMidpointSlider.setTooltip(
        "Left/right level difference drawn halfway from centre to edge. Display only, not audio Pan."
    );
    panEdgeSlider.setTooltip(
        "Left/right level difference drawn 99% of the way to the edge. Display only, not audio Pan."
    );
    for (auto* slider : {&persistenceSlider, &contrastSlider, &panMidpointSlider, &panEdgeSlider}) {
        content.addAndMakeVisible(*slider);
    }
    // A colliding edit pushes the peer while retaining the same minimum curve gap.
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

    identifyControl(resetAppearanceButton, "resetDisplayAppearance", "Reset display appearance");
    resetAppearanceButton.setTooltip(
        "Reset colours, frequency scale, rendering, contrast, display position, Persistence, "
        "frame-rate readout, and smooth correlation numbers. "
        "Keep sound, Link, presets, visibility, layout, and stopped analysis."
    );
    resetAppearanceButton.onClick = [this] { resetDisplayAppearance(); };
    content.addAndMakeVisible(resetAppearanceButton);

    identifyControl(helpText, "displayControlsHelp", "Help and controls");
    helpText.setMultiLine(true, true);
    helpText.setReadOnly(true);
    helpText.setScrollbarsShown(false);
    helpText.setFont(textFont(LABEL_FONT_HEIGHT));
    helpText.setText(
        "Tab / Shift+Tab: move between controls.\n"
        "Arrow keys: adjust a focused slider. Shift: finer adjustment.\n"
        "Drag sliders relatively. Hold Shift for fine dragging or wheel steps. Double-click to reset a control.\n"
        "Spectrogram: drag crossovers or bands sideways. Command/Ctrl-drag vertically: Mid. "
        "Option/Alt-drag vertically: Side.\n"
        "Hover section edges to find collapse controls. Visibility toggles remain available here.\n"
        "Focus the divider and use Up / Down to resize (Shift: 1 pixel).\n"
        "S over a band: Solo. Command/Ctrl+B: Bypass. Command/Ctrl+D: Delta.\n"
        "Command/Ctrl+Z: Undo. Command/Ctrl+Y or Command/Ctrl+Shift+Z: Redo.\n"
        "Link follows local Mid gain edits, not host automation. Width and Recover remain independent.\n"
        "Stop spectrogram analysis affects the display only. Audio Bypass is a separate main control.\n"
        "F1: controls help. Command/Ctrl+comma: display settings. Escape: close settings."
    );
    content.addAndMakeVisible(helpText);

    state.addListener(this);
    juce::Desktop::getInstance().addFocusChangeListener(this);
    setSize(PANEL_WIDTH, std::clamp(maximumHeight.value_or(PANEL_HEIGHT), 1, PANEL_HEIGHT));
}

/// Detaches message-thread listeners before the settings bindings and viewport content are destroyed.
SettingsPanel::~SettingsPanel()
{
    juce::Desktop::getInstance().removeFocusChangeListener(this);
    state.removeListener(this);
}

/// Resets display appearance only, leaving sound, links, presets, visibility, layout, and analysis enablement intact.
void SettingsPanel::resetDisplayAppearance()
{
    persistenceSlider.setValue(DEFAULT_PERSISTENCE_SECONDS, juce::sendNotificationSync);
    contrastSlider.setValue(DEFAULT_CONTRAST_PERCENT, juce::sendNotificationSync);
    panMidpointSlider.setValue(DEFAULT_PAN_MIDPOINT_DB, juce::sendNotificationSync);
    panEdgeSlider.setValue(DEFAULT_PAN_EDGE_DB, juce::sendNotificationSync);
    state.setProperty(PHASE_COLOURS_PROPERTY, false, nullptr);
    state.setProperty(MEL_SCALE_PROPERTY, false, nullptr);
    state.setProperty(SOLID_RENDERING_PROPERTY, false, nullptr);
    state.setProperty(SHOW_FPS_PROPERTY, false, nullptr);
    state.setProperty(SMOOTH_CORRELATION_READOUT_PROPERTY, true, nullptr);
}

/// Makes the complete help section visible and focuses its accessible, selectable text when the panel is showing.
void SettingsPanel::showHelp()
{
    revealControl(helpText);
    if (isShowing()) {
        helpText.grabKeyboardFocus();
    }
}

/// Keeps a newly focused control visible, including a slider's nested inline text editor.
void SettingsPanel::globalFocusChanged(juce::Component* focusedComponent)
{
    if (focusedComponent != nullptr && content.isParentOf(focusedComponent)) {
        revealControl(*focusedComponent);
    }
}

/// Scrolls the minimum distance needed to expose a descendant within the viewport.
void SettingsPanel::revealControl(juce::Component& control)
{
    const auto bounds = content.getLocalArea(&control, control.getLocalBounds()).expanded(0, 4);
    const int top = viewport.getViewPositionY();
    const int height = viewport.getViewHeight();
    if (bounds.getY() < top) {
        viewport.setViewPosition(0, bounds.getY());
    } else if (bounds.getBottom() > top + height) {
        viewport.setViewPosition(0, bounds.getHeight() >= height ? bounds.getY() : bounds.getBottom() - height);
    }
}

/// Reflects live state recalls in explicit choices without writing those updates back into the tree.
void SettingsPanel::syncChoices()
{
    colourChoice.setSelectedId(
        static_cast<bool>(state.getProperty(PHASE_COLOURS_PROPERTY)) ? 2 : 1, juce::dontSendNotification
    );
    frequencyChoice.setSelectedId(
        static_cast<bool>(state.getProperty(MEL_SCALE_PROPERTY)) ? 2 : 1, juce::dontSendNotification
    );
    renderingChoice.setSelectedId(
        static_cast<bool>(state.getProperty(SOLID_RENDERING_PROPERTY)) ? 2 : 1, juce::dontSendNotification
    );
}

/// State callbacks are message-thread-only, matching the processor's live editor-tree ownership contract.
void SettingsPanel::valueTreePropertyChanged(juce::ValueTree& /*tree*/, const juce::Identifier& property)
{
    if (property == juce::Identifier(PHASE_COLOURS_PROPERTY) || property == juce::Identifier(MEL_SCALE_PROPERTY)
        || property == juce::Identifier(SOLID_RENDERING_PROPERTY))
    {
        syncChoices();
    }
}

/// Fills the panel background underneath its transparent viewport content.
void SettingsPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(palette::GREY_SEMI_DARK));
}

/// Lays out the full content independently of its bounded scrolling viewport.
void SettingsPanel::resized()
{
    viewport.setBounds(getLocalBounds());
    const int width = std::max(1, getWidth() - viewport.getScrollBarThickness());
    content.setSize(width, CONTENT_HEIGHT);
    auto area = content.getLocalBounds().reduced(MARGIN);
    titleLabel.setBounds(area.removeFromTop(ROW_HEIGHT));
    sharedLabel.setBounds(area.removeFromTop(SECTION_HEIGHT));
    persistenceLabel.setBounds(area.removeFromTop(SLIDER_LABEL_HEIGHT));
    persistenceSlider.setBounds(area.removeFromTop(ROW_HEIGHT));
    spectrogramLabel.setBounds(area.removeFromTop(SECTION_HEIGHT));
    hideSpectrogramButton.setBounds(area.removeFromTop(ROW_HEIGHT));
    for (const auto& [label, choice] :
         std::array {
             std::pair<juce::Label*, juce::ComboBox*> {&colourLabel, &colourChoice},
             std::pair<juce::Label*, juce::ComboBox*> {&frequencyLabel, &frequencyChoice},
             std::pair<juce::Label*, juce::ComboBox*> {&renderingLabel, &renderingChoice},
         })
    {
        auto row = area.removeFromTop(ROW_HEIGHT).reduced(0, 2);
        label->setBounds(row.removeFromLeft(CHOICE_LABEL_WIDTH));
        choice->setBounds(row);
    }
    contrastLabel.setBounds(area.removeFromTop(SLIDER_LABEL_HEIGHT));
    contrastSlider.setBounds(area.removeFromTop(ROW_HEIGHT));
    panMidpointLabel.setBounds(area.removeFromTop(SLIDER_LABEL_HEIGHT));
    panMidpointSlider.setBounds(area.removeFromTop(ROW_HEIGHT));
    panEdgeLabel.setBounds(area.removeFromTop(SLIDER_LABEL_HEIGHT));
    panEdgeSlider.setBounds(area.removeFromTop(ROW_HEIGHT));
    correlationLabel.setBounds(area.removeFromTop(SECTION_HEIGHT));
    showBandCorrelationButton.setBounds(area.removeFromTop(ROW_HEIGHT));
    smoothCorrelationReadoutButton.setBounds(area.removeFromTop(ROW_HEIGHT));
    advancedLabel.setBounds(area.removeFromTop(SECTION_HEIGHT));
    bypassRenderingButton.setBounds(area.removeFromTop(ROW_HEIGHT));
    showFpsButton.setBounds(area.removeFromTop(ROW_HEIGHT));
    area.removeFromTop(8);
    resetAppearanceButton.setBounds(area.removeFromTop(ROW_HEIGHT));
    helpLabel.setBounds(area.removeFromTop(SECTION_HEIGHT));
    helpText.setBounds(area.removeFromTop(HELP_HEIGHT));
}
}  // namespace esgrove::image_control
