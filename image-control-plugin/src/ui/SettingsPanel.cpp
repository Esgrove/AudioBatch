/// Implements compact display settings, strict numeric entry, scoped reset, and a separate shortcuts action.

#include "ui/SettingsPanel.h"

#include "Parameters.h"
#include "ui/ControlSlider.h"
#include "ui/ImageControlLookAndFeel.h"

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <utility>

namespace esgrove::image_control
{
namespace
{
/// Preferred width before the caller bounds the visible height.
constexpr int PANEL_WIDTH {390};
/// Insets and compact row sizes keep the complete settings comfortably readable.
constexpr int MARGIN {12};
constexpr int ROW_HEIGHT {28};
constexpr int TOP_INSET {4};
constexpr int SECTION_HEIGHT {24};
constexpr int SECTION_TOP_GAP {12};
constexpr int SECTION_BOTTOM_GAP {8};
constexpr int SLIDER_LABEL_HEIGHT {18};
constexpr int FOOTER_GAP {8};
/// Value boxes leave enough room for the explicit units.
constexpr int VALUE_BOX_WIDTH {76};
constexpr int VALUE_BOX_HEIGHT {22};
/// Choice captions align while preserving space for both full option names.
constexpr int CHOICE_LABEL_WIDTH {112};
/// Display typography follows the ordinary editor controls.
constexpr float LABEL_FONT_HEIGHT {14.0F};
constexpr float SECTION_FONT_HEIGHT {18.0F};
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
    label.setFont(heading ? textFont(SECTION_FONT_HEIGHT).boldened() : textFont(LABEL_FONT_HEIGHT));
    if (heading) {
        label.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_MIDDLE_LIGHT));
    }
    label.setBorderSize({});
    label.setJustificationType(heading ? juce::Justification::centredLeft : juce::Justification::bottomLeft);
}

