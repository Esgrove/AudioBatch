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
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"
#include "ui/SpectrogramRenderer.h"
#include "ui/SpectrogramWorker.h"

#include <algorithm>
#include <array>
#include <cstddef>
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
/// Vertical inset of the buttons in the header, in pixels.
constexpr int BUTTON_INSET {2};
/// Width of the compact undo, redo, and disclosure buttons.
constexpr int ICON_BUTTON_WIDTH {24};
/// Gap between compact icon buttons.
constexpr int ICON_BUTTON_GAP {4};
/// Width of the output label, in pixels.
constexpr int OUTPUT_LABEL_WIDTH {52};
/// Width of the output gain slider, in pixels.
constexpr int OUTPUT_SLIDER_WIDTH {190};
/// Width of the output gain value box, in pixels.
constexpr int VALUE_BOX_WIDTH {70};
/// Height of the output gain value box, in pixels.
constexpr int VALUE_BOX_HEIGHT {20};
/// Opacity of the band strips while the plugin is bypassed.
constexpr float BYPASSED_STRIP_ALPHA {0.72F};
/// Height retained by a hidden display's labelled restore handle.
constexpr int RESTORE_HANDLE_HEIGHT {18};
/// Quiet identity typography fits a preset subtitle within the ordinary header height.
constexpr float PRESET_FONT_HEIGHT {11.0F};
constexpr float COMPACT_BRAND_FONT_HEIGHT {14.0F};
constexpr int COMPACT_BRAND_HEIGHT {15};
/// The local row action reserves only the header portion of the meter column.
constexpr int CORRELATION_ACTION_HEIGHT {20};
/// A full-height hover target separates correlation from the controls without covering either.
constexpr int CONTROLS_ACTION_HEIGHT {12};
constexpr int SECTION_ACTION_WIDTH {24};
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
    layoutGeneration(owner.getHistory().generation()),
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
    presetLabel.setComponentID("header-preset-name");
    presetLabel.setTitle("Current preset");
    presetLabel.setFont(textFont(PRESET_FONT_HEIGHT));
    presetLabel.setBorderSize({});
    presetLabel.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_MIDDLE_LIGHT));
    addChildComponent(presetLabel);
    spectrogramDisclosure.setComponentID("spectrogram-disclosure");
    correlationDisclosure.setComponentID("correlation-disclosure");
    bandControlsDisclosure.setComponentID("band-controls-disclosure");
    bandControlsDisclosure.onClick = [this] {
        auto& state = imageControlProcessor.getEditorState();
        state.setProperty(COMPACT_BAND_CONTROLS_PROPERTY, !compactBandControls, nullptr);
        applyViewSettings();
    };
    spectrogramDisclosure.onClick = [this] {
        auto& state = imageControlProcessor.getEditorState();
        state.setProperty(HIDE_SPECTROGRAM_PROPERTY, !spectrogramHidden, nullptr);
        applyViewSettings();
    };
    correlationDisclosure.onClick = [this] {
        auto& state = imageControlProcessor.getEditorState();
        state.setProperty(SHOW_BAND_CORRELATION_PROPERTY, !correlationShown, nullptr);
        applyViewSettings();
    };
    addAndMakeVisible(spectrogramDisclosure);
    addAndMakeVisible(correlationDisclosure);
    addAndMakeVisible(bandControlsDisclosure);
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

    for (auto* button : {&monoButton, &deltaButton, &bypassButton}) {
        button->setWantsKeyboardFocus(true);
    }
    preButton.setWantsKeyboardFocus(true);
    postButton.setWantsKeyboardFocus(true);
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

    settingsButton.setTooltip("Display settings and controls help (" + shortcutModifier + "+comma, F1 for help)");
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
        const auto& state = imageControlProcessor.getEditorState();
        if (layoutGeneration != imageControlProcessor.getHistory().generation()) {
            divider.cancelDrag();
            applyViewSettings();
            return;
        }
        if (booleanSetting(state, COMPACT_BAND_CONTROLS_PROPERTY) || booleanSetting(state, HIDE_SPECTROGRAM_PROPERTY)) {
            divider.cancelDrag();
            return;
        }
        bandSectionHeight -= delta;
        resized();
    });
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        bandStrips.at(band) = std::make_unique<BandStrip>(parameters, editorState, band, owner.getHistory());
        constexpr int firstBandFocusOrder {16};
        bandStrips.at(band)->setExplicitFocusOrder(firstBandFocusOrder + static_cast<int>(band));
        addAndMakeVisible(*bandStrips.at(band));
        correlationMeters.at(band) = std::make_unique<BandCorrelationMeter>(bandName(band));
        addAndMakeVisible(*correlationMeters.at(band));
    }
    addAndMakeVisible(divider);
    spectrogramDisclosure.toFront(false);
    correlationDisclosure.toFront(false);
    bandControlsDisclosure.toFront(false);
    addChildComponent(tooltips);
    updateBandRanges();

    processingToolbarShown = static_cast<bool>(editorState.getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, true));
    processingToolbar.setVisible(processingToolbarShown);
    compactBandControls = booleanSetting(editorState, COMPACT_BAND_CONTROLS_PROPERTY);
    for (const auto& strip : bandStrips) {
        strip->setCompact(compactBandControls);
    }
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
    setKeyboardRouting(true);
}

