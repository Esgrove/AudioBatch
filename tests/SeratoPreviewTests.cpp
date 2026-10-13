/// Offscreen cue-arrow and musical-info tests using generated PCM peaks and synthetic metadata.
/// Exercises the actual preview components without devices, local music or redistributed Serato payloads.

#include "AudioInfoPanel.h"
#include "TestSuites.h"
#include "ThumbnailComponent.h"

#include <cmath>

namespace audiobatch::tests
{
namespace
{
/// Creates one second of complete mono or stereo peaks in JUCE's unchanged cache format.
juce::MemoryBlock previewPeaks(juce::AudioFormatManager& formats, const int channels)
{
    juce::AudioThumbnailCache cache(1);
    juce::AudioThumbnail thumbnail(16, formats, cache);
    constexpr int samples = 8192;
    juce::AudioBuffer<float> buffer(channels, samples);
    for (int channel = 0; channel < channels; ++channel) {
        for (int sample = 0; sample < samples; ++sample) {
            buffer.setSample(
                channel, sample, static_cast<float>(0.4 * std::sin(juce::MathConstants<double>::twoPi * sample / 64.0))
            );
        }
    }
    thumbnail.reset(channels, samples, samples);
    thumbnail.addBlock(0, buffer, 0, samples);
    juce::MemoryBlock result;
    juce::MemoryOutputStream output(result, false);
    thumbnail.saveTo(output);
    return result;
}

/// Paints the real preview at the requested physical display scale.
juce::Image renderPreview(ThumbnailComponent& thumbnail, const float scale = 1.0f)
{
    juce::Image image(juce::Image::ARGB, static_cast<int>(260.0f * scale), static_cast<int>(104.0f * scale), true);
    juce::Graphics graphics(image);
    graphics.addTransform(juce::AffineTransform::scale(scale));
    thumbnail.paint(graphics);
    return image;
}

/// Counts exact cue-colour pixels independently of the waveform's blue or indexed spectral brush.
int colourPixels(const juce::Image& image, const juce::Colour colour)
{
    int result = 0;
    for (int row = 0; row < image.getHeight(); ++row) {
        for (int column = 0; column < image.getWidth(); ++column) {
            result += image.getPixelAt(column, row) == colour ? 1 : 0;
        }
    }
    return result;
}

/// Retrieves a visible value from the panel's existing paired label children.
juce::String panelValue(const AudioInfoPanel& panel, const juce::String& name)
{
    for (int index = 0; index + 1 < panel.getNumChildComponents(); index += 2) {
        const auto* field = dynamic_cast<const juce::Label*>(panel.getChildComponent(index));
        const auto* value = dynamic_cast<const juce::Label*>(panel.getChildComponent(index + 1));
        if (field != nullptr && value != nullptr && field->isVisible() && field->getText() == name) {
            return value->getText();
        }
    }
    return {};
}
}  // namespace

/// Verifies presentation and resets independently of tag parsing and async-worker tests.
class SeratoPreviewTests final : public juce::UnitTest
{
public:
    /// Registers the portable GUI slice through its explicit factory.
    SeratoPreviewTests() : juce::UnitTest("Serato preview presentation", "AudioBatch") { }

