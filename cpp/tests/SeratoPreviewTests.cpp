/// Cue arrow and overload mark geometry and musical info tests using generated PCM peaks and synthetic metadata.
/// Pins the pure audio details model, grouped technical lines, and original musical key presentation.
/// Exercises the actual preview components without devices, local music, painting or redistributed Serato payloads.

#include "analysis/AudioAnalysisTypes.h"
#include "analysis/OverloadHistory.h"
#include "analysis/SpectrogramData.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/components/PreviewPlotGeometry.h"
#include "gui/components/ThumbnailComponent.h"
#include "gui/GuiLayout.h"
#include "serato/SeratoOverview.h"
#include "serato/SeratoTagTypes.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
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
/// Returns the horizontal position of a time using the actual waveform plot, excluding its axis gutters.
float expectedCuePosition(
    const ThumbnailComponent& thumbnail,
    const double seconds,
    const juce::Range<double> visibleRange
)
{
    const auto plot = thumbnail.getPlotGeometry().waveform;
    return static_cast<float>(plot.getX())
        + static_cast<float>(plot.getWidth())
        * static_cast<float>((seconds - visibleRange.getStart()) / visibleRange.getLength());
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
/// Finds actual visible metric text without depending on private label storage.
bool hasVisibleMetricText(const AudioInfoMetric& metric, const juce::String& text)
{
    return std::ranges::any_of(metric.getChildren(), [&](const juce::Component* child) {
        const auto* label = dynamic_cast<const juce::Label*>(child);
        return label != nullptr && label->isVisible() && label->getText() == text;
    });
}

/// Retrieves one labelled value from the pure details model, without relying on hidden component names.
juce::String trackValue(const AudioInfoDetails& details, const juce::String& label)
{
    const auto found = std::ranges::find(details.track, label, &AudioInfoValue::label);
    return found != details.track.end() ? found->value : juce::String();
}
}  // namespace

/// Verifies presentation and resets independently of tag parsing and async-worker tests.
class SeratoPreviewTests final : public juce::UnitTest
{
public:
    /// Registers the portable GUI slice through its explicit factory.
    SeratoPreviewTests() : juce::UnitTest("Serato preview presentation", "AudioBatch") {}

    void runTest() override;

private:
    void testCueArrows();
    void testCueReset();
    void testHotCueKeys();
    void testOverloadMarks();
    void testOverloadSettings();
    void testOverloadFlagFiltering();
    void verifyOverloadProjection(ThumbnailComponent& thumbnail, int channels, bool sample, bool truePeak, bool lines);
    void testTrackInfo();
    void testInfoDetails();
    void testMetricCaptions();
    void testPreviewModes();
    void testPreviewAxes();
    void testPreviewAxesForMode(bool enabled, PreviewViewMode mode);
    void testAmplitudeLabels();
    void verifyAmplitudeLabel(const PreviewAmplitudeLabel& label, const PreviewPlotGeometry& geometry, int channels);
    void testCompactAmplitudeLabels();
    void testSpectrogramDisplayIntensity();
    void testSpectrogramReset();
    void testPaneScrubbing();
};

/// Checks the arrow placement, colour, projection, clipping, resets and info-field precedence.
void SeratoPreviewTests::runTest()
{
    testCueArrows();
    testCueReset();
    testOverloadMarks();
    testOverloadSettings();
    testHotCueKeys();
    testTrackInfo();
    testInfoDetails();
    testMetricCaptions();
    testPreviewModes();
    testPreviewAxes();
    testAmplitudeLabels();
    testCompactAmplitudeLabels();
    testSpectrogramDisplayIntensity();
    testSpectrogramReset();
    testPaneScrubbing();
}

/// Checks the cue arrow placement at full view, zoom and pan, and that only the cues and the viewport move it.
void SeratoPreviewTests::testCueArrows()
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
        expectWithinAbsoluteError(cue.horizontalPosition, expectedCuePosition(thumbnail, 0.5, {0.0, 1.0}), 0.001f);
        const auto plot = thumbnail.getPlotGeometry().waveform;
        expectWithinAbsoluteError(cue.top, static_cast<float>(plot.getY()), 0.001f);
        expectWithinAbsoluteError(cue.bottom, static_cast<float>(plot.getBottom()), 0.001f);
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
            expectWithinAbsoluteError(
                zoomed.front().horizontalPosition, expectedCuePosition(thumbnail, 0.5, range), 0.001f
            );
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
void SeratoPreviewTests::testCueReset()
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
void SeratoPreviewTests::testHotCueKeys()
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