/// Stops the analysis worker, detaches the OpenGL context,
/// which stops the render thread and releases the GL resources,
/// then detaches the controls and the look and feel before the components go away.
ImageControlEditor::~ImageControlEditor()
{
    setKeyboardRouting(false);
    settingsCallout.reset();
    settingsPanel.reset();
    imageControlProcessor.setCorrelationEnabled(false);
    outputSlider.endDrag();
    spectrogramWorker.stop();
    openGLContext.detach();
    bypassAttachment.reset();
    deltaAttachment.reset();
    outputAttachment.reset();
    setLookAndFeel(nullptr);
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

/// Lays out the header, the spectrogram, the divider, and the band strips,
/// and remembers the size and the band section height in the editor settings once the saved layout is restored.
/// While the spectrogram is hidden, the band strips fill the editor below the header,
/// and the saved height stays the one to restore when the spectrogram is shown again.
void ImageControlEditor::resized()
{
    auto area = getLocalBounds().reduced(GAP);

    auto header = area.removeFromTop(HEADER_HEIGHT - GAP);
    // Spend narrow-width pressure on spacing first, then flexible labels and buttons, and lastly the output track.
    constexpr int groupGapCount {6};
    constexpr int iconGapCount {2};
    constexpr int minimumGap {2};
    constexpr int preferredButtonWidth {60};
    constexpr int preferredTapWidth {42};
    constexpr int preferredIdentityWidth {TITLE_WIDTH + 16};
    constexpr int fixedIconWidth {ICON_BUTTON_WIDTH * 4};
    constexpr int preferredWidth {
        preferredIdentityWidth + fixedIconWidth + preferredTapWidth * 2 + preferredButtonWidth * 3 + OUTPUT_LABEL_WIDTH
            + OUTPUT_SLIDER_WIDTH + groupGapCount * GAP + iconGapCount * ICON_BUTTON_GAP,
    };
    double pressure = static_cast<double>(std::max(0, preferredWidth - header.getWidth()));
    const auto compress = [&pressure](const int preferred, const int minimum, const int count) {
        const double reduction
            = std::min(static_cast<double>(preferred - minimum), pressure / static_cast<double>(count));
        pressure = std::max(0.0, pressure - reduction * static_cast<double>(count));
        return static_cast<double>(preferred) - reduction;
    };
    const double groupGap = compress(GAP, minimumGap, groupGapCount);
    const double iconGap = compress(ICON_BUTTON_GAP, minimumGap, iconGapCount);
    const double identityWidth = compress(preferredIdentityWidth, 112, 1);
    const double buttonWidth = compress(preferredButtonWidth, 48, 3);
    const double tapWidth = compress(preferredTapWidth, 34, 2);
    const double outputLabelWidth = compress(OUTPUT_LABEL_WIDTH, 42, 1);
    const auto outputWidth = static_cast<double>(header.getWidth()) - identityWidth - fixedIconWidth - tapWidth * 2.0
        - buttonWidth * 3.0 - outputLabelWidth - groupGapCount * groupGap - iconGapCount * iconGap;
    auto headerX = static_cast<double>(header.getX());
    const auto takeHeader = [&headerX, &header](const double width) {
        const int left = juce::roundToInt(headerX);
        headerX += width;
        return juce::Rectangle<int>(left, header.getY(), juce::roundToInt(headerX) - left, header.getHeight());
    };
    auto identity = takeHeader(identityWidth);
    if (presetLabel.isVisible()) {
        titleLabel.setBounds(identity.removeFromTop(COMPACT_BRAND_HEIGHT));
        presetLabel.setBounds(identity);
    } else {
        titleLabel.setBounds(identity);
        presetLabel.setBounds({});
    }
    settingsButton.setBounds(takeHeader(ICON_BUTTON_WIDTH).reduced(0, BUTTON_INSET));
    headerX += groupGap;
    preButton.setBounds(takeHeader(tapWidth).reduced(0, BUTTON_INSET));
    postButton.setBounds(takeHeader(tapWidth).reduced(0, BUTTON_INSET));
    headerX += groupGap;
    undoButton.setBounds(takeHeader(ICON_BUTTON_WIDTH).reduced(0, BUTTON_INSET));
    headerX += iconGap;
    redoButton.setBounds(takeHeader(ICON_BUTTON_WIDTH).reduced(0, BUTTON_INSET));
    headerX += groupGap;
    monoButton.setBounds(takeHeader(buttonWidth).reduced(0, BUTTON_INSET));
    headerX += groupGap;
    deltaButton.setBounds(takeHeader(buttonWidth).reduced(0, BUTTON_INSET));
    headerX += groupGap;
    bypassButton.setBounds(takeHeader(buttonWidth).reduced(0, BUTTON_INSET));
    headerX += groupGap;
    outputLabel.setBounds(takeHeader(outputLabelWidth));
    outputSlider.setBounds(takeHeader(outputWidth));
    headerX += iconGap;
    processingToolbarButton.setBounds(takeHeader(ICON_BUTTON_WIDTH).reduced(0, BUTTON_INSET));
    area.removeFromTop(GAP);
    if (processingToolbarShown) {
        processingToolbar.setBounds(area.removeFromTop(PROCESSING_TOOLBAR_HEIGHT));
        area.removeFromTop(GAP);
    } else {
        processingToolbar.setBounds({});
    }

    if (spectrogramHidden) {
        spectrogramDisclosure.setBounds(area.removeFromTop(RESTORE_HANDLE_HEIGHT));
        area.removeFromTop(GAP);
    }
    const int correlationHeight
        = (correlationShown ? BAND_CORRELATION_HEIGHT : RESTORE_HANDLE_HEIGHT) + CONTROLS_ACTION_HEIGHT;
    int controlsHeight = compactBandControls ? BAND_STRIP_COMPACT_HEIGHT : bandSectionHeight;
    if (!spectrogramHidden) {
        const int maxBandSectionHeight
            = std::max(BAND_STRIP_MIN_HEIGHT, area.getHeight() - GAP - MIN_SPECTROGRAM_HEIGHT - correlationHeight);
        if (!compactBandControls) {
            bandSectionHeight = std::clamp(bandSectionHeight, BAND_STRIP_MIN_HEIGHT, maxBandSectionHeight);
            controlsHeight = bandSectionHeight;
        }
        const auto bandArea = area.removeFromBottom(controlsHeight + correlationHeight);
        const auto dividerArea = area.removeFromBottom(GAP);
        divider.setBounds(dividerArea.expanded(0, DIVIDER_GRAB_PADDING));
        spectrogram.setBounds(area);
        spectrogramDisclosure.setBounds(
            area.withSizeKeepingCentre(SECTION_ACTION_WIDTH, CORRELATION_ACTION_HEIGHT).withY(area.getY())
        );
        area = bandArea;
    }

    auto correlationArea = area.removeFromTop(correlationShown ? BAND_CORRELATION_HEIGHT : RESTORE_HANDLE_HEIGHT);
    correlationDisclosure.setBounds(
        correlationShown ? correlationArea.withSizeKeepingCentre(SECTION_ACTION_WIDTH, CORRELATION_ACTION_HEIGHT)
                               .withY(correlationArea.getY())
                         : correlationArea
    );
    area.removeFromTop(CONTROLS_ACTION_HEIGHT);
    const int stripWidth = (area.getWidth() - GAP * static_cast<int>(BAND_COUNT - 1)) / static_cast<int>(BAND_COUNT);
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        bandStrips.at(band)->setBounds(area.removeFromLeft(stripWidth));
        correlationMeters.at(band)->setBounds(correlationArea.removeFromLeft(stripWidth));
        area.removeFromLeft(GAP);
        correlationArea.removeFromLeft(GAP);
    }
    layoutBandControlsDisclosure();
    updateSettingsBounds();

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

/// Centers the controls action in a dedicated gap, clear of every band value and analysis readout.
void ImageControlEditor::layoutBandControlsDisclosure()
{
    bandControlsDisclosure.setBounds(
        (getWidth() - SECTION_ACTION_WIDTH) / 2,
        bandStrips.front()->getY() - CONTROLS_ACTION_HEIGHT,
        SECTION_ACTION_WIDTH,
        CONTROLS_ACTION_HEIGHT
    );
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
        + RESTORE_HANDLE_HEIGHT + GAP
        + (compactBandControls ? BAND_STRIP_COMPACT_HEIGHT : std::max(bandSectionHeight, BAND_STRIP_MIN_HEIGHT))
        + (correlationShown ? BAND_CORRELATION_HEIGHT : RESTORE_HANDLE_HEIGHT) + CONTROLS_ACTION_HEIGHT;
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
    divider.setVisible(!hidden && !compactBandControls);
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

/// Accepts restored strip geometry and cancels unfinished text before a restored mode can relayout controls.
bool ImageControlEditor::synchronizeRestoredLayout()
{
    const auto generation = imageControlProcessor.getHistory().generation();
    if (layoutGeneration == generation) {
        return false;
    }
    layoutGeneration = generation;
    divider.cancelDrag();
    for (const auto& strip : bandStrips) {
        strip->discardPendingEdits();
    }
    const auto& state = imageControlProcessor.getEditorState();
    bandSectionHeight = std::max(
        BAND_STRIP_MIN_HEIGHT,
        static_cast<int>(state.getProperty(BAND_SECTION_HEIGHT_PROPERTY, DEFAULT_BAND_SECTION_HEIGHT))
    );
    return true;
}

/// Applies row visibility and compact sizing together so a restored layout cannot overwrite expanded dimensions.
void ImageControlEditor::applyLayoutSettings()
{
    const auto& editorState = imageControlProcessor.getEditorState();
    const bool restored = synchronizeRestoredLayout();
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
    const bool compact = booleanSetting(editorState, COMPACT_BAND_CONTROLS_PROPERTY);
    const bool controlsChanged = compactBandControls != compact;
    if (controlsChanged) {
        if (compact && divider.hasKeyboardFocus(true) && isShowing()) {
            bandControlsDisclosure.grabKeyboardFocus();
        }
        compactBandControls = compact;
        for (const auto& strip : bandStrips) {
            strip->setCompact(compact);
        }
    }
    divider.setVisible(!spectrogramHidden && !compactBandControls);
    const bool hidden = booleanSetting(editorState, HIDE_SPECTROGRAM_PROPERTY);
    if (shownHidden != hidden) {
        shownHidden = hidden;
        applySpectrogramHidden(hidden);
    } else if (toolbarChanged || correlationChanged || controlsChanged || restored) {
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
    updateDisplayDisclosures();
    updatePresetIdentity();
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
    preButton.setEnabled(!spectrogramHidden && !bypassRendering);
    postButton.setEnabled(!spectrogramHidden && !bypassRendering);
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

/// Keeps local disclosure targets available to keyboard users and reveals triangles over their display.
void ImageControlEditor::updateDisplayDisclosures()
{
    spectrogramDisclosure.setExpanded(!spectrogramHidden);
    correlationDisclosure.setExpanded(correlationShown);
    const juce::String bandAction
        = compactBandControls ? "Expand band controls" : "Collapse band controls to Mid, Side, Width, and Pan";
    if (bandControlsDisclosure.getToggleState() == compactBandControls
        || bandControlsDisclosure.getTitle() != bandAction)
    {
        bandControlsDisclosure.setExpanded(!compactBandControls);
        bandControlsDisclosure.setTitle(bandAction);
        bandControlsDisclosure.setDescription(bandAction);
        bandControlsDisclosure.setTooltip(bandAction);
    }
    const auto pointer = getMouseXYRelative();
    spectrogramDisclosure.setRevealed(spectrogram.getBounds().contains(pointer));
    bool overCorrelation = false;
    for (const auto& meter : correlationMeters) {
        overCorrelation = overCorrelation || (meter->isVisible() && meter->getBounds().contains(pointer));
    }
    correlationDisclosure.setRevealed(overCorrelation);
    bool overControls = false;
    for (const auto& strip : bandStrips) {
        overControls = overControls || strip->getBounds().contains(pointer);
    }
    bandControlsDisclosure.setRevealed(
        overControls || bandControlsDisclosure.getBounds().contains(pointer)
        || (!spectrogramHidden && divider.getBounds().contains(pointer))
        || (spectrogramHidden && pointer.y >= spectrogramDisclosure.getY()
            && pointer.y < spectrogramDisclosure.getBottom())
    );
}

/// Shows named preset feedback only where the expanded selector is not already showing it.
void ImageControlEditor::updatePresetIdentity()
{
    const auto presentation = imageControlProcessor.getPresetPresentation();
    const auto& provenance = presentation.provenance;
    const bool show = !processingToolbarShown && provenance.has_value();
    juce::String text;
    if (provenance.has_value()) {
        text = provenance->name + (presentation.modified ? " *" : "");
    }
    const bool visibilityChanged = presetLabel.isVisible() != show;
    presetLabel.setText(text, juce::dontSendNotification);
    const auto description = text.isEmpty() ? text : text + " · Slot " + (presentation.comparisonSlot == 0 ? "A" : "B");
    presetLabel.setTooltip(description);
    presetLabel.setDescription(description);
    presetLabel.setVisible(show);
    titleLabel.setFont(textFont(show ? COMPACT_BRAND_FONT_HEIGHT : TITLE_FONT_HEIGHT).boldened());
    if (visibilityChanged) {
        resized();
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
