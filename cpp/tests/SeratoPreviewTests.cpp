/// Cue arrow geometry and musical info tests using generated PCM peaks and synthetic metadata.
/// Exercises the actual preview components without devices, local music, painting or redistributed Serato payloads.

#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/components/ThumbnailComponent.h"
#include "gui/GuiLayout.h"
#include "serato/SeratoOverview.h"
#include "serato/SeratoTagTypes.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

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

/// Width of the preview in the tests, in logical pixels.
constexpr int previewWidth {260};
/// Height of the preview in the tests, in logical pixels.
constexpr int previewHeight {104};
/// Inset of the waveform from each edge of the preview, in logical pixels.
constexpr float waveformInset {2.0f};
/// Width of the waveform inside the preview, in logical pixels.
constexpr float waveformWidth {previewWidth - 2.0f * waveformInset};

/// Returns the horizontal position of a time in a preview that shows the given range.
float expectedCuePosition(const double seconds, const juce::Range<double> visibleRange)
{
    return waveformInset
        + waveformWidth * static_cast<float>((seconds - visibleRange.getStart()) / visibleRange.getLength());
}

/// Returns true when both arrow lists place the same cues in the same places.
bool sameArrows(const std::vector<CueArrows>& first, const std::vector<CueArrows>& second)
{
    return std::ranges::equal(first, second, [](const CueArrows& left, const CueArrows& right) {
        return left.colour == right.colour
            && juce::approximatelyEqual(left.horizontalPosition, right.horizontalPosition)
            && juce::approximatelyEqual(left.halfWidth, right.halfWidth)
            && juce::approximatelyEqual(left.height, right.height) && juce::approximatelyEqual(left.top, right.top)
            && juce::approximatelyEqual(left.bottom, right.bottom);
    });
}

