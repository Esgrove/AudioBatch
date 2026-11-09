/// Implements the ImageControl editor: the header with the settings button, the Pre and Post analyzer tap switch,
/// mono audition, delta, bypass, output gain, and the per-band correlation row,
/// the layout and saved size, the OpenGL context that draws the spectrogram, the analysis worker,
/// the display refresh that wakes the worker, advances the overlays, and sets the renderer's fade,
/// applying the display settings with bypassing and hiding the spectrogram, measuring the frame rate,
/// and the keyboard shortcuts for solo, delta, and bypass.

#include "ImageControlEditor.h"

#include "analysis/SpatialAnalyzer.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/SettingsPanel.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"
#include "ui/SpectrogramRenderer.h"
#include "ui/SpectrogramWorker.h"

#include <algorithm>
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
constexpr int DEFAULT_HEIGHT {680};
/// Smallest editor width, in pixels.
constexpr int MIN_EDITOR_WIDTH {760};
/// Smallest editor height, in pixels.
constexpr int MIN_EDITOR_HEIGHT {560};
/// Largest editor width, in pixels.
constexpr int MAX_EDITOR_WIDTH {4096};
/// Largest editor height, in pixels.
constexpr int MAX_EDITOR_HEIGHT {2400};
/// Height of the header row, in pixels.
constexpr int HEADER_HEIGHT {36};
/// Gap between sections, in pixels.
constexpr int GAP {8};
/// Height of the band strip section until the divider is dragged, in pixels.
constexpr int DEFAULT_BAND_SECTION_HEIGHT {260};
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
/// Opacity of the band strips while the plugin is bypassed.
constexpr float BYPASSED_STRIP_ALPHA {0.45F};