    /// Checks arrow direction, colour, projection, bounds, gain, resets and info-field precedence.
    void runTest() override
    {
        testCueArrows();
        testCueReset();
        testTrackInfo();
    }

private:
    /// Exercises the same inset time projection at full view, zoom, pan and fractional DPI.
    void testCueArrows()
    {
        beginTest("Cue arrows retain their colour and point inward above unchanged mono/stereo peaks");
        juce::AudioFormatManager formats;
        juce::AudioTransportSource transport;
        ThumbnailComponent thumbnail(formats, transport);
        thumbnail.setSize(260, 104);
        const auto colour = juce::Colour(239, 37, 37);
        auto markers = std::make_shared<serato::Markers>();
        markers->cues.push_back({.positionMilliseconds = 500, .colour = colour, .name = "Test cue"});
        for (const int channels : {1, 2}) {
            expect(thumbnail.loadFromCacheData(previewPeaks(formats, channels)));
            for (const float gain : {0.0f, 1.0f, 4.0f}) {
                thumbnail.setDisplayGain(gain);
                for (const float scale : {1.0f, 1.5f, 2.0f}) {
                    thumbnail.setSeratoMarkers({});
                    const auto before = renderPreview(thumbnail, scale);
                    thumbnail.setSeratoMarkers(markers);
                    const auto after = renderPreview(thumbnail, scale);
                    expect(colourPixels(after, colour) > 20);
                    int changedCentralPixels = 0;
                    for (int row = static_cast<int>(18.0f * scale); row < static_cast<int>(86.0f * scale); ++row) {
                        for (int column = 0; column < after.getWidth(); ++column) {
                            changedCentralPixels
                                += before.getPixelAt(column, row) != after.getPixelAt(column, row) ? 1 : 0;
                        }
                    }
                    expectEquals(changedCentralPixels, 0);
                }
            }
        }
        thumbnail.setDisplayGain(1.0f);
        const auto full = renderPreview(thumbnail);
        expectEquals(full.getPixelAt(130, 3).getARGB(), colour.getARGB());
        expectEquals(full.getPixelAt(132, 9).getARGB(), colour.getARGB());
        expect(full.getPixelAt(132, 3) != colour);
        expectEquals(full.getPixelAt(130, 100).getARGB(), colour.getARGB());
        expectEquals(full.getPixelAt(132, 94).getARGB(), colour.getARGB());
        expect(full.getPixelAt(132, 100) != colour);

        beginTest("Millisecond cues stay aligned through zoom and pan, and outside cues are clipped");
        thumbnail.setRange({0.25, 0.75});
        expectEquals(renderPreview(thumbnail).getPixelAt(130, 3).getARGB(), colour.getARGB());
        thumbnail.setRange({0.4, 0.9});
        expectEquals(renderPreview(thumbnail).getPixelAt(53, 3).getARGB(), colour.getARGB());
        thumbnail.setRange({0.6, 1.1});
        expectEquals(colourPixels(renderPreview(thumbnail), colour), 0);
        thumbnail.setRange({-0.25, 0.75});
        expectEquals(renderPreview(thumbnail).getPixelAt(194, 3).getARGB(), colour.getARGB());
        markers = std::make_shared<serato::Markers>();
        markers->cues.push_back({.positionMilliseconds = 1500, .colour = colour, .name = "Beyond track"});
        thumbnail.setSeratoMarkers(markers);
        thumbnail.setRange({0.0, 2.0});
        expectEquals(colourPixels(renderPreview(thumbnail), colour), 0);

        beginTest("Cues render over an Overview-coloured waveform without requiring an Overview");
        markers = std::make_shared<serato::Markers>();
        markers->cues.push_back({.positionMilliseconds = 500, .colour = colour, .name = {}});
        thumbnail.setSeratoMarkers(markers);
        thumbnail.setRange({0.0, 1.0});
        const auto overview = std::make_shared<serato::Overview>();
        std::array<std::uint8_t, serato::Overview::bandCount> slice {};
        slice.fill(30);
        overview->slices.push_back(slice);
        thumbnail.setSeratoOverview(overview);
        expect(colourPixels(renderPreview(thumbnail), colour) > 20);
        thumbnail.setSeratoOverview({});
        expect(colourPixels(renderPreview(thumbnail), colour) > 20);
        thumbnail.setRange({});
        expectEquals(colourPixels(renderPreview(thumbnail), colour), 0);
    }

