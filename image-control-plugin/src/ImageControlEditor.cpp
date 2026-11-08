/// Implements the ImageControl editor: the header with the bypass, the settings button, and the output gain,
/// the layout and saved size, the OpenGL context that draws the spectrogram, the analysis worker,
/// the display refresh that wakes the worker, advances the overlays, and sets the renderer's fade,
/// applying the display settings with bypassing and hiding the spectrogram, and measuring the frame rate.

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
constexpr int TITLE_WIDTH {140};
/// Width of the header buttons, in pixels.
constexpr int HEADER_BUTTON_WIDTH {90};
/// Vertical inset of the buttons in the header, in pixels.
constexpr int BUTTON_INSET {2};
/// Width of the output label, in pixels.
constexpr int OUTPUT_LABEL_WIDTH {52};
/// Width of the output gain slider, in pixels.
constexpr int OUTPUT_SLIDER_WIDTH {220};
/// Width of the output gain value box, in pixels.
constexpr int VALUE_BOX_WIDTH {70};
/// Height of the output gain value box, in pixels.
constexpr int VALUE_BOX_HEIGHT {20};
/// Opacity of the band strips while the plugin is bypassed.
constexpr float BYPASSED_STRIP_ALPHA {0.45F};

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
    imageControlProcessor(owner),
    spectrogram(owner.getParameters()),
    spectrogramWorker(owner.getAnalyzerFifo(), spectrogramRenderer),
    tooltips(*this),
    vblankAttachment(this, [this](const double timestamp) { refreshDisplay(timestamp); })
{
    setLookAndFeel(&lookAndFeel);
    auto& editorState = owner.getEditorState();
    auto& parameters = owner.getParameters();

    titleLabel.setText("ImageControl", juce::dontSendNotification);
    titleLabel.setFont(textFont(TITLE_FONT_HEIGHT).boldened());
    titleLabel.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_SUPER_LIGHT));
    addAndMakeVisible(titleLabel);

    bypassButton.setClickingTogglesState(true);
    bypassButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(palette::ORANGE));
    bypassButton.setTooltip("Pass the input through unprocessed, keeping the same latency");
    bypassAttachment
        = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(parameters, BYPASS_ID, bypassButton);
    addAndMakeVisible(bypassButton);

    settingsButton.setTooltip(
        "Display settings: the analyzed signal, the colours, the frequency scale, and how particles fade"
    );
    settingsButton.onClick = [this] { openSettings(); };
    addAndMakeVisible(settingsButton);

    outputLabel.setText("Output", juce::dontSendNotification);
    addAndMakeVisible(outputLabel);
    outputSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, VALUE_BOX_WIDTH, VALUE_BOX_HEIGHT);
    outputSlider.setDoubleClickReturnValue(true, 0.0);
    outputSlider.setTooltip("Level of the output after the bands are joined. Double-click to reset");
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
    }
    addAndMakeVisible(divider);
    addChildComponent(tooltips);
    updateBandRanges();

    bandSectionHeight
        = static_cast<int>(editorState.getProperty(BAND_SECTION_HEIGHT_PROPERTY, DEFAULT_BAND_SECTION_HEIGHT));

    setResizable(true, true);
    setResizeLimits(MIN_EDITOR_WIDTH, MIN_EDITOR_HEIGHT, MAX_EDITOR_WIDTH, MAX_EDITOR_HEIGHT);
    setSize(
        static_cast<int>(editorState.getProperty(EDITOR_WIDTH_PROPERTY, DEFAULT_WIDTH)),
        static_cast<int>(editorState.getProperty(EDITOR_HEIGHT_PROPERTY, DEFAULT_HEIGHT))
    );
    // After the first size, so hiding the spectrogram can shrink the editor from it.
    // It also hands the worker its first settings before the worker starts.
    applyViewSettings();
    spectrogramWorker.start();

    // OpenGL 4.1 core is the newest macOS offers, and the shaders need 3.3.
    // The frames follow the editor's VBlank callback instead of a free-running render loop.
    openGLContext.setPreferredVersion({4, 1});
    openGLContext.setRenderer(&spectrogramRenderer);
    openGLContext.setComponentPaintingEnabled(true);
    openGLContext.setContinuousRepainting(false);
    openGLContext.attachTo(*this);
}