/// Toggles a boolean parameter in a host change gesture of its own.
void toggleParameter(juce::RangedAudioParameter& parameter)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost(parameter.getValue() >= 0.5F ? 0.0F : 1.0F);
    parameter.endChangeGesture();
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
    spectrogram(owner.getParameters(), owner.getEditorState()),
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
        = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(parameters, BYPASS_ID, bypassButton);
    addAndMakeVisible(bypassButton);

    deltaButton.setClickingTogglesState(true);
    deltaButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(palette::YELLOW));
    deltaButton.setColour(juce::TextButton::textColourOnId, juce::Colour(palette::GREY_DARK));
    deltaButton.setTooltip(
        "Output only what the processing changes: the output minus the input. Bypass turns it off (" + shortcutModifier
        + "+D)"
    );
    deltaAttachment
        = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(parameters, DELTA_ID, deltaButton);
    addAndMakeVisible(deltaButton);

    monoButton.setClickingTogglesState(true);
    monoButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(palette::BLUE));
    monoButton.setToggleState(owner.isMonoAuditionEnabled(), juce::dontSendNotification);
    monoButton.setTooltip("Audition the live output in mono. Not saved, automated, or applied to offline renders");
    monoButton.onClick = [this] { imageControlProcessor.setMonoAuditionEnabled(monoButton.getToggleState()); };
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
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        parameters, OUTPUT_GAIN_ID, outputSlider
    );
    addAndMakeVisible(outputSlider);

    addAndMakeVisible(spectrogram);
    divider.setDragCallback([this](const int delta) {
        bandSectionHeight -= delta;
        resized();
    });
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        bandStrips.at(band) = std::make_unique<BandStrip>(parameters, editorState, band);
        addAndMakeVisible(*bandStrips.at(band));
        correlationMeters.at(band) = std::make_unique<BandCorrelationMeter>(bandName(band));
        addAndMakeVisible(*correlationMeters.at(band));
    }
    addAndMakeVisible(divider);
    addChildComponent(tooltips);
    updateBandRanges();

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
/// Each toggle is its own host gesture. Other keys go on to the host.
bool ImageControlEditor::keyPressed(const juce::KeyPress& key)
{
    const auto& parameters = imageControlProcessor.getParameters();
    const auto modifiers = key.getModifiers();
    const auto character = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
    const bool commandOnly = modifiers.isCommandDown() && !modifiers.isAltDown() && !modifiers.isShiftDown();
    if (commandOnly && character == 'd') {
        toggleParameter(*parameters.getParameter(DELTA_ID));
        return true;
    }
    if (commandOnly && character == 'b') {
        toggleParameter(*parameters.getParameter(BYPASS_ID));
        return true;
    }
    if (character == 's' && !modifiers.isCommandDown() && !modifiers.isCtrlDown() && !modifiers.isAltDown()) {
        const auto band = bandUnderPointer();
        if (!band.has_value()) {
            return false;
        }
        toggleParameter(*parameters.getParameter(soloParameterId(*band)));
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
    outputSlider.setBounds(header.removeFromRight(compact ? COMPACT_OUTPUT_WIDTH : OUTPUT_SLIDER_WIDTH));
    outputLabel.setBounds(header.removeFromRight(compact ? COMPACT_OUTPUT_LABEL_WIDTH : OUTPUT_LABEL_WIDTH));
    header.removeFromRight(GAP);
    bypassButton.setBounds(header.removeFromRight(buttonWidth).reduced(0, BUTTON_INSET));
    header.removeFromRight(GAP);
    deltaButton.setBounds(header.removeFromRight(buttonWidth).reduced(0, BUTTON_INSET));
    header.removeFromRight(GAP);
    monoButton.setBounds(header.removeFromRight(buttonWidth).reduced(0, BUTTON_INSET));
    area.removeFromTop(GAP);

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
    return HEADER_HEIGHT + GAP * 2 + std::max(bandSectionHeight, BAND_STRIP_MIN_HEIGHT)
        + (correlationShown ? BAND_CORRELATION_HEIGHT + GAP : 0);
}

/// Returns the persistence setting, the time constant of the particle fade, in seconds.
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

/// Applies the display settings and the bypass to the analysis worker, the spectrogram, and the band strips.
/// The settings change through the settings panel or a restored state, so this runs on every refresh.
/// Changing the tap, the colours, or the scale clears the particles, so the old view does not linger.
void ImageControlEditor::applyViewSettings()
{
    const auto& editorState = imageControlProcessor.getEditorState();
    monoButton.setToggleState(imageControlProcessor.isMonoAuditionEnabled(), juce::dontSendNotification);
    const bool showCorrelation = static_cast<bool>(editorState.getProperty(SHOW_BAND_CORRELATION_PROPERTY, true));
    if (correlationShown != showCorrelation) {
        correlationShown = showCorrelation;
        for (const auto& meter : correlationMeters) {
            meter->setVisible(showCorrelation);
        }
        if (spectrogramHidden) {
            applySpectrogramHidden(true);
        } else {
            resized();
        }
    }
    imageControlProcessor.setCorrelationEnabled(correlationShown && isShowing());
    const bool hidden = booleanSetting(editorState, HIDE_SPECTROGRAM_PROPERTY);
    if (shownHidden != hidden) {
        shownHidden = hidden;
        applySpectrogramHidden(hidden);
    }
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

    const StereoPositionCurve positionCurve(
        static_cast<double>(editorState.getProperty(PAN_MIDPOINT_PROPERTY)),
        static_cast<double>(editorState.getProperty(PAN_EDGE_PROPERTY))
    );
    spectrogram.setPositionCurve(positionCurve);

    const double sampleRate = imageControlProcessor.getSampleRate();
    spectrogramWorker.setSettings({
        .positionCurve = positionCurve,
        .sampleRate = sampleRate > 0.0 ? sampleRate : ANALYZER_DEFAULT_SAMPLE_RATE,
        .persistenceSeconds = persistenceSeconds(),
        .contrast = static_cast<double>(editorState.getProperty(CONTRAST_PROPERTY, DEFAULT_CONTRAST_PERCENT))
            / MAX_CONTRAST_PERCENT,
        .scale = scale,
        .colours = phaseColours ? SpectrogramColours::phase : SpectrogramColours::frequency,
        .analyze = !renderingBypassed && !spectrogramHidden,
    });

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
    imageControlProcessor.setCorrelationEnabled(correlationShown && isShowing());
}

/// Stops optional metering when the host hides the editor or any ancestor, even without another VBlank callback.
void ImageControlEditor::componentVisibilityChanged()
{
    imageControlProcessor.setCorrelationEnabled(correlationShown && isShowing());
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
/// While rendering is bypassed or the spectrogram is hidden, the worker discards the queued audio unanalyzed.
/// Also follows the crossovers.
void ImageControlEditor::refreshDisplay(const double timestamp)
{
    const double elapsed = lastRefreshTimestamp > 0.0 ? timestamp - lastRefreshTimestamp : 0.0;
    lastRefreshTimestamp = timestamp;
    measureFramesPerSecond(elapsed);

    applyViewSettings();
    if (correlationShown) {
        const auto correlations = imageControlProcessor.getBandCorrelations();
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            correlationMeters.at(band)->setCorrelation(correlations.at(band));
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