/// Checks overload mark lanes, kinds, minimum width, gain independence, and reset with the waveform.
void SeratoPreviewTests::testOverloadMarks()
{
    beginTest("Overload marks follow the channel lanes and their kind, independent of gain");
    juce::AudioFormatManager formats;
    AudioPreviewPlayer transport;
    ThumbnailComponent thumbnail(formats, transport);
    thumbnail.setSize(previewWidth, previewHeight);
    // One second at 8192 Hz in ten blocks of 819 frames and a final partial one, like the generated peaks.
    constexpr double sampleRate {8192.0};
    const auto blockSize = overloadBlockSize(sampleRate);
    OverloadHistoryBuilder builder(2, sampleRate, 8192, blockSize);
    for (int block = 0; block < 11; ++block) {
        std::array<std::uint8_t, 2> flags {};
        if (block == 2) {
            flags[0] = OverloadHistory::sampleOver | OverloadHistory::truePeakOver;
        }
        if (block == 6) {
            flags[1] = OverloadHistory::truePeakOver;
        }
        builder.addBlock(flags);
    }
    const auto history = builder.finish();

    expect(thumbnail.loadFromCacheData(previewPeaks(formats, 2)));
    expect(thumbnail.getOverloadMarks().empty());
    thumbnail.setOverloadHistory(history);
    const auto marks = thumbnail.getOverloadMarks();
    const auto plot = thumbnail.getPlotGeometry().waveform;
    const auto laneTop = static_cast<float>(plot.getY());
    const auto laneHeight = std::floor(static_cast<float>(plot.getHeight()) * 0.5f);
    const auto inLane
        = [](const OverloadMark& mark, const float top) { return juce::approximatelyEqual(mark.laneTop, top); };
    const auto solid
        = std::ranges::find_if(marks, [&](const auto& mark) { return mark.sampleOver && inLane(mark, laneTop); });
    expect(solid != marks.end());
    if (solid != marks.end()) {
        expectWithinAbsoluteError(solid->laneBottom, laneTop + laneHeight, 0.001f);
        expect(solid->topEdge && !solid->bottomEdge);
        expect(solid->showLine);
        const auto blockSeconds = static_cast<double>(blockSize) / sampleRate;
        expectWithinAbsoluteError(
            solid->horizontal.getStart(), expectedCuePosition(thumbnail, 2 * blockSeconds, {0.0, 1.0}), 1.0f
        );
    }
    expect(std::ranges::none_of(marks, [&](const auto& mark) {
        return mark.sampleOver && inLane(mark, laneTop + laneHeight);
    }));
    expect(std::ranges::any_of(marks, [&](const auto& mark) {
        return !mark.sampleOver && inLane(mark, laneTop + laneHeight) && !mark.topEdge && mark.bottomEdge
            && mark.showLine;
    }));
    // The true peak coverage of the filter delay reaches at most a column before the solid run.
    expect(std::ranges::all_of(marks, [&](const auto& mark) {
        return mark.sampleOver || !inLane(mark, laneTop) || solid == marks.end()
            || mark.horizontal.getEnd() <= solid->horizontal.getEnd();
    }));
    thumbnail.setDisplayGain(4.0f);
    expectEquals(thumbnail.getOverloadMarks().size(), marks.size());

    beginTest("A short overload keeps the minimum mark width when zoomed out");
    {
        OverloadHistoryBuilder shortBuilder(2, sampleRate, 8192, blockSize);
        for (int block = 0; block < 11; ++block) {
            shortBuilder.addBlock(
                std::array<std::uint8_t, 2> {
                    static_cast<std::uint8_t>(block == 10 ? OverloadHistory::sampleOver : 0),
                    0,
                }
            );
        }
        thumbnail.setOverloadHistory(shortBuilder.finish());
        const auto shortMarks = thumbnail.getOverloadMarks();
        expectEquals(static_cast<int>(shortMarks.size()), 1);
        if (!shortMarks.empty()) {
            expect(shortMarks.front().horizontal.getLength() >= layout::waveform::overloadMinimumWidth);
        }
    }

    beginTest("An all-clear history, a mismatched channel count, and a new waveform show no marks");
    {
        OverloadHistoryBuilder clearBuilder(2, sampleRate, 8192, blockSize);
        for (int block = 0; block < 11; ++block) {
            clearBuilder.addBlock(std::array<std::uint8_t, 2> {});
        }
        thumbnail.setOverloadHistory(clearBuilder.finish());
        expect(thumbnail.getOverloadMarks().empty());
        expect(thumbnail.getTooltip().isEmpty());
        thumbnail.setOverloadHistory(history);
        expect(thumbnail.getTooltip().isNotEmpty());
        thumbnail.setViewMode(PreviewViewMode::spectrogram);
        expect(thumbnail.getTooltip().isEmpty());
        thumbnail.setViewMode(PreviewViewMode::waveform);
        expect(thumbnail.getTooltip().isNotEmpty());
        expect(thumbnail.loadFromCacheData(previewPeaks(formats, 1)));
        expect(thumbnail.getOverloadMarks().empty());
        thumbnail.setOverloadHistory(history);
        expect(thumbnail.getOverloadMarks().empty());
    }
}

/// Checks each raw flag combination independently of projection and paint order.
void SeratoPreviewTests::testOverloadFlagFiltering()
{
    beginTest("Raw overload kinds are filtered independently, including columns with both bits");
    for (const bool sample : {false, true}) {
        for (const bool truePeak : {false, true}) {
            for (const auto flags :
                 {
                     std::uint8_t {0},
                     OverloadHistory::sampleOver,
                     OverloadHistory::truePeakOver,
                     static_cast<std::uint8_t>(OverloadHistory::sampleOver | OverloadHistory::truePeakOver),
                 })
            {
                const auto filtered = visibleOverloadFlags(flags, sample, truePeak);
                expect(
                    ((filtered & OverloadHistory::sampleOver) != 0)
                    == (sample && (flags & OverloadHistory::sampleOver) != 0)
                );
                expect(
                    ((filtered & OverloadHistory::truePeakOver) != 0)
                    == (truePeak && (flags & OverloadHistory::truePeakOver) != 0)
                );
            }
        }
    }
}

/// Verifies one display-policy combination against native integer channel geometry.
void SeratoPreviewTests::verifyOverloadProjection(
    ThumbnailComponent& thumbnail,
    const int channels,
    const bool sample,
    const bool truePeak,
    const bool lines
)
{
    thumbnail.setOverloadIndicatorSettings(sample, truePeak, lines);
    const auto marks = thumbnail.getOverloadMarks();
    const auto plot = thumbnail.getPlotGeometry().waveform;
    expectEquals(static_cast<int>(marks.size()), (static_cast<int>(sample) + static_cast<int>(truePeak)) * channels);
    bool reachedTruePeak = false;
    for (std::size_t index = 0; index < marks.size(); ++index) {
        const auto channel = index % static_cast<std::size_t>(channels);
        const auto& mark = marks[index];
        if (!mark.sampleOver) {
            reachedTruePeak = true;
        }
        expect(!mark.sampleOver || !reachedTruePeak);
        expect(mark.sampleOver == (sample && std::cmp_less(index, channels)));
        if (sample && truePeak && !mark.sampleOver) {
            const auto& sampleMark = marks[channel];
            expect(mark.horizontal == sampleMark.horizontal);
            expectWithinAbsoluteError(mark.laneTop, sampleMark.laneTop, 0.001f);
            expectWithinAbsoluteError(mark.laneBottom, sampleMark.laneBottom, 0.001f);
        }
        expect(mark.showLine == lines);
        expect(mark.topEdge == (channel == 0UZ));
        expect(mark.bottomEdge == std::cmp_equal(channel, channels - 1));
        const int laneTop = plot.getY() + static_cast<int>(channel) * plot.getHeight() / channels;
        const int laneBottom = plot.getY() + (static_cast<int>(channel) + 1) * plot.getHeight() / channels;
        expectWithinAbsoluteError(mark.laneTop, static_cast<float>(laneTop), 0.001f);
        expectWithinAbsoluteError(mark.laneBottom, static_cast<float>(laneBottom), 0.001f);
    }
}

