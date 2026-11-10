/// Implements the ImageControl editor: the header with the settings button, the Pre and Post analyzer tap switch,
/// mono, delta, bypass, output gain, and the per-band correlation row,
/// the layout and saved size, the OpenGL context that draws the spectrogram, the analysis worker,
/// the display refresh that wakes the worker, advances the overlays, and sets the renderer's fade,
/// applying the display settings with bypassing and hiding the spectrogram, measuring the frame rate,
/// the header's history and disclosure actions, the A/B/preset toolbar, and the editing and monitoring shortcuts.

#include "ImageControlEditor.h"

#include "analysis/SpatialAnalyzer.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/HistoryAttachments.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/ProcessingToolbar.h"
#include "ui/SettingsPanel.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"
#include "ui/SpectrogramRenderer.h"
#include "ui/SpectrogramWorker.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

namespace esgrove::image_control
{
namespace
{
/// Default editor width, in pixels.
constexpr int DEFAULT_WIDTH {1000};
/// Default editor height, in pixels.
constexpr int DEFAULT_HEIGHT {764};
/// Smallest editor width, in pixels.
constexpr int MIN_EDITOR_WIDTH {760};
/// Smallest editor height, in pixels.
constexpr int MIN_EDITOR_HEIGHT {600};
/// Largest editor width, in pixels.
constexpr int MAX_EDITOR_WIDTH {4096};
/// Largest editor height, in pixels.
constexpr int MAX_EDITOR_HEIGHT {2400};
/// Height of the header row, in pixels.
constexpr int HEADER_HEIGHT {36};
/// Gap between sections, in pixels.
constexpr int GAP {8};
/// Height of the band strip section until the divider is dragged, in pixels.
constexpr int DEFAULT_BAND_SECTION_HEIGHT {304};
/// Smallest height of the spectrogram, in pixels.
constexpr int MIN_SPECTROGRAM_HEIGHT {160};
/// How far the divider reaches beyond the gap on each side, so it is easy to hover and grab, in pixels.
constexpr int DIVIDER_GRAB_PADDING {4};
/// Longest time one display refresh advances the spectrogram, in seconds,
/// so a pause in refreshes, such as a hidden window, does not fade everything at once.
constexpr double MAX_REFRESH_STEP_SECONDS {0.1};
/// Time over which the refresh rate is averaged, in seconds.
constexpr double FRAME_RATE_WINDOW_SECONDS {0.5};
/// Font height of the title.
constexpr float TITLE_FONT_HEIGHT {18.0F};
/// Width of the title, in pixels.
constexpr int TITLE_WIDTH {124};
/// Width of the header buttons, in pixels.
constexpr int HEADER_BUTTON_WIDTH {76};
/// Vertical inset of the buttons in the header, in pixels.
constexpr int BUTTON_INSET {2};
/// Width of the compact undo, redo, and disclosure buttons.
constexpr int ICON_BUTTON_WIDTH {24};
/// Gap between compact icon buttons.
constexpr int ICON_BUTTON_GAP {4};
/// Width of the vector disclosure glyph, matching AudioBatch's section controls.
constexpr float CHEVRON_WIDTH {10.0F};
/// Height of the vector disclosure glyph.
constexpr float CHEVRON_HEIGHT {5.0F};
/// Stroke shared by the header's font-independent vector symbols.
constexpr float ICON_STROKE_WIDTH {1.5F};
/// Inset keeps the disclosure glyph compact inside its full button hit target.
constexpr int CHEVRON_INSET {7};
/// Width of each half of the Pre and Post analyzer tap switch, in pixels.
constexpr int TAP_BUTTON_WIDTH {56};
/// Width of the output label, in pixels.
constexpr int OUTPUT_LABEL_WIDTH {52};
/// Width of the output gain slider, in pixels.
constexpr int OUTPUT_SLIDER_WIDTH {190};
/// Width of the output gain value box, in pixels.
constexpr int VALUE_BOX_WIDTH {70};
/// Height of the output gain value box, in pixels.
constexpr int VALUE_BOX_HEIGHT {20};
/// Maximum host parameter label length used for a local history action.
constexpr int HISTORY_NAME_LENGTH {100};
/// Opacity of the band strips while the plugin is bypassed.
constexpr float BYPASSED_STRIP_ALPHA {0.45F};

/// Toggles a boolean parameter in a host change gesture of its own.
void toggleParameter(juce::RangedAudioParameter& parameter, ParameterHistory& history)
{
    const auto generation = history.generation();
    history.beginEdit(parameter.getName(HISTORY_NAME_LENGTH));
    history.touch(parameter.paramID);
    parameter.beginChangeGesture();
    if (generation == history.generation()) {
        parameter.setValueNotifyingHost(parameter.getValue() >= 0.5F ? 0.0F : 1.0F);
    }
    parameter.endChangeGesture();
    if (generation == history.generation()) {
        history.endEdit();
    }
}

/// Returns a boolean editor setting.
bool booleanSetting(const juce::ValueTree& editorState, const char* property)
{
    return static_cast<bool>(editorState.getProperty(property));
}
}  // namespace

/// Creates the editor, binds its controls to the processor's parameters,
/// and starts the analysis worker, which first discards audio that queued while no editor was open.
ImageControlEditor::ImageControlEditor(ImageControlProcessor& owner) :
    AudioProcessorEditor(owner),
    ComponentMovementWatcher(this),
    imageControlProcessor(owner),
    processingToolbar(owner),
    spectrogram(owner.getParameters(), owner.getEditorState(), owner.getHistory()),
    spectrogramWorker(owner.getAnalyzerFifo(), spectrogramRenderer),
    tooltips(*this),
    vblankAttachment(this, [this](const double timestamp) { refreshDisplay(timestamp); })
{
    setLookAndFeel(&lookAndFeel);
    // Clicks on controls that take no keyboard focus focus the editor, so its shortcuts work.
    setWantsKeyboardFocus(true);
    auto& editorState = owner.getEditorState();
    auto& parameters = owner.getParameters();

    titleLabel.setText("ImageControl", juce::dontSendNotification);
    titleLabel.setFont(textFont(TITLE_FONT_HEIGHT).boldened());
    titleLabel.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_SUPER_LIGHT));
    addAndMakeVisible(titleLabel);
    configureHeaderIcons();
    undoButton.onClick = [this] {
        processingToolbar.undo();
        updateHistoryButtons();
    };
    redoButton.onClick = [this] {
        processingToolbar.redo();
        updateHistoryButtons();
    };
    processingToolbarButton.onClick = [this] {
        auto& state = imageControlProcessor.getEditorState();
        const bool shown = static_cast<bool>(state.getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, true));
        state.setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, !shown, nullptr);
        applyViewSettings();
    };
    addAndMakeVisible(undoButton);
    addAndMakeVisible(redoButton);
    addAndMakeVisible(processingToolbarButton);

    bypassButton.setClickingTogglesState(true);
    bypassButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(palette::ORANGE));