/// Accepts a finite, full numeric token and at most one matching optional suffix.
/// Returning the current value prevents malformed edits from silently becoming zero or a partial number.
double settingValueFromText(const juce::String& text, const juce::String& suffix, const double currentValue)
{
    auto token = text.trim();
    if (token.endsWithIgnoreCase(suffix)) {
        token = token.dropLastCharacters(suffix.length()).trimEnd();
    }
    const auto number = token.toStdString();
    return parseFiniteDouble(number, true).value_or(currentValue);
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
    slider.setTextValueSuffix(suffix == "%" ? suffix : " " + suffix);
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
    identifyControl(content, "displaySettingsContent", "Display preferences");
    identifyControl(viewport, "displaySettingsViewport", "Scrollable display settings");
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never);
    addAndMakeVisible(viewport);

    configureLabel(appearanceLabel, "Appearance", true);
    configureLabel(stereoPositionLabel, "Stereo position", true);
    configureLabel(correlationLabel, "Correlation", true);
    configureLabel(displayLabel, "Display behavior", true);
    identifyControl(appearanceLabel, "displayAppearanceSection", "Spectrogram appearance");
    identifyControl(stereoPositionLabel, "displayStereoPositionSection", "Stereo position display mapping");
    identifyControl(correlationLabel, "displayCorrelationSection", "Correlation display");
    identifyControl(displayLabel, "displayBehaviorSection", "Display behavior");
    configureLabel(persistenceLabel, "Persistence");
    configureLabel(contrastLabel, "Contrast");
    configureLabel(panMidpointLabel, "Midpoint (50%)");
    configureLabel(panEdgeLabel, "Edge (99%)");
    configureLabel(colourLabel, "Colour");
    configureLabel(frequencyLabel, "Frequency scale");
    configureLabel(renderingLabel, "Rendering");
    for (auto* label :
         {
             &appearanceLabel,
             &stereoPositionLabel,
             &correlationLabel,
             &displayLabel,
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
        std::pair<juce::ToggleButton*, const char*> {&maximizeSpectrogramButton, MAXIMIZE_SPECTROGRAM_PROPERTY},
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
    maximizeSpectrogramButton.setTooltip(
        "Expand the plot near the panel edges and place frequency and stereo-position labels inside it. "
        "Turn off to restore the outside-axis layout. Window size and sound stay unchanged."
    );

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

    identifyControl(keyboardShortcutsButton, "showKeyboardShortcuts", "Show keyboard shortcuts");
    keyboardShortcutsButton.setTooltip("Open the keyboard and pointer shortcuts submenu. F1.");
    keyboardShortcutsButton.onClick = [this] { showKeyboardShortcuts(); };
    content.addAndMakeVisible(keyboardShortcutsButton);
    identifyControl(backButton, "keyboardShortcutsBack", "Back to Display Settings");
    backButton.onClick = [this] { showPreferences(); };
    addChildComponent(backButton);
    addChildComponent(shortcutsPanel);

    state.addListener(this);
    juce::Desktop::getInstance().addFocusChangeListener(this);
    layoutPreferences(PANEL_WIDTH);
    setSize(PANEL_WIDTH, std::clamp(maximumHeight.value_or(getNaturalHeight()), 1, getNaturalHeight()));
}

/// Detaches message-thread listeners before the settings bindings and viewport content are destroyed.
SettingsPanel::~SettingsPanel()
{
    juce::Desktop::getInstance().removeFocusChangeListener(this);
    state.removeListener(this);
    colourChoice.onChange = nullptr;
    frequencyChoice.onChange = nullptr;
    renderingChoice.onChange = nullptr;
    panMidpointSlider.onValueChange = nullptr;
    panEdgeSlider.onValueChange = nullptr;
    for (auto* slider : {&persistenceSlider, &contrastSlider, &panMidpointSlider, &panEdgeSlider}) {
        slider->valueFromTextFunction = nullptr;
    }
}

/// Measures the laid-out footer plus the bottom inset, so the preferred height always matches the real controls.
int SettingsPanel::getNaturalHeight() const
{
    return keyboardShortcutsButton.getBottom() + MARGIN;
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

/// Replaces preferences with the nested shortcuts reference inside the existing callout.
void SettingsPanel::showKeyboardShortcuts()
{
    showingShortcuts = true;
    viewport.setVisible(false);
    shortcutsPanel.setVisible(true);
    backButton.setVisible(true);
    resized();
    if (isShowing()) {
        backButton.grabKeyboardFocus();
    }
}

/// Returns to preferences, retaining their scroll position and all bound control values.
void SettingsPanel::showPreferences()
{
    showingShortcuts = false;
    shortcutsPanel.setVisible(false);
    backButton.setVisible(false);
    viewport.setVisible(true);
    resized();
    if (isShowing()) {
        keyboardShortcutsButton.grabKeyboardFocus();
    }
}

/// Escape or Alt+Left goes back one submenu level before a subsequent Escape dismisses Settings.
bool SettingsPanel::keyPressed(const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    if (showingShortcuts
        && (key.getKeyCode() == juce::KeyPress::escapeKey
            || (key.getKeyCode() == juce::KeyPress::leftKey && modifiers.isAltDown())))
    {
        showPreferences();
        return true;
    }
    return false;
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
    content.setSize(getWidth(), getNaturalHeight());
    const int width = std::max(1, viewport.getViewWidth());
    layoutPreferences(width);
    content.setSize(width, getNaturalHeight());
    if (showingShortcuts) {
        auto area = getLocalBounds().reduced(MARGIN);
        backButton.setBounds(area.removeFromTop(ROW_HEIGHT));
        area.removeFromTop(FOOTER_GAP);
        shortcutsPanel.setBounds(area);
    }
}

/// Positions each preference row once, leaving the final footer bounds as the source of natural content height.
void SettingsPanel::layoutPreferences(const int width)
{
    int nextY = TOP_INSET;
    const auto takeRow = [width, &nextY](const int height) {
        const juce::Rectangle<int> row(MARGIN, nextY, std::max(1, width - MARGIN * 2), height);
        nextY += height;
        return row;
    };
    const auto placeHeading = [&nextY, &takeRow](juce::Label& heading) {
        if (nextY != TOP_INSET) {
            nextY += SECTION_TOP_GAP;
        }
        heading.setBounds(takeRow(SECTION_HEIGHT));
        nextY += SECTION_BOTTOM_GAP;
    };
    placeHeading(appearanceLabel);
    for (const auto& [label, choice] :
         std::array {
             std::pair<juce::Label*, juce::ComboBox*> {&colourLabel, &colourChoice},
             std::pair<juce::Label*, juce::ComboBox*> {&frequencyLabel, &frequencyChoice},
             std::pair<juce::Label*, juce::ComboBox*> {&renderingLabel, &renderingChoice},
         })
    {
        auto row = takeRow(ROW_HEIGHT).reduced(0, 2);
        label->setBounds(row.removeFromLeft(CHOICE_LABEL_WIDTH));
        choice->setBounds(row);
    }
    contrastLabel.setBounds(takeRow(SLIDER_LABEL_HEIGHT));
    contrastSlider.setBounds(takeRow(ROW_HEIGHT));
    placeHeading(stereoPositionLabel);
    panMidpointLabel.setBounds(takeRow(SLIDER_LABEL_HEIGHT));
    panMidpointSlider.setBounds(takeRow(ROW_HEIGHT));
    panEdgeLabel.setBounds(takeRow(SLIDER_LABEL_HEIGHT));
    panEdgeSlider.setBounds(takeRow(ROW_HEIGHT));
    placeHeading(correlationLabel);
    showBandCorrelationButton.setBounds(takeRow(ROW_HEIGHT));
    smoothCorrelationReadoutButton.setBounds(takeRow(ROW_HEIGHT));
    placeHeading(displayLabel);
    persistenceLabel.setBounds(takeRow(SLIDER_LABEL_HEIGHT));
    persistenceSlider.setBounds(takeRow(ROW_HEIGHT));
    maximizeSpectrogramButton.setBounds(takeRow(ROW_HEIGHT));
    hideSpectrogramButton.setBounds(takeRow(ROW_HEIGHT));
    bypassRenderingButton.setBounds(takeRow(ROW_HEIGHT));
    showFpsButton.setBounds(takeRow(ROW_HEIGHT));
    nextY += FOOTER_GAP;
    resetAppearanceButton.setBounds(takeRow(ROW_HEIGHT));
    nextY += FOOTER_GAP;
    keyboardShortcutsButton.setBounds(takeRow(ROW_HEIGHT));
}
}  // namespace esgrove::image_control