/// Pins raw-kind filtering, outer-edge caps and kind-independent line policy without drawing pixels.
void SeratoPreviewTests::testOverloadSettings()
{
    testOverloadFlagFiltering();
    beginTest("Mono caps touch both outer edges, stereo caps never touch the channel divider");
    juce::AudioFormatManager formats;
    AudioPreviewPlayer transport;
    ThumbnailComponent thumbnail(formats, transport);
    thumbnail.setSize(previewWidth, previewHeight + 1);
    for (const int channels : {1, 2}) {
        expect(thumbnail.loadFromCacheData(previewPeaks(formats, channels)));
        const auto history = std::make_shared<OverloadHistory>();
        history->channelCount = channels;
        history->sampleRate = 8192.0;
        history->lengthInSamples = 8192;
        history->samplesPerBucket = overloadBlockSize(history->sampleRate);
        history->flags.assign(
            static_cast<std::size_t>(channels) * 11, OverloadHistory::sampleOver | OverloadHistory::truePeakOver
        );
        expect(isValidOverloadHistory(*history));
        thumbnail.setOverloadHistory(history);
        const auto originalFlags = history->flags;
        for (const bool sample : {false, true}) {
            for (const bool truePeak : {false, true}) {
                for (const bool lines : {false, true}) {
                    verifyOverloadProjection(thumbnail, channels, sample, truePeak, lines);
                    expect(history->flags == originalFlags);
                }
            }
        }
        thumbnail.setOverloadIndicatorSettings(false, true, false);
        thumbnail.setDisplayGain(4.0f);
        thumbnail.setRange({0.25, 0.75});
        const auto trueOnly = thumbnail.getOverloadMarks();
        expectEquals(static_cast<int>(trueOnly.size()), channels);
        expect(std::ranges::all_of(trueOnly, [](const auto& mark) { return !mark.sampleOver && !mark.showLine; }));
        thumbnail.setRange({0.0, 1.0});
        thumbnail.setDisplayGain(1.0f);
    }

    beginTest("True peaks paint last when separate columns overlap after minimum-width expansion");
    expect(thumbnail.loadFromCacheData(previewPeaks(formats, 1)));
    thumbnail.setSize(layout::waveform::AXIS_GUTTER_WIDTH + layout::waveform::contentInset * 2 + 11, previewHeight);
    const auto adjacent = std::make_shared<OverloadHistory>();
    adjacent->channelCount = 1;
    adjacent->sampleRate = 8192.0;
    adjacent->lengthInSamples = 8192;
    adjacent->samplesPerBucket = overloadBlockSize(adjacent->sampleRate);
    adjacent->flags.assign(11, 0);
    adjacent->flags[2] = OverloadHistory::truePeakOver;
    adjacent->flags[3] = OverloadHistory::sampleOver;
    thumbnail.setOverloadHistory(adjacent);
    thumbnail.setOverloadIndicatorSettings(true, true, false);
    const auto adjacentMarks = thumbnail.getOverloadMarks();
    expectEquals(static_cast<int>(adjacentMarks.size()), 2);
    if (adjacentMarks.size() == 2) {
        expect(adjacentMarks[0].sampleOver);
        expect(!adjacentMarks[1].sampleOver);
        expect(adjacentMarks[0].horizontal.intersects(adjacentMarks[1].horizontal));
        expectWithinAbsoluteError(
            adjacentMarks[0].horizontal.getLength(), layout::waveform::overloadMinimumWidth, 0.001f
        );
        expectWithinAbsoluteError(
            adjacentMarks[1].horizontal.getLength(), layout::waveform::overloadMinimumWidth, 0.001f
        );
    }
    thumbnail.setSize(previewWidth, previewHeight);

    beginTest("Overload display preferences survive source replacement without retaining source history");
    thumbnail.setOverloadIndicatorSettings(false, false, false);
    expect(thumbnail.loadFromCacheData(previewPeaks(formats, 1)));
    const auto history = std::make_shared<OverloadHistory>();
    history->channelCount = 1;
    history->sampleRate = 8192.0;
    history->lengthInSamples = 8192;
    history->samplesPerBucket = overloadBlockSize(history->sampleRate);
    history->flags.assign(11, OverloadHistory::sampleOver);
    expect(isValidOverloadHistory(*history));
    thumbnail.setOverloadHistory(history);
    expect(thumbnail.getOverloadMarks().empty());
    expect(thumbnail.getTooltip().isEmpty());
    thumbnail.setOverloadIndicatorSettings(true, true, true);
    expectEquals(static_cast<int>(thumbnail.getOverloadMarks().size()), 1);
    expect(thumbnail.getTooltip().contains("solid yellow"));
    expect(thumbnail.getTooltip().contains("solid orange"));
    thumbnail.setViewMode(PreviewViewMode::spectrogram);
    expect(thumbnail.getTooltip().isEmpty());
}