    /// Ensures neither a new cached source nor a failed or cleared source retains the previous file's cues.
    void testCueReset()
    {
        beginTest("Cached source replacement, explicit reset and empty URL remove old cues");
        juce::AudioFormatManager formats;
        juce::AudioTransportSource transport;
        ThumbnailComponent thumbnail(formats, transport);
        thumbnail.setSize(260, 104);
        const auto peaks = previewPeaks(formats, 1);
        const auto colour = juce::Colour(239, 37, 37);
        const auto markers = std::make_shared<serato::Markers>();
        markers->cues.push_back({.positionMilliseconds = 500, .colour = colour, .name = {}});
        expect(thumbnail.loadFromCacheData(peaks));
        thumbnail.setSeratoMarkers(markers);
        expect(colourPixels(renderPreview(thumbnail), colour) > 20);
        expect(thumbnail.loadFromCacheData(peaks));
        expectEquals(colourPixels(renderPreview(thumbnail), colour), 0);
        thumbnail.setSeratoMarkers(markers);
        thumbnail.setSeratoMarkers({});
        expectEquals(colourPixels(renderPreview(thumbnail), colour), 0);
        thumbnail.setSeratoMarkers(markers);
        thumbnail.setURL({});
        expectEquals(colourPixels(renderPreview(thumbnail), colour), 0);
        expect(!thumbnail.loadFromCacheData({}));
    }

    /// Shows metadata only when available, preserving it during pending/failed analysis and clearing it on replacement.
    void testTrackInfo()
    {
        beginTest("Autotags tempo takes precedence, standard key stays independent and absent fields disappear");
        AudioInfoPanel panel;
        panel.setSize(320, 400);
        AudioAnalysisRecord record;
        record.status = AudioAnalysisStatus::analyzed;
        serato::SeratoData data;
        data.autotags = serato::Autotags {.bpm = 128.25};
        data.trackInfo = serato::TrackInfo {.bpm = 130.0, .key = "8A"};
        panel.showRecord(record, {}, &data);
        expectEquals(panelValue(panel, "BPM"), juce::String("128.25"));
        expectEquals(panelValue(panel, "Key"), juce::String("8A"));
        data.autotags.reset();
        panel.showRecord(record, {}, &data);
        expectEquals(panelValue(panel, "BPM"), juce::String("130.00"));
        data.trackInfo->bpm.reset();
        panel.showRecord(record, {}, &data);
        expect(panelValue(panel, "BPM").isEmpty());
        expectEquals(panelValue(panel, "Key"), juce::String("8A"));
        data.trackInfo.reset();
        data.beatGrid = serato::BeatGrid {.markers = {{.bpm = 96.5}}};
        panel.showRecord(record, {}, &data);
        expectEquals(panelValue(panel, "BPM"), juce::String("96.50"));
        expect(panelValue(panel, "Key").isEmpty());
        panel.showRecord(record, {});
        expect(panelValue(panel, "BPM").isEmpty());

        beginTest("Musical metadata survives pending or failed analysis without treating Analysis version as key");
        data.beatGrid.reset();
        data.trackInfo = serato::TrackInfo {.bpm = 92.0, .key = "F minor"};
        record.status = AudioAnalysisStatus::pending;
        panel.showRecord(record, "Analyzing", &data);
        expectEquals(panelValue(panel, "BPM"), juce::String("92.00"));
        expectEquals(panelValue(panel, "Key"), juce::String("F minor"));
        record.status = AudioAnalysisStatus::failed;
        panel.showRecord(record, {}, &data);
        expectEquals(panelValue(panel, "Key"), juce::String("F minor"));
        data.trackInfo.reset();
        data.analysis = serato::Analysis {.majorVersion = 2, .minorVersion = 5};
        panel.showRecord(record, {}, &data);
        expect(panelValue(panel, "Key").isEmpty());
        panel.clear();
        expect(panelValue(panel, "BPM").isEmpty());
        expectEquals(panel.getPreferredHeight(), AudioInfoPanel::verticalPadding * 2);
    }
};

/// Constructs the synthetic cue and track-info presentation suite.
std::unique_ptr<juce::UnitTest> createSeratoPreviewTests()
{
    return std::make_unique<SeratoPreviewTests>();
}
}  // namespace audiobatch::tests