#if JUCE_MAC
    const juce::String shortcutModifier = "Cmd";
#else
    const juce::String shortcutModifier = "Ctrl";
#endif
    bypassButton.setTooltip(
        "Pass the input through unprocessed, keeping the same latency (" + shortcutModifier + "+B)"
    );
    bypassAttachment
        = std::make_unique<HistoryButtonAttachment>(parameters, BYPASS_ID, bypassButton, owner.getHistory());
    addAndMakeVisible(bypassButton);

    deltaButton.setClickingTogglesState(true);
    deltaButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(palette::YELLOW));
    deltaButton.setColour(juce::TextButton::textColourOnId, juce::Colour(palette::GREY_DARK));
    deltaButton.setTooltip(
        "Output only what the processing changes: the output minus the input. Bypass turns it off (" + shortcutModifier
        + "+D)"
    );
    deltaAttachment = std::make_unique<HistoryButtonAttachment>(parameters, DELTA_ID, deltaButton, owner.getHistory());
    addAndMakeVisible(deltaButton);

    monoButton.setClickingTogglesState(true);
    monoButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(palette::BLUE));
    monoButton.setTooltip(
        "Fold the final output to mono after Delta or bypass, including Post analysis and offline renders"
    );
    monoAttachment = std::make_unique<HistoryButtonAttachment>(parameters, MONO_ID, monoButton, owner.getHistory());
    addAndMakeVisible(monoButton);

    settingsButton.setTooltip(
        "Display settings: the colours, the frequency scale, the contrast, the rendering style, and how particles fade"
    );
    settingsButton.onClick = [this] { openSettings(); };
    addAndMakeVisible(settingsButton);

    // The two halves of the analyzer tap switch follow the editor setting in applyViewSettings,
    // so a restored state shows the right half.
    preButton.setConnectedEdges(juce::Button::ConnectedOnRight);
    postButton.setConnectedEdges(juce::Button::ConnectedOnLeft);
    preButton.setTooltip("Show the input of the plugin in the spectrogram, before the bands are changed");
    postButton.setTooltip("Show the output of the plugin in the spectrogram, or the difference while Delta is on");
    preButton.onClick = [this] { setAnalyzeOutput(false); };
    postButton.onClick = [this] { setAnalyzeOutput(true); };
    addAndMakeVisible(preButton);
    addAndMakeVisible(postButton);

    outputLabel.setText("Output", juce::dontSendNotification);
    addAndMakeVisible(outputLabel);
    outputSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, VALUE_BOX_WIDTH, VALUE_BOX_HEIGHT);
    outputSlider.setDoubleClickReturnValue(true, 0.0);
    outputSlider.setTooltip("Level of the output after the bands are joined. Double-click to reset");
    outputSlider.setTitle("Output gain");
    outputAttachment
        = std::make_unique<HistorySliderAttachment>(parameters, OUTPUT_GAIN_ID, outputSlider, owner.getHistory());
    addAndMakeVisible(outputSlider);

    addAndMakeVisible(processingToolbar);
    addAndMakeVisible(spectrogram);
    divider.setDragCallback([this](const int delta) {
        bandSectionHeight -= delta;
        resized();
    });
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        bandStrips.at(band) = std::make_unique<BandStrip>(parameters, editorState, band, owner.getHistory());
        addAndMakeVisible(*bandStrips.at(band));
        correlationMeters.at(band) = std::make_unique<BandCorrelationMeter>(bandName(band));
        addAndMakeVisible(*correlationMeters.at(band));
    }
    addAndMakeVisible(divider);
    addChildComponent(tooltips);
    updateBandRanges();

    processingToolbarShown = static_cast<bool>(editorState.getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, true));
    processingToolbar.setVisible(processingToolbarShown);
    correlationShown = static_cast<bool>(editorState.getProperty(SHOW_BAND_CORRELATION_PROPERTY, true));
    for (const auto& meter : correlationMeters) {
        meter->setVisible(correlationShown);
    }

    // The saved layout is read before the resize limits are set,
    // because setting them constrains the still empty editor to the minimum size and lays it out.
    // The layout is saved only once it is restored, so that minimum size never replaces the saved one.
    bandSectionHeight
        = static_cast<int>(editorState.getProperty(BAND_SECTION_HEIGHT_PROPERTY, DEFAULT_BAND_SECTION_HEIGHT));
    const auto savedWidth = static_cast<int>(editorState.getProperty(EDITOR_WIDTH_PROPERTY, DEFAULT_WIDTH));
    const auto savedHeight = static_cast<int>(editorState.getProperty(EDITOR_HEIGHT_PROPERTY, DEFAULT_HEIGHT));
    const int savedBandSectionHeight = bandSectionHeight;

    setResizable(true, true);
    setResizeLimits(MIN_EDITOR_WIDTH, MIN_EDITOR_HEIGHT, MAX_EDITOR_WIDTH, MAX_EDITOR_HEIGHT);
    bandSectionHeight = savedBandSectionHeight;
    setSize(savedWidth, savedHeight);
    layoutRestored = true;
    resized();
    // After the first size, so hiding the spectrogram can shrink the editor from it.
    // It also hands the worker its first settings before the worker starts.
    applyViewSettings();
    spectrogramWorker.start();

    // OpenGL 4.1 core is the newest macOS offers, and the shaders need 3.3.
    // The frames follow the editor's VBlank callback instead of a free-running render loop.
    openGLContext.setPreferredVersion({4, 1});
    openGLContext.setPreferredProfile(juce::OpenGLProfile::core);
    openGLContext.setRenderer(&spectrogramRenderer);
    openGLContext.setComponentPaintingEnabled(true);
    openGLContext.setContinuousRepainting(false);
    openGLContext.attachTo(*this);
}