/// Shows metadata only when available, preserving it during pending/failed analysis and clearing it on replacement.
void SeratoPreviewTests::testTrackInfo()
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
    expectEquals(trackValue(formatInfoDetails(record, &data), "BPM"), juce::String("128.25 BPM"));
    expectEquals(trackValue(formatInfoDetails(record, &data), "Key"), juce::String("8A"));
    expectEquals(panelValue(panel, "BPM"), juce::String("128.25 BPM"));
    expectEquals(panelValue(panel, "Key"), juce::String("8A"));
    data.autotags.reset();
    panel.showRecord(record, {}, &data);
    expectEquals(trackValue(formatInfoDetails(record, &data), "BPM"), juce::String("130.00 BPM"));
    expectEquals(panelValue(panel, "BPM"), juce::String("130.00 BPM"));
    expectEquals(panelValue(panel, "Key"), juce::String("8A"));
    data.trackInfo->bpm.reset();
    panel.showRecord(record, {}, &data);
    expect(trackValue(formatInfoDetails(record, &data), "BPM").isEmpty());
    expectEquals(panelValue(panel, "Key"), juce::String("8A"));
    data.beatGrid = serato::BeatGrid {.markers = {{.bpm = 96.5}}};
    data.trackInfo->bpm = 0.0;
    expectEquals(trackValue(formatInfoDetails(record, &data), "BPM"), juce::String("96.50 BPM"));
    data.trackInfo->bpm.reset();
    expectEquals(trackValue(formatInfoDetails(record, &data), "BPM"), juce::String("96.50 BPM"));
    data.autotags = serato::Autotags {.bpm = -1.0};
    data.trackInfo->bpm = 130.0;
    expectEquals(trackValue(formatInfoDetails(record, &data), "BPM"), juce::String("130.00 BPM"));
    data.autotags.reset();
    data.trackInfo.reset();
    panel.showRecord(record, {}, &data);
    expectEquals(trackValue(formatInfoDetails(record, &data), "BPM"), juce::String("96.50 BPM"));
    expect(trackValue(formatInfoDetails(record, &data), "Key").isEmpty());
    expectEquals(panelValue(panel, "BPM"), juce::String("96.50 BPM"));
    panel.showRecord(record, {});
    expect(panelValue(panel, "BPM").isEmpty());
    expect(panelValue(panel, "Key").isEmpty());

    beginTest("Gain-adjusted peaks remain immediate, loudness waits for sample-based measurement");
    record.overallPeak = static_cast<float>(std::pow(10.0, 0.9 / 20.0));
    record.overallTruePeak = std::pow(10.0, 1.5 / 20.0);
    record.maxShortTermLufs = -8.5;
    record.integratedLufs = -12.5;
    record.customGain = Decibels {-1.0f};
    for (const auto compact : {false, true}) {
        panel.setCompact(compact);
        panel.showRecord(record, {});
        expectEquals(panelValue(panel, "Peak"), juce::String("0.90"));
        expectEquals(panelValue(panel, "Peak after custom gain"), juce::String("(-0.1)"));
        expectEquals(panelValue(panel, "True peak after custom gain"), juce::String("(+0.5)"));
        expect(panelValue(panel, "Max short-term after custom gain").isEmpty());
        expect(panelValue(panel, "Integrated loudness after custom gain").isEmpty());
        expectEquals(panelValue(panel, "Integrated loudness"), juce::String("-12.50"));
        panel.showRecord(record, {}, nullptr, false);
        expect(panelValue(panel, "Peak after custom gain").isEmpty());
        expect(panelValue(panel, "Integrated loudness after custom gain").isEmpty());
    }
    panel.setCompact(false);
    record.customGain.reset();
    panel.showRecord(record, {});
    expect(panelValue(panel, "Peak after custom gain").isEmpty());
    expect(panelValue(panel, "Integrated loudness after custom gain").isEmpty());
    record.customGain = Decibels {3.0f};
    record.overallPeak = 0.0f;
    record.integratedLufs = AudioAnalysisRecord::negativeInfinityLoudness;
    panel.showRecord(record, {});
    expectEquals(panelValue(panel, "Peak after custom gain"), juce::String("(-INF)"));
    expect(panelValue(panel, "Integrated loudness after custom gain").isEmpty());
    record.overallPeak = static_cast<float>(std::pow(10.0, -3.01 / 20.0));
    panel.showRecord(record, {});
    expectEquals(panelValue(panel, "Peak after custom gain"), juce::String("(+0.0)"));
    record.status = AudioAnalysisStatus::pending;
    panel.showRecord(record, {});
    expect(panelValue(panel, "Peak after custom gain").isEmpty());
    panel.clear();
    expect(panelValue(panel, "Peak after custom gain").isEmpty());

    beginTest("Musical metadata survives pending or failed analysis without treating Analysis version as key");
    data.beatGrid.reset();
    data.trackInfo = makeTrackInfo(92.0, "F minor");
    record.status = AudioAnalysisStatus::pending;
    panel.showRecord(record, "Analyzing", &data);
    expectEquals(trackValue(formatInfoDetails(record, &data), "BPM"), juce::String("92.00 BPM"));
    expectEquals(trackValue(formatInfoDetails(record, &data), "Key"), juce::String("F minor / 4A"));
    expectEquals(panelValue(panel, "BPM"), juce::String("92.00 BPM"));
    expectEquals(panelValue(panel, "Key"), juce::String("F minor / 4A"));
    record.status = AudioAnalysisStatus::failed;
    panel.showRecord(record, {}, &data);
    expectEquals(panelValue(panel, "BPM"), juce::String("92.00 BPM"));
    expectEquals(panelValue(panel, "Key"), juce::String("F minor / 4A"));
    data.trackInfo.reset();
    data.analysis = serato::Analysis {.majorVersion = 2, .minorVersion = 5};
    panel.showRecord(record, {}, &data);
    expect(trackValue(formatInfoDetails(record, &data), "Key").isEmpty());
    expect(panelValue(panel, "BPM").isEmpty());
    expect(panelValue(panel, "Key").isEmpty());
    panel.clear();
    expect(panelValue(panel, "BPM").isEmpty());
    expect(panelValue(panel, "Key").isEmpty());
}

/// Checks width-dependent captions without altering values or their accessible metric names.
void SeratoPreviewTests::testMetricCaptions()
{
    beginTest("Metric captions switch to distinct unit-only labels when measured names cannot fit");
    constexpr std::array CAPTIONS {"Peak", "True peak", "Short-term", "Integrated"};
    constexpr std::array UNITS {"dBFS", "dBTP", "LUFS", "LUFS"};
    constexpr std::array COMPACT_UNITS {"dBFS", "dBTP", "LUFS-S", "LUFS-I"};
    for (std::size_t index = 0; index < CAPTIONS.size(); ++index) {
        AudioInfoMetric metric(CAPTIONS[index], CAPTIONS[index], UNITS[index], COMPACT_UNITS[index]);
        metric.setValue("-12.50");
        for (const auto compact : {false, true}) {
            metric.setCompact(compact);
            for (const auto width : {30, 400, 30}) {
                metric.setSize(width, layout::information::metricHeight);
                expect(hasVisibleMetricText(metric, "-12.50"));
                expectEquals(metric.getName(), juce::String(CAPTIONS[index]));
                expect(hasVisibleMetricText(metric, CAPTIONS[index]) == (width == 400));
                if (width == 30) {
                    expect(hasVisibleMetricText(metric, COMPACT_UNITS[index]));
                }
            }
        }
    }
}

