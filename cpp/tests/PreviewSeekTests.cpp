/// Device-free regression tests for click and drag seeking through the preview component.
/// The real player renders into a simulated output device.
/// Held playback and release-only seeking are exercised without physical audio, mouse automation, or visual assertions.

#include "gui/AudioBatchComponentState.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/components/PreviewPlotGeometry.h"
#include "gui/components/ThumbnailComponent.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/FileUtils.h"

#include <JuceHeader.h>

#include <memory>

namespace audiobatch::tests
{
namespace
{
/// Output blocks rendered while a click is held.
constexpr int HOLD_BLOCKS {32};
/// Source samples of the 44.1 kHz fixture that the held blocks consume at 48 kHz, with one block of margin.
constexpr int HOLD_SOURCE_SAMPLES {(HOLD_BLOCKS + 1) * 512 * 44'100 / 48'000 + 512};
/// Bounded wait for the read-ahead thread, generous for unoptimized and sanitizer builds.
constexpr juce::uint32 READ_AHEAD_TIMEOUT_MILLISECONDS {2000};

/// Constructs a component-local event, allowing releases with no button still held.
/// A click count of two marks the press and release that complete a double click.
juce::MouseEvent seekEvent(
    ThumbnailComponent& thumbnail,
    const juce::Point<float> position,
    const int modifiers,
    const int clicks = 1
)
{
    return {
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
        clicks,
        false
    };
}
}  // namespace

/// Covers release-time seeking and pane-latched projection using the production transport.
class PreviewSeekTests final : public juce::UnitTest
{
public:
    /// Registers the suite for explicit creation by the shared runner.
    PreviewSeekTests() : juce::UnitTest("Preview seek", "AudioBatch") {}

    void runTest() override;

private:
    void testClick(
        AudioPreviewPlayer& player,
        ThumbnailComponent& thumbnail,
        const TestAudioDeviceState& device,
        juce::Range<double> range,
        juce::Rectangle<int> plot,
        bool hold,
        bool initiallyPlaying
    );
    void testClicks(
        AudioPreviewPlayer& player,
        ThumbnailComponent& thumbnail,
        const TestAudioDeviceState& device,
        juce::Range<double> range
    );
    void testCrossingGestures(
        AudioPreviewPlayer& player,
        ThumbnailComponent& thumbnail,
        const TestAudioDeviceState& device,
        juce::Range<double> range
    );
    void testDoubleClickAndLatchedRange(ThumbnailComponent& thumbnail, AudioPreviewPlayer& player);
};

/// Checks held and immediate clicks, crossing drags, and non-seeking mouse gestures.
void PreviewSeekTests::runTest()
{
    const auto file = fixtureFile("silence_stereo_44100_16.wav");
    TestAudioDeviceState device;
    AudioPreviewPlayer player;
    installTestAudioDevice(player.getDeviceManager(), device);
    juce::AudioFormatManager formats;
    ThumbnailComponent thumbnail(formats, player);
    thumbnail.setSize(640, 240);

    beginTest("The committed preview fixture loads without opening physical audio");
    const auto loaded = player.load(juce::URL(file));
    expect(loaded);
    if (!loaded) {
        return;
    }
    const auto length = player.getLoadedLengthInSamples();
    const auto duration = player.getLoadedDurationSeconds();
    expect(length.has_value());
    expect(duration.has_value());
    if (!length || !duration) {
        return;
    }
    thumbnail.setPendingSource(*length, *duration, utils::FileIdentity(file));
    const juce::Range<double> range(0.25, 0.75);
    thumbnail.setRange(range);
    testClicks(player, thumbnail, device, range);
    testCrossingGestures(player, thumbnail, device, range);
    testDoubleClickAndLatchedRange(thumbnail, player);
    thumbnail.setRange(range);
    player.suspend();
    beginTest("Source replacement clears a held target without committing its seek");
    thumbnail.setViewMode(PreviewViewMode::waveform);
    const auto plot = thumbnail.getPlotGeometry().waveform;
    const auto position = juce::Point<float>(previewTimeToX(0.5, range, plot), static_cast<float>(plot.getCentreY()));
    thumbnail.mouseDown(seekEvent(thumbnail, position, juce::ModifierKeys::leftButtonModifier));
    expect(thumbnail.getPendingSeekPosition().has_value());
    thumbnail.setURL({});
    expect(!thumbnail.getPendingSeekPosition());
    const auto previous = player.getCurrentPosition();
    thumbnail.mouseUp(seekEvent(thumbnail, position, 0));
    expect(!player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), previous, 0.000001);
}