/// Stops the analysis worker, detaches the OpenGL context,
/// which stops the render thread and releases the GL resources,
/// then detaches the controls and the look and feel before the components go away.
ImageControlEditor::~ImageControlEditor()
{
    imageControlProcessor.setCorrelationEnabled(false);
    outputSlider.endDrag();
    spectrogramWorker.stop();
    openGLContext.detach();
    bypassAttachment.reset();
    deltaAttachment.reset();
    outputAttachment.reset();
    setLookAndFeel(nullptr);
}

/// Creates compact vector symbols with full button hit targets and stable, font-independent accessible identities.
void ImageControlEditor::configureHeaderIcons()
{
    undoButton.setComponentID("processing-undo");
    undoButton.setTitle("Undo processing edit");
    undoButton.setDescription("Undo processing edit");
    undoButton.setWantsKeyboardFocus(true);
    undoButton.setExplicitFocusOrder(1);
    redoButton.setComponentID("processing-redo");
    redoButton.setTitle("Redo processing edit");
    redoButton.setDescription("Redo processing edit");
    redoButton.setWantsKeyboardFocus(true);
    redoButton.setExplicitFocusOrder(2);
    processingToolbarButton.setComponentID("processing-toolbar-toggle");
    processingToolbarButton.setTitle("A/B comparison and presets");
    processingToolbarButton.setDescription("Show or hide A/B comparison and presets");
    processingToolbarButton.setWantsKeyboardFocus(true);
    processingToolbarButton.setExplicitFocusOrder(3);
    processingToolbarButton.setToggleable(true);
    processingToolbar.setExplicitFocusOrder(4);

    constexpr juce::Point<float> ARROW_END {12.0F, 11.0F};
    constexpr juce::Point<float> ARROW_CURVE_FIRST {15.0F, 4.0F};
    constexpr juce::Point<float> ARROW_CURVE_SECOND {7.0F, 0.0F};
    constexpr juce::Point<float> ARROW_TIP {1.0F, 6.0F};
    constexpr juce::Point<float> ARROW_HEAD_TOP {1.0F, 2.0F};
    constexpr juce::Point<float> ARROW_HEAD_RIGHT {5.0F, 6.0F};
    juce::Path arrowPath;
    arrowPath.startNewSubPath(ARROW_END);
    arrowPath.cubicTo(ARROW_CURVE_FIRST, ARROW_CURVE_SECOND, ARROW_TIP);
    arrowPath.startNewSubPath(ARROW_HEAD_TOP);
    arrowPath.lineTo(ARROW_TIP);
    arrowPath.lineTo(ARROW_HEAD_RIGHT);
    juce::DrawablePath arrow;
    arrow.setPath(arrowPath);
    arrow.setFill(juce::Colours::transparentBlack);
    arrow.setStrokeFill(juce::Colour(palette::GREY_SUPER_LIGHT));
    arrow.setStrokeType(
        juce::PathStrokeType(ICON_STROKE_WIDTH, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
    );
    undoButton.setImages(&arrow);
    arrowPath.applyTransform(juce::AffineTransform::scale(-1.0F, 1.0F));
    arrow.setPath(arrowPath);
    redoButton.setImages(&arrow);

    // A row below the button collapses upward and expands downward, with no animated transition.
    juce::Path chevronPath;
    chevronPath.startNewSubPath(0.0F, CHEVRON_HEIGHT);
    chevronPath.lineTo(CHEVRON_WIDTH * 0.5F, 0.0F);
    chevronPath.lineTo(CHEVRON_WIDTH, CHEVRON_HEIGHT);
    juce::DrawablePath collapseChevron;
    collapseChevron.setPath(chevronPath);
    collapseChevron.setFill(juce::Colours::transparentBlack);
    collapseChevron.setStrokeFill(juce::Colour(palette::GREY_MIDDLE_LIGHT));
    collapseChevron.setStrokeThickness(ICON_STROKE_WIDTH);
    chevronPath.applyTransform(
        juce::AffineTransform::rotation(juce::MathConstants<float>::pi, CHEVRON_WIDTH * 0.5F, CHEVRON_HEIGHT * 0.5F)
    );
    juce::DrawablePath expandChevron;
    expandChevron.setPath(chevronPath);
    expandChevron.setFill(juce::Colours::transparentBlack);
    expandChevron.setStrokeFill(juce::Colour(palette::GREY_MIDDLE_LIGHT));
    expandChevron.setStrokeThickness(ICON_STROKE_WIDTH);
    processingToolbarButton.setImages(&expandChevron, nullptr, nullptr, nullptr, &collapseChevron);
    processingToolbarButton.setEdgeIndent(CHEVRON_INSET);
    processingToolbarButton.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    processingToolbarButton.setColour(juce::DrawableButton::backgroundOnColourId, juce::Colours::transparentBlack);
}

/// Keeps always-visible history availability and descriptive tooltips synchronized with processor-owned history.
void ImageControlEditor::updateHistoryButtons()
{
    const auto& history = imageControlProcessor.getHistory();
    undoButton.setEnabled(history.canUndo() && !history.isEditing());
    redoButton.setEnabled(history.canRedo() && !history.isEditing());
    undoButton.setTooltip(history.canUndo() ? "Undo " + history.undoDescription() : "No processing edit to undo");
    redoButton.setTooltip(history.canRedo() ? "Redo " + history.redoDescription() : "No processing edit to redo");
}

/// Fills the background, leaving the plot of the spectrogram unpainted so the OpenGL picture shows through.
void ImageControlEditor::paint(juce::Graphics& graphics)
{
    if (spectrogram.isVisible()) {
        const auto plot = spectrogram.getPlotBounds() + spectrogram.getPosition().toFloat();
        graphics.excludeClipRegion(plot.getSmallestIntegerContainer());
    }
    graphics.fillAll(juce::Colour(palette::GREY_DARK));
}

/// Handles the editor's keyboard shortcuts, which also reach it from controls that do not use the key:
/// S toggles the solo of the band under the pointer, in the spectrogram or a band strip,
/// and Ctrl or Cmd with D toggles Delta and with B toggles Bypass.
/// Ctrl or Cmd with Z undoes, while Y or Shift+Z redoes, except when a text field owns editing focus.
/// Each toggle is its own host gesture. Other keys go on to the host.
bool ImageControlEditor::keyPressed(const juce::KeyPress& key)
{
    auto& history = imageControlProcessor.getHistory();
    const auto& parameters = imageControlProcessor.getParameters();
    const auto modifiers = key.getModifiers();
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    const bool textEditing = dynamic_cast<const juce::TextEditor*>(focused) != nullptr;
    const auto letter = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
    if (!textEditing && modifiers.isCommandDown() && !modifiers.isAltDown()) {
        const bool redoShortcut
            = (letter == 'z' && modifiers.isShiftDown()) || (letter == 'y' && !modifiers.isShiftDown());
        if (redoShortcut) {
            processingToolbar.redo();
            updateHistoryButtons();
            return true;
        }
        if (letter == 'z') {
            processingToolbar.undo();
            updateHistoryButtons();
            return true;
        }
    }
    if (textEditing) {
        return false;
    }
    const auto character = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
    const bool commandOnly = modifiers.isCommandDown() && !modifiers.isAltDown() && !modifiers.isShiftDown();
    if (commandOnly && character == 'd') {
        toggleParameter(*parameters.getParameter(DELTA_ID), history);
        return true;
    }
    if (commandOnly && character == 'b') {
        toggleParameter(*parameters.getParameter(BYPASS_ID), history);
        return true;
    }
    if (character == 's' && !modifiers.isCommandDown() && !modifiers.isCtrlDown() && !modifiers.isAltDown()) {
        const auto band = bandUnderPointer();
        if (!band.has_value()) {
            return false;
        }
        toggleParameter(*parameters.getParameter(soloParameterId(*band)), history);
        return true;
    }
    return false;
}

/// Lays out the header, the spectrogram, the divider, and the band strips,
/// and remembers the size and the band section height in the editor settings once the saved layout is restored.
/// While the spectrogram is hidden, the band strips fill the editor below the header,
/// and the saved height stays the one to restore when the spectrogram is shown again.
void ImageControlEditor::resized()
{
    auto area = getLocalBounds().reduced(GAP);

    auto header = area.removeFromTop(HEADER_HEIGHT - GAP);
    // Compact widths keep all processing controls visible and separate at the minimum editor width.
    constexpr int COMPACT_HEADER_THRESHOLD {940};
    constexpr int COMPACT_TITLE_WIDTH {100};
    constexpr int COMPACT_OUTPUT_WIDTH {126};
    constexpr int COMPACT_OUTPUT_LABEL_WIDTH {46};
    const bool compact = getWidth() < COMPACT_HEADER_THRESHOLD;
    const int buttonWidth = compact ? 60 : HEADER_BUTTON_WIDTH;
    const int tapWidth = compact ? 42 : TAP_BUTTON_WIDTH;
    titleLabel.setBounds(header.removeFromLeft(compact ? COMPACT_TITLE_WIDTH : TITLE_WIDTH));
    settingsButton.setBounds(header.removeFromLeft(compact ? 64 : HEADER_BUTTON_WIDTH).reduced(0, BUTTON_INSET));
    header.removeFromLeft(GAP);
    preButton.setBounds(header.removeFromLeft(tapWidth).reduced(0, BUTTON_INSET));
    postButton.setBounds(header.removeFromLeft(tapWidth).reduced(0, BUTTON_INSET));
    header.removeFromLeft(GAP);
    undoButton.setBounds(header.removeFromLeft(ICON_BUTTON_WIDTH).reduced(0, BUTTON_INSET));
    header.removeFromLeft(ICON_BUTTON_GAP);
    redoButton.setBounds(header.removeFromLeft(ICON_BUTTON_WIDTH).reduced(0, BUTTON_INSET));
    processingToolbarButton.setBounds(header.removeFromRight(ICON_BUTTON_WIDTH).reduced(0, BUTTON_INSET));
    header.removeFromRight(ICON_BUTTON_GAP);
    outputSlider.setBounds(header.removeFromRight(compact ? COMPACT_OUTPUT_WIDTH : OUTPUT_SLIDER_WIDTH));
    outputLabel.setBounds(header.removeFromRight(compact ? COMPACT_OUTPUT_LABEL_WIDTH : OUTPUT_LABEL_WIDTH));
    header.removeFromRight(GAP);
    bypassButton.setBounds(header.removeFromRight(buttonWidth).reduced(0, BUTTON_INSET));
    header.removeFromRight(GAP);
    deltaButton.setBounds(header.removeFromRight(buttonWidth).reduced(0, BUTTON_INSET));
    header.removeFromRight(GAP);
    monoButton.setBounds(header.removeFromRight(buttonWidth).reduced(0, BUTTON_INSET));
    area.removeFromTop(GAP);
    if (processingToolbarShown) {
        processingToolbar.setBounds(area.removeFromTop(PROCESSING_TOOLBAR_HEIGHT));
        area.removeFromTop(GAP);
    } else {
        processingToolbar.setBounds({});
    }

    const int correlationHeight = correlationShown ? BAND_CORRELATION_HEIGHT + GAP : 0;
    if (!spectrogramHidden) {
        const int maxBandSectionHeight
            = std::max(BAND_STRIP_MIN_HEIGHT, area.getHeight() - GAP - MIN_SPECTROGRAM_HEIGHT - correlationHeight);
        bandSectionHeight = std::clamp(bandSectionHeight, BAND_STRIP_MIN_HEIGHT, maxBandSectionHeight);
        const auto bandArea = area.removeFromBottom(bandSectionHeight + correlationHeight);
        const auto dividerArea = area.removeFromBottom(GAP);
        divider.setBounds(dividerArea.expanded(0, DIVIDER_GRAB_PADDING));
        spectrogram.setBounds(area);
        area = bandArea;
    }

    auto correlationArea = correlationShown ? area.removeFromTop(BAND_CORRELATION_HEIGHT) : juce::Rectangle<int> {};
    if (correlationShown) {
        area.removeFromTop(GAP);
    }
    const int stripWidth = (area.getWidth() - GAP * static_cast<int>(BAND_COUNT - 1)) / static_cast<int>(BAND_COUNT);
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        bandStrips.at(band)->setBounds(area.removeFromLeft(stripWidth));
        correlationMeters.at(band)->setBounds(correlationArea.removeFromLeft(stripWidth));
        area.removeFromLeft(GAP);
        correlationArea.removeFromLeft(GAP);
    }

    if (!layoutRestored || layoutTransition) {
        return;
    }
    auto& editorState = imageControlProcessor.getEditorState();
    editorState.setProperty(EDITOR_WIDTH_PROPERTY, getWidth(), nullptr);
    if (!spectrogramHidden) {
        editorState.setProperty(EDITOR_HEIGHT_PROPERTY, getHeight(), nullptr);
    }
    editorState.setProperty(BAND_SECTION_HEIGHT_PROPERTY, bandSectionHeight, nullptr);
}