/// Retrieves a named value regardless of the inspector's card and viewport hierarchy.
juce::String panelValue(const AudioInfoPanel& panel, const juce::String& name)
{
    std::vector<const juce::Component*> pending {&panel};
    while (!pending.empty()) {
        const auto* current = pending.back();
        pending.pop_back();
        for (const auto* child : current->getChildren()) {
            if (!child->isVisible()) {
                continue;
            }
            if (const auto* label = dynamic_cast<const juce::Label*>(child);
                label != nullptr && label->getName() == name)
            {
                return label->getText();
            }
            pending.push_back(child);
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
    SeratoPreviewTests() : juce::UnitTest("Serato preview presentation", "AudioBatch") {}

    /// Checks the arrow placement, colour, projection, clipping, resets and info-field precedence.
    void runTest() override
    {
        testCueArrows();
        testCueReset();
        testHotCueKeys();
        testTrackInfo();
    }

private:
    /// Checks the cue arrow placement at full view, zoom and pan, and that only the cues and the viewport move it.
    void testCueArrows()
    {
        beginTest("Cue arrows hang from both waveform edges, opaque and independent of gain and channels");
        juce::AudioFormatManager formats;
        AudioPreviewPlayer transport;
        ThumbnailComponent thumbnail(formats, transport);
        thumbnail.setSize(previewWidth, previewHeight);
        const auto colour = juce::Colour(239, 37, 37);
        auto markers = std::make_shared<serato::Markers>();
        markers->cues.push_back({.positionMilliseconds = 500, .colour = colour.withAlpha(0.5f), .name = "Test cue"});

        expect(thumbnail.loadFromCacheData(previewPeaks(formats, 1)));
        expect(thumbnail.getCueArrows().empty());
        thumbnail.setSeratoMarkers(markers);
        const auto arrows = thumbnail.getCueArrows();
        expectEquals(static_cast<int>(arrows.size()), 1);
        if (arrows.size() == 1) {
            const auto& cue = arrows.front();
            expect(cue.colour == colour);
            expectWithinAbsoluteError(cue.horizontalPosition, expectedCuePosition(0.5, {0.0, 1.0}), 0.001f);
            expectWithinAbsoluteError(cue.top, waveformInset, 0.001f);
            expectWithinAbsoluteError(cue.bottom, previewHeight - waveformInset, 0.001f);
            expectWithinAbsoluteError(cue.height, layout::waveform::cueArrowHeight, 0.001f);
            expectWithinAbsoluteError(cue.halfWidth, layout::waveform::cueArrowWidth / 2.0f, 0.001f);
            // The arrows stay near the edges, so they never cover the middle of the waveform.
            expect(cue.height <= (cue.bottom - cue.top) / 4.0f);
        }

        for (const int channels : {1, 2}) {
            expect(thumbnail.loadFromCacheData(previewPeaks(formats, channels)));
            thumbnail.setSeratoMarkers(markers);
            for (const float gain : {0.0f, 1.0f, 4.0f}) {
                thumbnail.setDisplayGain(gain);
                expect(sameArrows(thumbnail.getCueArrows(), arrows), utils::format("{} channels", channels));
            }
        }
        thumbnail.setDisplayGain(1.0f);

        beginTest("Cue arrows shrink on a short waveform and keep their proportions");
        thumbnail.setSize(previewWidth, 20);
        const auto shortArrows = thumbnail.getCueArrows();
        expectEquals(static_cast<int>(shortArrows.size()), 1);
        if (shortArrows.size() == 1) {
            const auto& cue = shortArrows.front();
            expectWithinAbsoluteError(cue.height, (cue.bottom - cue.top) / 4.0f, 0.001f);
            expectWithinAbsoluteError(
                cue.halfWidth * 2.0f / cue.height,
                layout::waveform::cueArrowWidth / layout::waveform::cueArrowHeight,
                0.001f
            );
        }
        thumbnail.setSize(previewWidth, previewHeight);

        beginTest("Millisecond cues stay aligned through zoom and pan, and cues outside the view have no arrows");
        for (const auto range : {juce::Range(0.25, 0.75), juce::Range(0.4, 0.9), juce::Range(-0.25, 0.75)}) {
            thumbnail.setRange(range);
            const auto zoomed = thumbnail.getCueArrows();
            expectEquals(static_cast<int>(zoomed.size()), 1);
            if (!zoomed.empty()) {
                expectWithinAbsoluteError(zoomed.front().horizontalPosition, expectedCuePosition(0.5, range), 0.001f);
            }
        }
        thumbnail.setRange({0.6, 1.1});
        expect(thumbnail.getCueArrows().empty());
        markers = std::make_shared<serato::Markers>();
        markers->cues.push_back({.positionMilliseconds = 1500, .colour = colour, .name = "Beyond track"});
        thumbnail.setSeratoMarkers(markers);
        thumbnail.setRange({0.0, 2.0});
        expect(thumbnail.getCueArrows().empty());

        beginTest("Cue arrows do not depend on the Overview colouring");
        markers = std::make_shared<serato::Markers>();
        markers->cues.push_back({.positionMilliseconds = 500, .colour = colour, .name = {}});
        thumbnail.setSeratoMarkers(markers);
        thumbnail.setRange({0.0, 1.0});
        const auto withoutOverview = thumbnail.getCueArrows();
        const auto overview = std::make_shared<serato::Overview>();
        std::array<std::uint8_t, serato::Overview::bandCount> slice {};
        slice.fill(30);
        overview->slices.push_back(slice);
        thumbnail.setSeratoOverview(overview);
        expectEquals(static_cast<int>(withoutOverview.size()), 1);
        expect(sameArrows(thumbnail.getCueArrows(), withoutOverview));
        thumbnail.setSeratoOverview({});
        thumbnail.setRange({});
        expect(thumbnail.getCueArrows().empty());
    }

    /// Ensures neither a new cached source nor a failed or cleared source retains the previous file's cues.
    void testCueReset()
    {
        beginTest("Cached source replacement, explicit reset and empty URL remove old cues");
        juce::AudioFormatManager formats;
        AudioPreviewPlayer transport;
        ThumbnailComponent thumbnail(formats, transport);
        thumbnail.setSize(previewWidth, previewHeight);
        const auto peaks = previewPeaks(formats, 1);
        const auto markers = std::make_shared<serato::Markers>();
        markers->cues.push_back({.positionMilliseconds = 500, .colour = juce::Colours::red, .name = {}});
        expect(thumbnail.loadFromCacheData(peaks));
        thumbnail.setSeratoMarkers(markers);
        expectEquals(static_cast<int>(thumbnail.getCueArrows().size()), 1);
        expect(thumbnail.loadFromCacheData(peaks));
        expect(thumbnail.getCueArrows().empty());
        thumbnail.setSeratoMarkers(markers);
        thumbnail.setSeratoMarkers({});
        expect(thumbnail.getCueArrows().empty());
        thumbnail.setSeratoMarkers(markers);
        thumbnail.setURL({});
        expect(thumbnail.getCueArrows().empty());
        expect(!thumbnail.loadFromCacheData({}));
    }

    /// Maps the number keys to Serato hot cue slots and leaves other keys to the parent component.
    void testHotCueKeys()
    {
        beginTest("Number keys jump to their Serato hot cue slot and other keys pass through");
        juce::AudioFormatManager formats;
        AudioPreviewPlayer transport;
        expect(transport.load(juce::URL(fixtureFile("silence_stereo_44100_16.wav"))));
        ThumbnailComponent thumbnail(formats, transport);
        thumbnail.setSize(260, 104);
        expect(thumbnail.loadFromCacheData(previewPeaks(formats, 1)));
        const auto markers = std::make_shared<serato::Markers>();
        markers->cues.push_back({.positionMilliseconds = 250, .index = 0, .colour = juce::Colours::red, .name = {}});
        markers->cues.push_back({.positionMilliseconds = 750, .index = 2, .colour = juce::Colours::green, .name = {}});
        thumbnail.setSeratoMarkers(markers);

        expect(thumbnail.keyPressed(juce::KeyPress('3', juce::ModifierKeys::noModifiers, '3')));
        expectWithinAbsoluteError(transport.getCurrentPosition(), 0.75, 0.001);
        expect(thumbnail.keyPressed(juce::KeyPress('1', juce::ModifierKeys::noModifiers, '1')));
        expectWithinAbsoluteError(transport.getCurrentPosition(), 0.25, 0.001);
        transport.stop();
        transport.setPosition(0.5);
        expect(thumbnail.keyPressed(juce::KeyPress('2', juce::ModifierKeys::noModifiers, '2')));
        expectWithinAbsoluteError(transport.getCurrentPosition(), 0.5, 0.001);
        expect(!thumbnail.keyPressed(juce::KeyPress('9', juce::ModifierKeys::noModifiers, '9')));
        expect(!thumbnail.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey, juce::ModifierKeys::noModifiers, ' ')));
        expect(!thumbnail.keyPressed(juce::KeyPress('1', juce::ModifierKeys::commandModifier, '1')));
        expectWithinAbsoluteError(transport.getCurrentPosition(), 0.5, 0.001);

        transport.stop();
        transport.release();
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
        data.trackInfo = makeTrackInfo(130.0, "8A");
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
        data.trackInfo = makeTrackInfo(92.0, "F minor");
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
    }
};

/// Constructs the synthetic cue and track-info presentation suite.
std::unique_ptr<juce::UnitTest> createSeratoPreviewTests()
{
    return std::make_unique<SeratoPreviewTests>();
}
}  // namespace audiobatch::tests
