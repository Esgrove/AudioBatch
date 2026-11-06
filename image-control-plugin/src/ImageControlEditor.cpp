/// Implements the ImageControl editor: the header controls bound to the editor settings and the output gain,
/// the layout and saved size, and the timer that feeds the analyzer and refreshes the spectrogram and band ranges.

#include "ImageControlEditor.h"

#include "analysis/SpatialAnalyzer.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/BandStrip.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>

namespace esgrove::image_control
{
namespace
{
/// Default editor width, in pixels.
constexpr int DEFAULT_WIDTH {1000};
/// Default editor height, in pixels.
constexpr int DEFAULT_HEIGHT {660};
/// Smallest editor width, in pixels.
constexpr int MIN_WIDTH {760};
/// Smallest editor height, in pixels.
constexpr int MIN_HEIGHT {540};
/// Largest editor width, in pixels.
constexpr int MAX_WIDTH_PIXELS {2800};
/// Largest editor height, in pixels.
constexpr int MAX_HEIGHT_PIXELS {1800};
/// Height of the header row, in pixels.
constexpr int HEADER_HEIGHT {36};
/// Gap between sections, in pixels.
constexpr int GAP {8};
/// Share of the height below the header that the spectrogram takes.
constexpr float SPECTROGRAM_SHARE {0.58F};
/// Frames drained from the analyzer ring per pop.
constexpr std::size_t DRAIN_CHUNK {4096};
/// Font height of the title.
constexpr float TITLE_FONT_HEIGHT {18.0F};
/// Width of the title, in pixels.
constexpr int TITLE_WIDTH {140};
/// Width of the analyzer tap button, in pixels.
constexpr int TAP_BUTTON_WIDTH {140};
/// Vertical inset of the analyzer tap button in the header, in pixels.
constexpr int TAP_BUTTON_INSET {2};
/// Width of the persistence label, in pixels.
constexpr int PERSISTENCE_LABEL_WIDTH {78};
/// Width of the persistence slider, in pixels.
constexpr int PERSISTENCE_SLIDER_WIDTH {220};
/// Width of the persistence value box, in pixels.
constexpr int PERSISTENCE_TEXT_BOX_WIDTH {56};
/// Step of the persistence slider, in seconds.
constexpr double PERSISTENCE_STEP {0.01};
/// Width of the output label, in pixels.
constexpr int OUTPUT_LABEL_WIDTH {52};
/// Width of the output gain slider, in pixels.
constexpr int OUTPUT_SLIDER_WIDTH {200};
/// Width of the output gain value box, in pixels.
constexpr int OUTPUT_TEXT_BOX_WIDTH {64};
/// Height of the value boxes in the header, in pixels.
constexpr int HEADER_TEXT_BOX_HEIGHT {18};
/// Editor background, as ARGB.
constexpr juce::uint32 EDITOR_BACKGROUND {0xff12151c};

/// Returns the text of the analyzer tap button for a tap.
juce::String tapButtonText(const bool analyzeOutput)
{
    return analyzeOutput ? "Showing: Output" : "Showing: Input";
}
}  // namespace

/// Creates the editor, binds its controls to the processor's parameters and editor settings,
/// and discards audio that queued while no editor was open.
ImageControlEditor::ImageControlEditor(ImageControlProcessor& owner) :
    AudioProcessorEditor(owner),
    imageControlProcessor(owner),
    spectrogram(analyzer, owner.getParameters()),
    leftScratch(DRAIN_CHUNK, 0.0F),
    rightScratch(DRAIN_CHUNK, 0.0F),
    vblankAttachment(this, [this] { refreshDisplay(); })
{
    setLookAndFeel(&lookAndFeel);
    auto& editorState = owner.getEditorState();
    auto& parameters = owner.getParameters();

    titleLabel.setText("ImageControl", juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(TITLE_FONT_HEIGHT, juce::Font::bold));
    addAndMakeVisible(titleLabel);

    tapButton.setClickingTogglesState(true);
    tapButton.getToggleStateValue().referTo(editorState.getPropertyAsValue(ANALYZE_OUTPUT_PROPERTY, nullptr));
    tapButton.setButtonText(tapButtonText(tapButton.getToggleState()));
    tapButton.setTooltip("Switch the spectrogram between the input and the output of the plugin");
    tapButton.onStateChange = [this] {
        const auto text = tapButtonText(tapButton.getToggleState());
        if (tapButton.getButtonText() != text) {
            tapButton.setButtonText(text);
            analyzer.clear();
        }
    };
    addAndMakeVisible(tapButton);

    persistenceLabel.setText("Persistence", juce::dontSendNotification);
    addAndMakeVisible(persistenceLabel);
    persistenceSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    persistenceSlider.setTextBoxStyle(
        juce::Slider::TextBoxRight, false, PERSISTENCE_TEXT_BOX_WIDTH, HEADER_TEXT_BOX_HEIGHT
    );
    persistenceSlider.setRange(MIN_PERSISTENCE_SECONDS, MAX_PERSISTENCE_SECONDS, PERSISTENCE_STEP);
    persistenceSlider.setSkewFactorFromMidPoint(DEFAULT_PERSISTENCE_SECONDS);
    persistenceSlider.setTextValueSuffix(" s");
    persistenceSlider.setDoubleClickReturnValue(true, DEFAULT_PERSISTENCE_SECONDS);
    persistenceSlider.getValueObject().referTo(editorState.getPropertyAsValue(PERSISTENCE_PROPERTY, nullptr));
    persistenceSlider.onValueChange = [this] { analyzer.setPersistence(persistenceSlider.getValue()); };
    addAndMakeVisible(persistenceSlider);
    analyzer.setPersistence(persistenceSlider.getValue());

    outputLabel.setText("Output", juce::dontSendNotification);
    addAndMakeVisible(outputLabel);
    outputSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    outputSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, OUTPUT_TEXT_BOX_WIDTH, HEADER_TEXT_BOX_HEIGHT);
    outputSlider.setDoubleClickReturnValue(true, 0.0);
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        parameters, OUTPUT_GAIN_ID, outputSlider
    );
    addAndMakeVisible(outputSlider);

    addAndMakeVisible(spectrogram);
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        bandStrips.at(band) = std::make_unique<BandStrip>(parameters, editorState, band);
        addAndMakeVisible(*bandStrips.at(band));
    }
    updateBandRanges();
    drainAnalyzerFifo(false);

    setResizable(true, true);
    setResizeLimits(MIN_WIDTH, MIN_HEIGHT, MAX_WIDTH_PIXELS, MAX_HEIGHT_PIXELS);
    setSize(
        static_cast<int>(editorState.getProperty(EDITOR_WIDTH_PROPERTY, DEFAULT_WIDTH)),
        static_cast<int>(editorState.getProperty(EDITOR_HEIGHT_PROPERTY, DEFAULT_HEIGHT))
    );
}