/// Returns the band under the pointer: the band hovered in the spectrogram, or the band strip the pointer is over.
std::optional<std::size_t> ImageControlEditor::bandUnderPointer() const
{
    if (spectrogram.isVisible()) {
        if (const auto band = spectrogram.getHoveredBand()) {
            return band;
        }
    }
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        if (bandStrips.at(band)->isMouseOver(true)) {
            return band;
        }
    }
    return std::nullopt;
}

/// Returns the editor height that fits the header and the band strips without the spectrogram.
int ImageControlEditor::compactHeight() const
{
    return HEADER_HEIGHT + GAP * 2 + (processingToolbarShown ? PROCESSING_TOOLBAR_HEIGHT + GAP : 0)
        + std::max(bandSectionHeight, BAND_STRIP_MIN_HEIGHT) + (correlationShown ? BAND_CORRELATION_HEIGHT + GAP : 0);
}

/// Returns the shared persistence of the particle fade and correlation trend/range, in seconds.
double ImageControlEditor::persistenceSeconds() const
{
    return static_cast<double>(imageControlProcessor.getEditorState().getProperty(PERSISTENCE_PROPERTY));
}

/// Returns what the renderer draws: nothing while the spectrogram is hidden,
/// the background while rendering is bypassed, and otherwise the particles or their solid layers.
SpectrogramRenderMode ImageControlEditor::renderMode() const
{
    if (spectrogramHidden) {
        return SpectrogramRenderMode::hidden;
    }
    if (renderingBypassed) {
        return SpectrogramRenderMode::background;
    }
    return booleanSetting(imageControlProcessor.getEditorState(), SOLID_RENDERING_PROPERTY)
        ? SpectrogramRenderMode::solid
        : SpectrogramRenderMode::particles;
}