/// Pins the pure details contract, units, omissions, original key spelling, and readiness-independent tags.
void SeratoPreviewTests::testInfoDetails()
{
    beginTest("Details group known technical values without samples or placeholder separators");
    AudioAnalysisRecord record;
    record.file = juce::File("/test/music.flac");
    record.fileName = "music.flac";
    record.status = AudioAnalysisStatus::analyzed;
    record.channels = 2;
    record.bitsPerSample = 24;
    record.sampleRate = 44'100;
    record.durationSeconds = 205.4;
    record.fileSize = 36'200'000;
    record.lengthInSamples = 9'058'140;
    const auto details = formatInfoDetails(record);
    expectEquals(static_cast<int>(details.format.size()), 4);
    expectEquals(static_cast<int>(details.audio.size()), 3);
    expect(details.track.empty());
    AudioInfoPanel panel;
    panel.setSize(320, 400);
    panel.showRecord(record, {});
    expectEquals(panelValue(panel, "Format"), juce::String("FLAC"));
    expectEquals(panelValue(panel, "Channels"), juce::String("Stereo"));
    expectEquals(panelValue(panel, "Duration"), juce::String("3:25.4"));
    expectEquals(panelValue(panel, "Size"), juce::String("36.2 MB"));
    expectEquals(panelValue(panel, "Bit depth"), juce::String("24-bit"));
    expectEquals(panelValue(panel, "Sample rate"), juce::String("44.1 kHz"));
    expectEquals(panelValue(panel, "Bitrate"), juce::String("1410 kbps"));
    const auto grid = formatInfoGrid(details);
    constexpr std::array FIELDS {
        "Bit depth",
        "Sample rate",
        "Bitrate",
        "Duration",
        "Size",
        "Format",
        "Channels",
        "BPM",
        "Key",
        "Year",
    };
    for (std::size_t index = 0; index < grid.size(); ++index) {
        expectEquals(grid[index].label, juce::String(FIELDS[index]));
    }
    expect(grid[7].value.isEmpty() && grid[8].value.isEmpty() && grid[9].value.isEmpty());
    expect(panelValue(panel, "Samples").isEmpty());
    if (details.format.size() == 4) {
        expectEquals(details.format[0].label, juce::String("Format"));
        expectEquals(details.format[1].label, juce::String("Channels"));
        expectEquals(details.format[2].label, juce::String("Duration"));
        expectEquals(details.format[3].label, juce::String("Size"));
    }
    for (const auto& [channels, expected] : {std::pair {1, "Mono"}, std::pair {2, "Stereo"}, std::pair {6, "6 ch"}}) {
        record.channels = channels;
        const auto channelDetails = formatInfoDetails(record);
        expectEquals(static_cast<int>(channelDetails.format.size()), 4);
        if (channelDetails.format.size() == 4) {
            expectEquals(channelDetails.format[1].value, juce::String(expected));
        }
    }
    record.bitsPerSample = 0;
    record.sampleRate = 0;
    record.durationSeconds = 0.0;
    record.fileSize = 0;
    record.channels = 0;
    panel.showRecord(record, {});
    expectEquals(panelValue(panel, "Format"), juce::String("FLAC"));
    expect(formatInfoDetails(record).audio.empty());
    expect(panelValue(panel, "Audio").isEmpty());
    auto unknownRecord = record;
    unknownRecord.file = juce::File();
    expect(formatInfoDetails(unknownRecord).format.empty());

    beginTest("Tempo uses the first valid analyzed, standard, or beat-grid value");
    serato::SeratoData tempoData;
    tempoData.trackInfo = makeTrackInfo(128.0, {});
    tempoData.beatGrid = serato::BeatGrid {.markers = {{.bpm = 125.0}}};
    for (const auto tempo :
         {0.0, -2.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
    {
        tempoData.trackInfo->bpm = tempo;
        expectEquals(trackValue(formatInfoDetails(record, &tempoData), "BPM"), juce::String("125.00 BPM"));
    }

    beginTest("Key display trims only the original spelling and never duplicates Camelot inputs");
    serato::SeratoData data;
    data.trackInfo = makeTrackInfo(128.0, "Gm");
    data.trackInfo->year = "2019";
    data.trackInfo->genre = "Deep House";
    data.trackInfo->album = "Some Album";
    data.trackInfo->comment = "First line\nSecond line";
    for (const auto& [original, expected] :
         {
             std::pair {"  Gm  ", "Gm / 6A"},
             std::pair {"F#m", "F#m / 11A"},
             std::pair {"Abmin", "Abmin / 1A"},
             std::pair {"8A", "8A"},
             std::pair {"08A", "08A"},
             std::pair {"08a", "08a"},
             std::pair {"012A", "012A"},
             std::pair {"8 a", "8 a"},
             std::pair {"1d", "1d / 8B"},
             std::pair {"  Hm  ", "Hm"},
             std::pair {"Unknown", "Unknown"},
         })
    {
        data.trackInfo->key = original;
        expectEquals(trackValue(formatInfoDetails(record, &data), "Key"), juce::String(expected));
    }
    data.trackInfo->key = "Gm";
    for (const auto status : {AudioAnalysisStatus::pending, AudioAnalysisStatus::failed}) {
        record.status = status;
        const auto taggedDetails = formatInfoDetails(record, &data);
        expect(taggedDetails.format.empty());
        expect(taggedDetails.audio.empty());
        expectEquals(taggedDetails.genre, juce::String("Deep House"));
        expectEquals(taggedDetails.album, juce::String("Some Album"));
        expectEquals(taggedDetails.comment, juce::String("First line\nSecond line"));
        panel.showRecord(record, {}, &data);
        expectEquals(panelValue(panel, "BPM"), juce::String("128.00 BPM"));
        expectEquals(panelValue(panel, "Key"), juce::String("Gm / 6A"));
        expectEquals(panelValue(panel, "Year"), juce::String("2019"));
        expectEquals(panelValue(panel, "Genre"), taggedDetails.genre);
        expectEquals(panelValue(panel, "Album"), taggedDetails.album);
        expectEquals(panelValue(panel, "Comment"), taggedDetails.comment);
        expect(panelValue(panel, "Format").isEmpty());
        expect(panelValue(panel, "Audio").isEmpty());
    }
    panel.showRecord(record, {});
    expect(panelValue(panel, "BPM").isEmpty());
    expect(panelValue(panel, "Key").isEmpty());
    expect(panelValue(panel, "Album").isEmpty());
    expect(panelValue(panel, "Comment").isEmpty());
    panel.clear();
    expectEquals(panelValue(panel, "File"), juce::String("No file selected"));
    expect(panelValue(panel, "BPM").isEmpty());
    expect(panelValue(panel, "Key").isEmpty());
}

/// Checks pane semantics and a shared time projection without screenshot or raster assertions.
void SeratoPreviewTests::testPreviewModes()
{
    beginTest("Four preview modes preserve one viewport and choose exactly one loudness host");
    juce::AudioFormatManager formats;
    AudioPreviewPlayer transport;
    ThumbnailComponent thumbnail(formats, transport);
    thumbnail.setSize(600, 240);
    expect(thumbnail.loadFromCacheData(previewPeaks(formats, 2)));
    const auto markers = std::make_shared<serato::Markers>();
    markers->cues.push_back({.positionMilliseconds = 500, .colour = juce::Colours::red, .name = {}});
    thumbnail.setSeratoMarkers(markers);
    thumbnail.setRange({0.25, 0.75});
    int ownerRequests = 0;
    thumbnail.setViewModeChangedCallback([&](PreviewViewMode) { ++ownerRequests; });
    for (const auto mode :
         {
             PreviewViewMode::waveform,
             PreviewViewMode::spectrogram,
             PreviewViewMode::sideBySide,
             PreviewViewMode::stacked,
         })
    {
        thumbnail.setViewMode(mode);
        expect(thumbnail.getViewMode() == mode);
        thumbnail.setLoudnessGraphSettings(true, -24.0, -4.0);
        const auto geometry = thumbnail.getPlotGeometry();
        expect(geometry.loudness == (mode == PreviewViewMode::spectrogram ? geometry.spectrogram : geometry.waveform));
        expect(!geometry.loudnessAxis.intersects(geometry.frequencyAxis));
        expect(geometry.waveform.isEmpty() == (mode == PreviewViewMode::spectrogram));
        expect(geometry.spectrogram.isEmpty() == (mode == PreviewViewMode::waveform));
        if (mode == PreviewViewMode::stacked) {
            expectEquals(geometry.waveform.getX(), geometry.spectrogram.getX());
            expectEquals(geometry.waveform.getRight(), geometry.spectrogram.getRight());
            expect(geometry.waveform.getBottom() < geometry.spectrogram.getY());
        }
        if (mode == PreviewViewMode::sideBySide) {
            expect(geometry.waveform.getRight() < geometry.spectrogram.getX());
        }
        const auto arrows = thumbnail.getCueArrows();
        expectEquals(
            static_cast<int>(arrows.size()),
            mode == PreviewViewMode::waveform || mode == PreviewViewMode::spectrogram ? 1 : 2
        );
        for (const auto plot : {geometry.waveform, geometry.spectrogram}) {
            if (plot.isEmpty()) {
                continue;
            }
            const auto horizontal = previewTimeToX(0.5, {0.25, 0.75}, plot);
            expectWithinAbsoluteError(horizontal, static_cast<float>(plot.getCentreX()), 0.5f);
            expectWithinAbsoluteError(previewXToTime(horizontal, {0.25, 0.75}, plot), 0.5, 0.001);
            expectWithinAbsoluteError(
                previewXToTime(static_cast<float>(plot.getRight() + 100), {0.25, 0.75}, plot), 0.75, 0.001
            );
            expect(std::ranges::any_of(arrows, [&](const auto& arrow) {
                return juce::approximatelyEqual(arrow.horizontalPosition, horizontal);
            }));
        }
        thumbnail.setSize(1, 1);
        const auto tiny = thumbnail.getPlotGeometry();
        expect(tiny.waveform.isEmpty() && tiny.spectrogram.isEmpty());
        expect(thumbnail.getViewMode() == mode);
        thumbnail.setSize(600, 240);
    }
    expectEquals(ownerRequests, 0);
}

/// Pins separate right LUFS and left source axes, equal gutters, and non-overlapping channel dBFS labels.
void SeratoPreviewTests::testPreviewAxes()
{
    beginTest("Frequency captions use fixed references through 20 kHz");
    expectEquals(static_cast<int>(PREVIEW_FREQUENCIES.size()), 10);
    expectWithinAbsoluteError(PREVIEW_FREQUENCIES.front(), 0.0, 0.001);
    expectWithinAbsoluteError(PREVIEW_FREQUENCIES.back(), 20'000.0, 0.001);
    beginTest("Equal source-axis gutters align both panes and separate LUFS from amplitude or frequency");
    for (const auto enabled : {false, true}) {
        for (const auto mode :
             {
                 PreviewViewMode::waveform,
                 PreviewViewMode::spectrogram,
                 PreviewViewMode::sideBySide,
                 PreviewViewMode::stacked,
             })
        {
            testPreviewAxesForMode(enabled, mode);
        }
        const auto waveform = previewPlotGeometry({0, 0, 600, 240}, PreviewViewMode::waveform, enabled);
        const auto spectrum = previewPlotGeometry({0, 0, 600, 240}, PreviewViewMode::spectrogram, enabled);
        expectEquals(waveform.waveform.getX(), spectrum.spectrogram.getX());
        expectEquals(waveform.waveform.getRight(), spectrum.spectrogram.getRight());
        expectEquals(spectrum.sourceWarning.getX(), spectrum.spectrogram.getX());
        expectEquals(spectrum.sourceWarning.getRight(), spectrum.spectrogram.getRight());
    }
}

/// Checks axis ownership and non-overlap for one visualization mode.
void SeratoPreviewTests::testPreviewAxesForMode(const bool enabled, const PreviewViewMode mode)
{
    const auto geometry = previewPlotGeometry({0, 0, 600, 240}, mode, enabled);
    for (const auto& pair :
         {
             std::pair {geometry.waveform, geometry.amplitudeAxis},
             std::pair {geometry.spectrogram, geometry.frequencyAxis},
         })
    {
        const auto plot = pair.first;
        const auto axis = pair.second;
        if (plot.isEmpty()) {
            expect(axis.isEmpty());
            continue;
        }
        expect(!plot.intersects(axis));
        expectEquals(axis.getWidth(), layout::waveform::AXIS_GUTTER_WIDTH - layout::waveform::AXIS_LABEL_GAP);
        expect(axis.getRight() <= plot.getX());
    }
    if (enabled) {
        expect(geometry.loudnessAxis.getX() >= geometry.loudness.getRight());
        expect(!geometry.loudnessAxis.intersects(geometry.amplitudeAxis));
        expect(!geometry.loudnessAxis.intersects(geometry.frequencyAxis));
    } else {
        expect(geometry.loudnessAxis.isEmpty());
    }
    if (mode == PreviewViewMode::stacked) {
        expectEquals(geometry.waveform.getX(), geometry.spectrogram.getX());
        expectEquals(geometry.waveform.getRight(), geometry.spectrogram.getRight());
    }
    if (mode == PreviewViewMode::sideBySide) {
        expectEquals(geometry.waveform.getWidth(), geometry.spectrogram.getWidth());
    }
}

/// Checks analytical dBFS conversion and signed linear positions independently for each source channel.
void SeratoPreviewTests::testAmplitudeLabels()
{
    beginTest("Both waveform polarities show only -1 and -6 dBFS references");
    for (const auto channels : {1, 2}) {
        const auto geometry = previewPlotGeometry({0, 0, 600, 2000}, PreviewViewMode::waveform, true);
        const auto labels = previewAmplitudeLabels(geometry, channels);
        expectEquals(static_cast<int>(labels.size()), channels * 4);
        for (const auto& label : labels) {
            verifyAmplitudeLabel(label, geometry, channels);
        }
        for (int channel = 0; channel < channels; ++channel) {
            for (const auto decibels : {-1.0f, -6.0f}) {
                for (const auto polarity : {1.0f, -1.0f}) {
                    const auto amplitude
                        = polarity * static_cast<float>(std::pow(10.0, static_cast<double>(decibels) / 20.0));
                    expect(std::ranges::any_of(labels, [&](const auto& label) {
                        return label.channel == channel && juce::approximatelyEqual(label.amplitude, amplitude)
                            && juce::approximatelyEqual(label.decibels, decibels);
                    }));
                }
            }
        }
    }
}

/// Verifies one caption's allowed dBFS magnitude, signed linear position and bounded placement.
void SeratoPreviewTests::verifyAmplitudeLabel(
    const PreviewAmplitudeLabel& label,
    const PreviewPlotGeometry& geometry,
    const int channels
)
{
    const auto top = geometry.waveform.getY() + label.channel * geometry.waveform.getHeight() / channels;
    const auto bottom = geometry.waveform.getY() + (label.channel + 1) * geometry.waveform.getHeight() / channels;
    const auto vertical = static_cast<float>(top) + (1.0f - label.amplitude) * static_cast<float>(bottom - top) * 0.5f;
    // Grid lines reuse this unclamped position, so it must follow the amplitude even where the caption is clamped.
    expectWithinAbsoluteError(label.vertical, vertical, 0.0001f);
    const auto magnitude = std::pow(10.0, static_cast<double>(label.decibels) / 20.0);
    expectWithinAbsoluteError(static_cast<double>(std::abs(label.amplitude)), magnitude, 0.000001);
    expect(juce::approximatelyEqual(label.decibels, -1.0f) || juce::approximatelyEqual(label.decibels, -6.0f));
    expect(geometry.amplitudeAxis.contains(label.bounds));
}

/// Checks compact amplitude captions remain bounded and never overlap neighboring labels.
void SeratoPreviewTests::testCompactAmplitudeLabels()
{
    beginTest("Compact dBFS captions preserve -1 dBFS and omit crowded lower-priority references");
    for (int height = 5; height <= 100; ++height) {
        const auto geometry = previewPlotGeometry({0, 0, 600, height}, PreviewViewMode::waveform, false);
        const auto labels = previewAmplitudeLabels(geometry, 2);
        for (std::size_t first = 0; first < labels.size(); ++first) {
            expect(geometry.amplitudeAxis.contains(labels[first].bounds));
            for (auto second = first + 1; second < labels.size(); ++second) {
                expect(!labels[first].bounds.intersects(labels[second].bounds));
                if (labels[first].channel == labels[second].channel) {
                    expect(labels[first].decibels >= labels[second].decibels);
                    expect(!labels[first]
                                .bounds.expanded(0, layout::waveform::AMPLITUDE_LABEL_SPACING)
                                .intersects(labels[second].bounds));
                }
            }
        }
    }
    const auto compact = previewPlotGeometry({0, 0, 600, 24}, PreviewViewMode::waveform, false);
    const auto compactLabels = previewAmplitudeLabels(compact, 1);
    expectEquals(static_cast<int>(compactLabels.size()), 1);
    if (!compactLabels.empty()) {
        expectWithinAbsoluteError(compactLabels.front().amplitude, std::pow(10.0f, -1.0f / 20.0f), 0.000001f);
        expectWithinAbsoluteError(compactLabels.front().decibels, -1.0f, 0.000001f);
    }
}

/// Checks the presentation transfer independently of calibration, quantization, caches or rendered pixels.
void SeratoPreviewTests::testSpectrogramDisplayIntensity()
{
    beginTest("Smoothstep display preserves saturated endpoints and monotonically darkens quiet values");
    expectWithinAbsoluteError(spectrogramDisplayIntensity(0), 0.0f, 0.000001f);
    expectWithinAbsoluteError(spectrogramDisplayIntensity(255), 1.0f, 0.000001f);
    expectWithinAbsoluteError(spectrogramDisplayIntensity(51), 0.104f, 0.000001f);
    expectWithinAbsoluteError(spectrogramDisplayIntensity(204), 0.896f, 0.000001f);
    for (int value = 0; value <= 255; ++value) {
        const auto displayed = spectrogramDisplayIntensity(static_cast<std::uint8_t>(value));
        const auto normalized = static_cast<float>(value) / 255.0f;
        expect(displayed >= 0.0f && displayed <= 1.0f);
        if (value > 0) {
            expect(displayed > spectrogramDisplayIntensity(static_cast<std::uint8_t>(value - 1)));
        }
        if (value > 0 && value < 128) {
            expect(displayed < normalized);
        } else if (value >= 128 && value < 255) {
            expect(displayed > normalized);
        }
        expectWithinAbsoluteError(
            displayed + spectrogramDisplayIntensity(static_cast<std::uint8_t>(255 - value)), 1.0f, 0.000001f
        );
    }
}

/// Verifies retained artifact lifecycle independently of native waveform readiness or painting.
void SeratoPreviewTests::testSpectrogramReset()
{
    beginTest("Pending source can adopt a spectrogram before peaks, and source replacement clears its raster");
    juce::AudioFormatManager formats;
    AudioPreviewPlayer transport;
    ThumbnailComponent thumbnail(formats, transport);
    thumbnail.setSize(600, 240);
    const utils::FileIdentity identity(fixtureFile("silence_stereo_44100_16.wav"));
    thumbnail.setPendingSource(44'100, 1.0, identity);
    thumbnail.setViewMode(PreviewViewMode::spectrogram);
    const auto data = std::make_shared<SpectrogramData>();
    data->sampleRate = 44'100;
    data->channelCount = 2;
    data->lengthInSamples = 44'100;
    const auto columns = 1 + (data->lengthInSamples - 1) / data->samplesPerColumn;
    data->intensities.resize(static_cast<std::size_t>(columns) * SpectrogramData::frequencyRows, 128);
    expect(isValidSpectrogramData(*data));
    thumbnail.setSpectrogramData(data);
    expect(thumbnail.hasSpectrogramRaster());
    thumbnail.setDisplayGain(4.0f);
    thumbnail.setRange({0.25, 0.75});
    thumbnail.setViewMode(PreviewViewMode::stacked);
    expect(thumbnail.hasSpectrogramRaster());
    expect(!thumbnail.getPlotGeometry().spectrogram.isEmpty());
    thumbnail.setPendingSource(44'100, 1.0, identity);
    expect(!thumbnail.hasSpectrogramRaster());
    expect(thumbnail.getViewMode() == PreviewViewMode::stacked);
    thumbnail.setSpectrogramData(data);
    expect(thumbnail.hasSpectrogramRaster());
    expect(thumbnail.loadFromCacheData(previewPeaks(formats, 2), identity));
    expect(!thumbnail.hasSpectrogramRaster());
    thumbnail.setSpectrogramData(data);
    expect(!thumbnail.hasSpectrogramRaster(), "Cached peaks establish a different source rate and frame count");
    const auto nativeGeometry = std::make_shared<SpectrogramData>();
    nativeGeometry->sampleRate = 8192;
    nativeGeometry->channelCount = 1;
    nativeGeometry->lengthInSamples = 8192;
    nativeGeometry->intensities.resize(std::size_t {8} * SpectrogramData::frequencyRows, 128);
    thumbnail.setSpectrogramData(nativeGeometry);
    expect(!thumbnail.hasSpectrogramRaster(), "Actual native channel geometry must also match");
    nativeGeometry->channelCount = 2;
    thumbnail.setSpectrogramData(nativeGeometry);
    expect(thumbnail.hasSpectrogramRaster());
    thumbnail.setPendingSource(44'100, 1.0, identity);
    const auto wrongRate = std::make_shared<SpectrogramData>(*data);
    wrongRate->sampleRate = 48'000;
    thumbnail.setSpectrogramData(wrongRate);
    expect(!thumbnail.hasSpectrogramRaster());
    const auto wrongLength = std::make_shared<SpectrogramData>(*data);
    --wrongLength->lengthInSamples;
    thumbnail.setSpectrogramData(wrongLength);
    expect(!thumbnail.hasSpectrogramRaster());
    thumbnail.setSpectrogramData(data);
    expect(thumbnail.hasSpectrogramRaster());
    thumbnail.setURL({});
    expect(!thumbnail.hasSpectrogramRaster());
    const auto invalid = std::make_shared<SpectrogramData>(*data);
    invalid->intensities.pop_back();
    thumbnail.setSpectrogramData(invalid);
    expect(!thumbnail.hasSpectrogramRaster());
}

/// Delivers real mouse handlers to both plots without a desktop peer or audio device.
void SeratoPreviewTests::testPaneScrubbing()
{
    beginTest("Each pane seeks in its own plot, crossing a split keeps the mouse-down projection");
    juce::AudioFormatManager formats;
    AudioPreviewPlayer transport;
    expect(transport.load(juce::URL(fixtureFile("silence_stereo_44100_16.wav"))));
    ThumbnailComponent thumbnail(formats, transport);
    thumbnail.setSize(600, 240);
    expect(thumbnail.loadFromCacheData(previewPeaks(formats, 2)));
    const auto eventAt = [&](const juce::Point<float> position, const int modifiers) {
        return juce::MouseEvent(
            juce::Desktop::getInstance().getMainMouseSource(),
            position,
            juce::ModifierKeys(modifiers),
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            &thumbnail,
            &thumbnail,
            juce::Time(),
            position,
            juce::Time(),
            1,
            false
        );
    };
    for (const auto mode : {PreviewViewMode::sideBySide, PreviewViewMode::stacked}) {
        thumbnail.setViewMode(mode);
        const auto geometry = thumbnail.getPlotGeometry();
        for (const auto plot : {geometry.waveform, geometry.spectrogram}) {
            transport.suspend();
            thumbnail.setRange({0.25, 0.75});
            const auto position
                = juce::Point<float>(previewTimeToX(0.5, {0.25, 0.75}, plot), static_cast<float>(plot.getCentreY()));
            const auto beforePress = transport.getCurrentPosition();
            thumbnail.mouseDown(eventAt(position, juce::ModifierKeys::leftButtonModifier));
            expectWithinAbsoluteError(transport.getCurrentPosition(), beforePress, 0.001);
            const auto crossing = juce::Point<float>(static_cast<float>(plot.getRight() + 100), position.y);
            thumbnail.mouseDrag(eventAt(crossing, juce::ModifierKeys::leftButtonModifier));
            expectWithinAbsoluteError(transport.getCurrentPosition(), beforePress, 0.001);
            thumbnail.mouseUp(eventAt(crossing, juce::ModifierKeys::leftButtonModifier));
            expectWithinAbsoluteError(transport.getCurrentPosition(), 0.75, 0.001);
            transport.suspend();
            transport.setPosition(0.5);
            thumbnail.mouseDown(eventAt(position, juce::ModifierKeys::middleButtonModifier));
            thumbnail.mouseDrag(eventAt(crossing, juce::ModifierKeys::middleButtonModifier));
            thumbnail.mouseUp(eventAt(crossing, juce::ModifierKeys::middleButtonModifier));
            thumbnail.mouseDoubleClick(eventAt(position, juce::ModifierKeys::middleButtonModifier));
            expectWithinAbsoluteError(transport.getCurrentPosition(), 0.5, 0.001);
            expect(!transport.isPlaying());
            // Exercise popup drag, release and double-click guards without opening a native popup menu.
            thumbnail.mouseDrag(eventAt(crossing, juce::ModifierKeys::rightButtonModifier));
            thumbnail.mouseUp(eventAt(crossing, juce::ModifierKeys::rightButtonModifier));
            thumbnail.mouseDoubleClick(eventAt(position, juce::ModifierKeys::rightButtonModifier));
            expectWithinAbsoluteError(transport.getCurrentPosition(), 0.5, 0.001);
            expect(!transport.isPlaying());
        }
    }
    transport.release();
}

/// Constructs the synthetic cue and track-info presentation suite.
std::unique_ptr<juce::UnitTest> createSeratoPreviewTests()
{
    return std::make_unique<SeratoPreviewTests>();
}
}  // namespace audiobatch::tests