/// Exercises each available pane without coupling click expectations to the layout mode.
void PreviewSeekTests::testClicks(
    AudioPreviewPlayer& player,
    ThumbnailComponent& thumbnail,
    const TestAudioDeviceState& device,
    const juce::Range<double> range
)
{
    for (const auto mode :
         {
             PreviewViewMode::waveform,
             PreviewViewMode::spectrogram,
             PreviewViewMode::sideBySide,
             PreviewViewMode::stacked,
         })
    {
        thumbnail.setViewMode(mode);
        const auto geometry = thumbnail.getPlotGeometry();
        for (const auto plot : {geometry.waveform, geometry.spectrogram}) {
            if (plot.isEmpty()) {
                continue;
            }
            for (const auto initiallyPlaying : {true, false}) {
                for (const auto hold : {true, false}) {
                    testClick(player, thumbnail, device, range, plot, hold, initiallyPlaying);
                }
            }
        }
    }
}

/// Verifies held and immediate release-only seeks for both playing and stopped previews.
void PreviewSeekTests::testClick(
    AudioPreviewPlayer& player,
    ThumbnailComponent& thumbnail,
    const TestAudioDeviceState& device,
    const juce::Range<double> range,
    const juce::Rectangle<int> plot,
    const bool hold,
    const bool initiallyPlaying
)
{
    if (initiallyPlaying && hold) {
        beginTest("A held click keeps playing without seeking until release");
    } else if (initiallyPlaying) {
        beginTest("An immediate click seeks while preserving active playback");
    } else if (hold) {
        beginTest("A held stopped preview starts only on release");
    } else {
        beginTest("An immediate stopped click seeks and starts");
    }
    player.suspend();
    player.setPosition(0.05);
    if (initiallyPlaying) {
        player.start();
        renderTestAudioDevice(device);
    }
    const auto opens = device.opens;
    const auto closes = device.closes;
    const auto originalPosition = player.getCurrentPosition();
    const auto target = juce::Point<float>(previewTimeToX(0.6, range, plot), static_cast<float>(plot.getCentreY()));
    thumbnail.mouseDown(seekEvent(thumbnail, target, juce::ModifierKeys::leftButtonModifier));
    expect(player.isPlaying() == initiallyPlaying);
    expect(!player.isStopping());
    expectWithinAbsoluteError(thumbnail.getPendingSeekPosition().value_or(-1.0), 0.6, 0.000001);
    expectWithinAbsoluteError(player.getCurrentPosition(), originalPosition, 0.000001);
    if (hold) {
        // Exercise sustained playback while staying within the one-second fixture and initial read-ahead.
        // The loop renders faster than real time, so wait for the decoder to buffer the whole hold first,
        // which an unoptimized build does not do within the single block that the seek waits for.
        if (initiallyPlaying) {
            expect(player.waitForReadAhead(HOLD_SOURCE_SAMPLES, READ_AHEAD_TIMEOUT_MILLISECONDS));
        }
        for (int block = 0; block < HOLD_BLOCKS; ++block) {
            const auto before = player.getCurrentPosition();
            renderTestAudioDevice(device);
            player.updatePlaybackStatus();
            expect(player.isPlaying() == initiallyPlaying);
            expect(!player.isStopping());
            if (initiallyPlaying) {
                expectWithinAbsoluteError(player.getCurrentPosition(), before + 512.0 / 48'000.0, 0.000001);
            } else {
                expectWithinAbsoluteError(player.getCurrentPosition(), originalPosition, 0.000001);
            }
            expectEquals(device.opens, opens);
            expectEquals(device.closes, closes);
        }
    }
    thumbnail.mouseUp(seekEvent(thumbnail, target, 0));
    expect(!thumbnail.getPendingSeekPosition());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.6, 0.000001);
    expect(player.isPlaying());
    expect(!player.isStopping());
    expectEquals(device.opens, opens + (initiallyPlaying ? 0 : 1));
    expectEquals(device.closes, closes);
    renderTestAudioDevice(device);
    player.updatePlaybackStatus();
    expect(player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.6 + 512.0 / 48'000.0, 0.000001);
    const auto releasedPosition = player.getCurrentPosition();
    thumbnail.mouseUp(seekEvent(thumbnail, target, 0));
    expectWithinAbsoluteError(player.getCurrentPosition(), releasedPosition, 0.000001);
}

/// Checks latched crossing coordinates and guards against middle-button and popup transport changes.
void PreviewSeekTests::testCrossingGestures(
    AudioPreviewPlayer& player,
    ThumbnailComponent& thumbnail,
    const TestAudioDeviceState& device,
    const juce::Range<double> range
)
{
    thumbnail.setViewMode(PreviewViewMode::sideBySide);
    const auto geometry = thumbnail.getPlotGeometry();
    for (const auto plot : {geometry.waveform, geometry.spectrogram}) {
        const auto start = juce::Point<float>(previewTimeToX(0.5, range, plot), static_cast<float>(plot.getCentreY()));
        const auto crossing = juce::Point<float>(
            static_cast<float>(
                plot == geometry.waveform ? geometry.spectrogram.getCentreX() : geometry.waveform.getCentreX()
            ),
            start.y
        );
        const auto endpoint = plot == geometry.waveform ? range.getEnd() : range.getStart();
        beginTest("Crossing between side-by-side plots retains and clamps the mouse-down time projection");
        player.suspend();
        player.setPosition(0.05);
        player.start();
        renderTestAudioDevice(device);
        const auto opens = device.opens;
        const auto closes = device.closes;
        const auto originalDragPosition = player.getCurrentPosition();
        thumbnail.mouseDown(seekEvent(thumbnail, start, juce::ModifierKeys::leftButtonModifier));
        expectWithinAbsoluteError(player.getCurrentPosition(), originalDragPosition, 0.000001);
        const auto intermediate = juce::Point<float>(previewTimeToX(0.55, range, plot), start.y);
        thumbnail.mouseDrag(seekEvent(thumbnail, intermediate, juce::ModifierKeys::leftButtonModifier));
        expectWithinAbsoluteError(thumbnail.getPendingSeekPosition().value_or(-1.0), 0.55, 0.000001);
        expectWithinAbsoluteError(player.getCurrentPosition(), originalDragPosition, 0.000001);
        expect(player.waitForReadAhead(HOLD_SOURCE_SAMPLES, READ_AHEAD_TIMEOUT_MILLISECONDS));
        for (int block = 0; block < HOLD_BLOCKS; ++block) {
            const auto before = player.getCurrentPosition();
            thumbnail.mouseDrag(seekEvent(thumbnail, crossing, juce::ModifierKeys::leftButtonModifier));
            expectWithinAbsoluteError(thumbnail.getPendingSeekPosition().value_or(-1.0), endpoint, 0.000001);
            expectWithinAbsoluteError(player.getCurrentPosition(), before, 0.000001);
            renderTestAudioDevice(device);
            player.updatePlaybackStatus();
            expect(player.isPlaying());
            expect(!player.isStopping());
            expectWithinAbsoluteError(player.getCurrentPosition(), before + 512.0 / 48'000.0, 0.000001);
            expectEquals(device.opens, opens);
            expectEquals(device.closes, closes);
        }
        thumbnail.mouseUp(seekEvent(thumbnail, crossing, 0));
        expect(!thumbnail.getPendingSeekPosition());
        expectWithinAbsoluteError(player.getCurrentPosition(), endpoint, 0.000001);
        expect(player.isPlaying());
        expectEquals(device.opens, opens);
        expectEquals(device.closes, closes);
        renderTestAudioDevice(device);
        expectWithinAbsoluteError(player.getCurrentPosition(), endpoint + 512.0 / 48'000.0, 0.000001);

        beginTest("A middle-button gesture neither seeks nor interrupts active playback");
        player.suspend();
        player.setPosition(0.1);
        player.start();
        const auto originalPosition = player.getCurrentPosition();
        thumbnail.mouseDown(seekEvent(thumbnail, start, juce::ModifierKeys::middleButtonModifier));
        thumbnail.mouseDrag(seekEvent(thumbnail, crossing, juce::ModifierKeys::middleButtonModifier));
        thumbnail.mouseUp(seekEvent(thumbnail, crossing, 0));
        expectWithinAbsoluteError(player.getCurrentPosition(), originalPosition, 0.000001);
        expect(player.isPlaying());
        renderTestAudioDevice(device);
        player.updatePlaybackStatus();
        expectWithinAbsoluteError(player.getCurrentPosition(), originalPosition + 512.0 / 48'000.0, 0.000001);
        expect(player.isPlaying());

        beginTest("Popup drag and release events preserve running playback");
        const auto beforePopup = player.getCurrentPosition();
        const auto popupOpens = device.opens;
        const auto popupCloses = device.closes;
        // Exercise popup guards without opening a native menu or requiring a component peer.
        thumbnail.mouseDrag(seekEvent(thumbnail, crossing, juce::ModifierKeys::rightButtonModifier));
        thumbnail.mouseUp(seekEvent(thumbnail, crossing, juce::ModifierKeys::rightButtonModifier));
        thumbnail.mouseUp(seekEvent(thumbnail, crossing, 0));
        expectWithinAbsoluteError(player.getCurrentPosition(), beforePopup, 0.000001);
        renderTestAudioDevice(device);
        player.updatePlaybackStatus();
        expectWithinAbsoluteError(player.getCurrentPosition(), beforePopup + 512.0 / 48'000.0, 0.000001);
        expect(player.isPlaying());
        expectEquals(device.opens, popupOpens);
        expectEquals(device.closes, popupCloses);

        beginTest("A popup-modified release cancels a pending seek without interrupting playback");
        thumbnail.mouseDown(seekEvent(thumbnail, start, juce::ModifierKeys::leftButtonModifier));
        const auto beforeCancelledSeek = player.getCurrentPosition();
        expect(thumbnail.getPendingSeekPosition().has_value());
        thumbnail.mouseUp(seekEvent(thumbnail, crossing, juce::ModifierKeys::rightButtonModifier));
        expect(!thumbnail.getPendingSeekPosition());
        thumbnail.mouseUp(seekEvent(thumbnail, crossing, 0));
        expectWithinAbsoluteError(player.getCurrentPosition(), beforeCancelledSeek, 0.000001);
        renderTestAudioDevice(device);
        player.updatePlaybackStatus();
        expectWithinAbsoluteError(player.getCurrentPosition(), beforeCancelledSeek + 512.0 / 48'000.0, 0.000001);
        expect(player.isPlaying());
        expectEquals(device.opens, popupOpens);
        expectEquals(device.closes, popupCloses);

        beginTest("Popup drag and release events never seek or resume a stopped preview");
        player.suspend();
        player.setPosition(0.1);
        // Exercise popup guards without opening a native menu or requiring a component peer.
        thumbnail.mouseDrag(seekEvent(thumbnail, crossing, juce::ModifierKeys::rightButtonModifier));
        thumbnail.mouseUp(seekEvent(thumbnail, crossing, juce::ModifierKeys::rightButtonModifier));
        thumbnail.mouseUp(seekEvent(thumbnail, crossing, 0));
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.1, 0.000001);
        expect(!player.isPlaying());

        beginTest("A popup-modified release cannot commit an active left-button scrub");
        thumbnail.mouseDown(seekEvent(thumbnail, start, juce::ModifierKeys::leftButtonModifier));
        const auto scrubPosition = player.getCurrentPosition();
        thumbnail.mouseUp(seekEvent(thumbnail, crossing, juce::ModifierKeys::rightButtonModifier));
        thumbnail.mouseUp(seekEvent(thumbnail, crossing, 0));
        expectWithinAbsoluteError(player.getCurrentPosition(), scrubPosition, 0.000001);
        expect(!player.isPlaying());
    }
    player.suspend();
}

/// Checks that a double click restarts without an intermediate seek,
/// and that zooming during a held drag keeps the mouse-down time projection.
void PreviewSeekTests::testDoubleClickAndLatchedRange(ThumbnailComponent& thumbnail, AudioPreviewPlayer& player)
{
    thumbnail.setViewMode(PreviewViewMode::waveform);
    const juce::Range<double> range(0.25, 0.75);
    thumbnail.setRange(range);
    const auto plot = thumbnail.getPlotGeometry().waveform;
    const auto target = juce::Point<float>(previewTimeToX(0.6, range, plot), static_cast<float>(plot.getCentreY()));

    beginTest("The release completing a double click leaves the restart to the double-click handler");
    player.suspend();
    player.setPosition(0.4);
    thumbnail.mouseDown(seekEvent(thumbnail, target, juce::ModifierKeys::leftButtonModifier, 2));
    expectWithinAbsoluteError(thumbnail.getPendingSeekPosition().value_or(-1.0), 0.0, 0.000001);
    thumbnail.mouseDrag(seekEvent(thumbnail, target, juce::ModifierKeys::leftButtonModifier, 2));
    expectWithinAbsoluteError(thumbnail.getPendingSeekPosition().value_or(-1.0), 0.0, 0.000001);
    thumbnail.mouseUp(seekEvent(thumbnail, target, 0, 2));
    expect(!thumbnail.getPendingSeekPosition());
    expect(!player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.4, 0.000001);
    thumbnail.mouseDoubleClick(seekEvent(thumbnail, target, juce::ModifierKeys::leftButtonModifier, 2));
    expect(player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.0, 0.000001);

    beginTest("Zooming during a held drag keeps the mouse-down time projection");
    player.suspend();
    player.setPosition(0.1);
    thumbnail.mouseDown(seekEvent(thumbnail, target, juce::ModifierKeys::leftButtonModifier));
    thumbnail.setRange({0.5, 0.6});
    thumbnail.mouseDrag(seekEvent(thumbnail, target, juce::ModifierKeys::leftButtonModifier));
    expectWithinAbsoluteError(thumbnail.getPendingSeekPosition().value_or(-1.0), 0.6, 0.000001);
    thumbnail.mouseUp(seekEvent(thumbnail, target, 0));
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.6, 0.000001);
    player.suspend();
}

/// Creates the click-seek regression suite for registration by the parent runner.
std::unique_ptr<juce::UnitTest> createPreviewSeekTests()
{
    return std::make_unique<PreviewSeekTests>();
}
}  // namespace audiobatch::tests