/// Hides the spectrogram and shrinks the editor to the header and the band strips, with only the width resizable,
/// or shows it again at the saved height.
void ImageControlEditor::applySpectrogramHidden(const bool hidden)
{
    const auto& editorState = imageControlProcessor.getEditorState();
    const int expandedHeight = static_cast<int>(editorState.getProperty(EDITOR_HEIGHT_PROPERTY, DEFAULT_HEIGHT));
    const int savedBandHeight = bandSectionHeight;
    const juce::ScopedValueSetter<bool> transition(layoutTransition, true);
    spectrogramHidden = hidden;
    if (hidden) {
        spectrogram.endDrag();
    }
    spectrogram.setVisible(!hidden);
    divider.setVisible(!hidden);
    spectrogramWorker.requestClear();
    if (hidden) {
        const int height = compactHeight();
        setResizeLimits(MIN_EDITOR_WIDTH, height, MAX_EDITOR_WIDTH, height);
        setSize(getWidth(), height);
    } else {
        setResizeLimits(MIN_EDITOR_WIDTH, MIN_EDITOR_HEIGHT, MAX_EDITOR_WIDTH, MAX_EDITOR_HEIGHT);
        bandSectionHeight = savedBandHeight;
        setSize(getWidth(), expandedHeight);
    }
    // Same size or not, the layout and the background change.
    resized();
    repaint();
}