/// Detaches the output slider and the look and feel before the components go away.
ImageControlEditor::~ImageControlEditor()
{
    outputAttachment.reset();
    setLookAndFeel(nullptr);
}

/// Fills the background.
void ImageControlEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(EDITOR_BACKGROUND));
}

/// Lays out the header, the spectrogram, and the band strips, and remembers the size in the editor settings.
void ImageControlEditor::resized()
{
    auto area = getLocalBounds().reduced(GAP);

    auto header = area.removeFromTop(HEADER_HEIGHT - GAP);
    titleLabel.setBounds(header.removeFromLeft(TITLE_WIDTH));
    tapButton.setBounds(header.removeFromLeft(TAP_BUTTON_WIDTH).reduced(0, TAP_BUTTON_INSET));
    header.removeFromLeft(GAP * 2);
    outputSlider.setBounds(header.removeFromRight(OUTPUT_SLIDER_WIDTH));
    outputLabel.setBounds(header.removeFromRight(OUTPUT_LABEL_WIDTH));
    header.removeFromRight(GAP * 2);
    persistenceLabel.setBounds(header.removeFromLeft(PERSISTENCE_LABEL_WIDTH));
    persistenceSlider.setBounds(header.removeFromLeft(PERSISTENCE_SLIDER_WIDTH));
    area.removeFromTop(GAP);

    spectrogram.setBounds(
        area.removeFromTop(juce::roundToInt(static_cast<float>(area.getHeight()) * SPECTROGRAM_SHARE))
    );
    area.removeFromTop(GAP);

    const int stripWidth = (area.getWidth() - GAP * static_cast<int>(BAND_COUNT - 1)) / static_cast<int>(BAND_COUNT);
    for (auto& strip : bandStrips) {
        strip->setBounds(area.removeFromLeft(stripWidth));
        area.removeFromLeft(GAP);
    }

    auto& editorState = imageControlProcessor.getEditorState();
    editorState.setProperty(EDITOR_WIDTH_PROPERTY, getWidth(), nullptr);
    editorState.setProperty(EDITOR_HEIGHT_PROPERTY, getHeight(), nullptr);
}

/// Empties the processor's analyzer ring, passing the audio to the analyzer when analyze is set.
void ImageControlEditor::drainAnalyzerFifo(const bool analyze)
{
    auto& fifo = imageControlProcessor.getAnalyzerFifo();
    while (true) {
        const auto frames = static_cast<std::size_t>(fifo.pop(leftScratch, rightScratch));
        if (frames == 0) {
            return;
        }
        if (analyze) {
            analyzer.pushSamples(
                std::span<const float>(leftScratch).first(frames), std::span<const float>(rightScratch).first(frames)
            );
        }
        if (frames < DRAIN_CHUNK) {
            return;
        }
    }
}

/// Feeds the analyzer with the audio queued since the last display refresh.
/// Renders the spectrogram image again only when the analyzer analyzed new frames,
/// and repaints the crossover lines and band ranges only when the crossovers moved,
/// so refreshes without changes cost almost nothing at high refresh rates.
/// Prepares the analyzer again when the host changed the sample rate.
void ImageControlEditor::refreshDisplay()
{
    const double sampleRate = imageControlProcessor.getSampleRate() > 0.0 ? imageControlProcessor.getSampleRate()
                                                                          : ANALYZER_DEFAULT_SAMPLE_RATE;
    if (!juce::approximatelyEqual(sampleRate, analyzedSampleRate)) {
        analyzedSampleRate = sampleRate;
        analyzer.prepare(sampleRate);
    }
    drainAnalyzerFifo(true);

    if (analyzer.getFrameCount() != renderedFrameCount) {
        renderedFrameCount = analyzer.getFrameCount();
        spectrogram.refresh();
    }

    const Crossovers crossovers = imageControlProcessor.getCrossovers();
    const bool crossoversMoved
        = !std::ranges::equal(crossovers, displayedCrossovers, [](const float first, const float second) {
              return juce::approximatelyEqual(first, second);
          });
    if (crossoversMoved) {
        displayedCrossovers = crossovers;
        updateBandRanges();
        spectrogram.repaint();
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