/// Stops the analysis worker, detaches the OpenGL context, which stops the render thread and releases the GL
/// resources, then detaches the controls and the look and feel before the components go away.
ImageControlEditor::~ImageControlEditor()
{
    spectrogramWorker.stop();
    openGLContext.detach();
    bypassAttachment.reset();
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
/// and remembers the size and the band section height in the editor settings.
/// While the spectrogram is hidden, the band strips fill the editor below the header,
/// and the saved height stays the one to restore when the spectrogram is shown again.
void ImageControlEditor::resized()
{
    auto area = getLocalBounds().reduced(GAP);

    auto header = area.removeFromTop(HEADER_HEIGHT - GAP);
    titleLabel.setBounds(header.removeFromLeft(TITLE_WIDTH));
    bypassButton.setBounds(header.removeFromLeft(HEADER_BUTTON_WIDTH).reduced(0, BUTTON_INSET));
    header.removeFromLeft(GAP);
    settingsButton.setBounds(header.removeFromLeft(HEADER_BUTTON_WIDTH).reduced(0, BUTTON_INSET));
    outputSlider.setBounds(header.removeFromRight(OUTPUT_SLIDER_WIDTH));
    outputLabel.setBounds(header.removeFromRight(OUTPUT_LABEL_WIDTH));
    area.removeFromTop(GAP);

    if (!spectrogramHidden) {
        const int maxBandSectionHeight
            = std::max(BAND_STRIP_MIN_HEIGHT, area.getHeight() - GAP - MIN_SPECTROGRAM_HEIGHT);
        bandSectionHeight = std::clamp(bandSectionHeight, BAND_STRIP_MIN_HEIGHT, maxBandSectionHeight);
        const auto bandArea = area.removeFromBottom(bandSectionHeight);
        const auto dividerArea = area.removeFromBottom(GAP);
        divider.setBounds(dividerArea.expanded(0, DIVIDER_GRAB_PADDING));
        spectrogram.setBounds(area);
        area = bandArea;
    }

    const int stripWidth = (area.getWidth() - GAP * static_cast<int>(BAND_COUNT - 1)) / static_cast<int>(BAND_COUNT);
    for (auto& strip : bandStrips) {
        strip->setBounds(area.removeFromLeft(stripWidth));
        area.removeFromLeft(GAP);
    }

    auto& editorState = imageControlProcessor.getEditorState();
    editorState.setProperty(EDITOR_WIDTH_PROPERTY, getWidth(), nullptr);
    if (!spectrogramHidden) {
        editorState.setProperty(EDITOR_HEIGHT_PROPERTY, getHeight(), nullptr);
    }
    editorState.setProperty(BAND_SECTION_HEIGHT_PROPERTY, bandSectionHeight, nullptr);
}

/// Returns the editor height that fits the header and the band strips without the spectrogram.
int ImageControlEditor::compactHeight() const
{
    return HEADER_HEIGHT + GAP * 2 + std::max(bandSectionHeight, BAND_STRIP_MIN_HEIGHT);
}

/// Returns the persistence setting, the time constant of the particle fade, in seconds.
double ImageControlEditor::persistenceSeconds() const
{
    return static_cast<double>(imageControlProcessor.getEditorState().getProperty(PERSISTENCE_PROPERTY));
}

/// Returns what the renderer draws: nothing while the spectrogram is hidden,
/// the background while rendering is bypassed, and the particles otherwise.
SpectrogramRenderMode ImageControlEditor::renderMode() const
{
    if (spectrogramHidden) {
        return SpectrogramRenderMode::hidden;
    }
    return renderingBypassed ? SpectrogramRenderMode::background : SpectrogramRenderMode::particles;
}

/// Hides the spectrogram and shrinks the editor to the header and the band strips, with only the width resizable,
/// or shows it again at the saved height.
void ImageControlEditor::applySpectrogramHidden(const bool hidden)
{
    spectrogramHidden = hidden;
    spectrogram.setVisible(!hidden);
    divider.setVisible(!hidden);
    spectrogramWorker.requestClear();
    if (hidden) {
        const int height = compactHeight();
        setResizeLimits(MIN_EDITOR_WIDTH, height, MAX_EDITOR_WIDTH, height);
        setSize(getWidth(), height);
    } else {
        setResizeLimits(MIN_EDITOR_WIDTH, MIN_EDITOR_HEIGHT, MAX_EDITOR_WIDTH, MAX_EDITOR_HEIGHT);
        const auto& editorState = imageControlProcessor.getEditorState();
        setSize(getWidth(), static_cast<int>(editorState.getProperty(EDITOR_HEIGHT_PROPERTY, DEFAULT_HEIGHT)));
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
        spectrogram.setFrequencyScale(scale);
        spectrogramWorker.requestClear();
        shownTapOutput = tapOutput;
        shownPhaseColours = phaseColours;
        shownScale = scale;
    }

    const double sampleRate = imageControlProcessor.getSampleRate();
    spectrogramWorker.setSettings({
        .sampleRate = sampleRate > 0.0 ? sampleRate : ANALYZER_DEFAULT_SAMPLE_RATE,
        .focusDecibels = static_cast<double>(editorState.getProperty(FOCUS_PROPERTY)),
        .persistenceSeconds = persistenceSeconds(),
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
        for (auto& strip : bandStrips) {
            strip->setAlpha(bypassed ? BYPASSED_STRIP_ALPHA : 1.0F);
        }
        shownBypassed = bypassed;
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