/// Applies row visibility and compact sizing together so a restored layout cannot overwrite expanded dimensions.
void ImageControlEditor::applyLayoutSettings()
{
    const auto& editorState = imageControlProcessor.getEditorState();
    const bool showToolbar = static_cast<bool>(editorState.getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, true));
    const bool toolbarChanged = processingToolbarShown != showToolbar;
    processingToolbarShown = showToolbar;
    processingToolbar.setVisible(showToolbar);
    processingToolbarButton.setToggleState(showToolbar, juce::dontSendNotification);
    processingToolbarButton.setTooltip(
        showToolbar ? "Hide A/B comparison and presets" : "Show A/B comparison and presets"
    );
    const bool showCorrelation = static_cast<bool>(editorState.getProperty(SHOW_BAND_CORRELATION_PROPERTY, true));
    const bool correlationChanged = correlationShown != showCorrelation;
    if (correlationChanged) {
        correlationShown = showCorrelation;
        for (const auto& meter : correlationMeters) {
            meter->setVisible(showCorrelation);
        }
    }
    const bool hidden = booleanSetting(editorState, HIDE_SPECTROGRAM_PROPERTY);
    if (shownHidden != hidden) {
        shownHidden = hidden;
        applySpectrogramHidden(hidden);
    } else if (toolbarChanged || correlationChanged) {
        if (hidden) {
            // Compact mode's fixed height follows the visible rows without replacing the saved expanded height.
            const juce::ScopedValueSetter<bool> transition(layoutTransition, true);
            const int height = compactHeight();
            setResizeLimits(MIN_EDITOR_WIDTH, height, MAX_EDITOR_WIDTH, height);
            setSize(getWidth(), height);
        }
        resized();
        repaint();
    }
}

/// Applies the display settings and the bypass to the analysis worker, the spectrogram, and the band strips.
/// The settings change through the settings panel or a restored state, so this runs on every refresh.
/// Changing the tap, the colours, or the scale clears the particles, so the old view does not linger.
void ImageControlEditor::applyViewSettings()
{
    // Keep consuming preset completions and observing edits even while the second row is collapsed.
    processingToolbar.refresh();
    updateHistoryButtons();
    applyLayoutSettings();
    const auto& editorState = imageControlProcessor.getEditorState();
    const bool smoothCorrelationReadout
        = static_cast<bool>(editorState.getProperty(SMOOTH_CORRELATION_READOUT_PROPERTY, true));
    for (const auto& meter : correlationMeters) {
        meter->setSmoothedReadout(smoothCorrelationReadout);
    }
    updateCorrelationMonitoring();
    const bool bypassRendering = booleanSetting(editorState, BYPASS_RENDERING_PROPERTY);
    if (shownRenderingBypassed != bypassRendering) {
        shownRenderingBypassed = bypassRendering;
        renderingBypassed = bypassRendering;
        spectrogram.setRenderingBypassed(bypassRendering);
        // Resuming starts from a clean picture and analysis instead of a frozen one.
        spectrogramWorker.requestClear();
    }
    const bool tapOutput = booleanSetting(editorState, ANALYZE_OUTPUT_PROPERTY);
    const bool phaseColours = booleanSetting(editorState, PHASE_COLOURS_PROPERTY);
    const auto scale
        = booleanSetting(editorState, MEL_SCALE_PROPERTY) ? FrequencyScale::mel : FrequencyScale::logarithmic;
    if (shownTapOutput != tapOutput || shownPhaseColours != phaseColours || shownScale != scale) {
        preButton.setToggleState(!tapOutput, juce::dontSendNotification);
        postButton.setToggleState(tapOutput, juce::dontSendNotification);
        spectrogram.setFrequencyScale(scale);
        spectrogramWorker.requestClear();
        shownTapOutput = tapOutput;
        shownPhaseColours = phaseColours;
        shownScale = scale;
    }

    const std::array positionSettings {
        static_cast<double>(editorState.getProperty(PAN_MIDPOINT_PROPERTY)),
        static_cast<double>(editorState.getProperty(PAN_EDGE_PROPERTY)),
    };
    if (!shownPositionSettings.has_value()
        || !std::ranges::equal(
            positionSettings,
            *shownPositionSettings,
            [](const double first, const double second) { return juce::approximatelyEqual(first, second); }
        ))
    {
        analysisSettings.positionCurve = StereoPositionCurve(positionSettings.front(), positionSettings.back());
        shownPositionSettings = positionSettings;
        spectrogram.setPositionCurve(analysisSettings.positionCurve);
    }

    const double sampleRate = imageControlProcessor.getSampleRate();
    analysisSettings.sampleRate = sampleRate > 0.0 ? sampleRate : ANALYZER_DEFAULT_SAMPLE_RATE;
    analysisSettings.persistenceSeconds = persistenceSeconds();
    analysisSettings.contrast
        = static_cast<double>(editorState.getProperty(CONTRAST_PROPERTY, DEFAULT_CONTRAST_PERCENT))
        / MAX_CONTRAST_PERCENT;
    analysisSettings.scale = scale;
    analysisSettings.colours = phaseColours ? SpectrogramColours::phase : SpectrogramColours::frequency;
    updateSpectrogramMonitoring();

    if (booleanSetting(editorState, SHOW_FPS_PROPERTY)) {
        spectrogram.setFrameStatistics(measuredFramesPerSecond, spectrogramRenderer.gpuFrameMilliseconds());
    } else {
        spectrogram.setFrameStatistics(std::nullopt, std::nullopt);
    }
    spectrogram.setRenderingUnavailable(spectrogramRenderer.isUnavailable());

    const bool bypassed = bypassButton.getToggleState();
    if (shownBypassed != bypassed) {
        for (const auto& strip : bandStrips) {
            strip->setAlpha(bypassed ? BYPASSED_STRIP_ALPHA : 1.0F);
        }
        shownBypassed = bypassed;
    }
}

/// Leaves geometry to resized. The watcher is needed only for peer and ancestor visibility changes.
void ImageControlEditor::componentMovedOrResized(bool /*wasMoved*/, bool /*wasResized*/) {}

/// Tracks host attachment and removal even when the editor's own visible flag does not change.
void ImageControlEditor::componentPeerChanged()
{
    updateCorrelationMonitoring();
    updateSpectrogramMonitoring();
}

/// Stops optional analysis when the host hides the editor or any ancestor, even without another VBlank callback.
void ImageControlEditor::componentVisibilityChanged()
{
    updateCorrelationMonitoring();
    updateSpectrogramMonitoring();
}

/// Keeps the worker draining without FFT work unless a visible, usable spectrogram can display the result.
/// Sending visibility transitions synchronously also advances the worker's source epoch before audio can resume.
void ImageControlEditor::updateSpectrogramMonitoring()
{
    const auto& editorState = imageControlProcessor.getEditorState();
    const bool analyze = isShowing() && !booleanSetting(editorState, HIDE_SPECTROGRAM_PROPERTY)
        && !booleanSetting(editorState, BYPASS_RENDERING_PROPERTY) && !spectrogramRenderer.isUnavailable();
    const bool changed = analysisSettings.analyze != analyze;
    analysisSettings.analyze = analyze;
    spectrogramWorker.setSettings(analysisSettings);
    if (changed) {
        spectrogramWorker.wake();
    }
}

/// Disables optional metering and clears presentation history synchronously when its view is hidden.
void ImageControlEditor::updateCorrelationMonitoring()
{
    const bool enabled = correlationShown && isShowing();
    imageControlProcessor.setCorrelationEnabled(enabled);
    if (!enabled) {
        shownCorrelationGeneration.reset();
        for (const auto& meter : correlationMeters) {
            if (meter != nullptr) {
                meter->clearHistory();
            }
        }
    }
}

/// Updates the measured rate of frames the renderer drew twice a second.
void ImageControlEditor::measureFramesPerSecond(const double elapsed)
{
    frameRateClock += elapsed;
    if (frameRateClock >= FRAME_RATE_WINDOW_SECONDS) {
        const int frames = spectrogramRenderer.renderedFrameCount();
        measuredFramesPerSecond = static_cast<double>(frames - measuredFrameCount) / frameRateClock;
        measuredFrameCount = frames;
        frameRateClock = 0.0;
    }
}

/// Opens the display settings in a call-out box pointing at the settings button.
/// The box owns the panel and closes when it loses focus.
void ImageControlEditor::openSettings()
{
    juce::CallOutBox::launchAsynchronously(
        std::make_unique<SettingsPanel>(imageControlProcessor.getEditorState()), settingsButton.getBounds(), this
    );
}

/// Selects the analyzer tap from the Pre and Post switch: the output of the plugin, or its input.
/// The switch shows the setting on the next refresh.
void ImageControlEditor::setAnalyzeOutput(const bool output)
{
    imageControlProcessor.getEditorState().setProperty(ANALYZE_OUTPUT_PROPERTY, output, nullptr);
    applyViewSettings();
}

/// Runs on every display refresh:
/// applies the settings, wakes the analysis worker to turn the audio queued since the last refresh into particles,
/// advances the overlays by the time that passed, hands the fade and the plot to the renderer,
/// and asks the OpenGL context for a frame.
/// When the editor is not showing or the spectrogram is hidden, bypassed, or unavailable,
/// the worker keeps discarding queued audio without FFT work.
/// Also follows the crossovers.
void ImageControlEditor::refreshDisplay(const double timestamp)
{
    const double elapsed = lastRefreshTimestamp > 0.0 ? timestamp - lastRefreshTimestamp : 0.0;
    lastRefreshTimestamp = timestamp;
    measureFramesPerSecond(elapsed);

    applyViewSettings();
    if (correlationShown && isShowing()) {
        const auto snapshot = imageControlProcessor.getBandCorrelationSnapshot();
        if (shownCorrelationGeneration != snapshot.generation) {
            for (const auto& meter : correlationMeters) {
                meter->clearHistory();
            }
            shownCorrelationGeneration = snapshot.generation;
        }
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            correlationMeters.at(band)->setCorrelation(snapshot.readings.at(band), elapsed, persistenceSeconds());
        }
    }
    spectrogramWorker.wake();
    const double step = std::clamp(elapsed, 0.0, MAX_REFRESH_STEP_SECONDS);
    spectrogram.advance(step);
    tooltips.advance(step);
    spectrogramRenderer.submitFrame(
        step, persistenceSeconds(), spectrogram.getPlotBounds() + spectrogram.getPosition().toFloat(), renderMode()
    );
    openGLContext.triggerRepaint();

    const Crossovers crossovers = imageControlProcessor.getCrossovers();
    if (!sameCrossovers(crossovers, displayedCrossovers)) {
        displayedCrossovers = crossovers;
        updateBandRanges();
    }
}

/// Shows the frequency range of each band, which follows the crossovers.
void ImageControlEditor::updateBandRanges()
{
    const Crossovers crossovers = imageControlProcessor.getCrossovers();
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        juce::String text;
        if (band == 0) {
            text = "Below " + formatFrequency(crossovers.front());
        } else if (band == BAND_COUNT - 1) {
            text = "Above " + formatFrequency(crossovers.back());
        } else {
            text = formatFrequency(crossovers.at(band - 1)) + " to " + formatFrequency(crossovers.at(band));
        }
        bandStrips.at(band)->setRangeText(text);
    }
}
}  // namespace esgrove::image_control
