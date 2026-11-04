/// Device-free tests for the preview player around the renderer:
/// the cue start position, the deferred initial position, the ownership of the simulated output device,
/// the output device status shown in the status bar, the persisted preview preferences, and the loudness axis ticks.
/// Also checks settings geometry and validation space, and the inspector's status-free summary and wrapped details.
/// The committed silence fixture exercises playback without opening hardware or third-party plugins.

#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/components/AudioSettingsComponent.h"
#include "gui/components/StatusBar.h"
#include "gui/GuiLayout.h"
#include "serato/SeratoTagTypes.h"
#include "TestHelpers.h"
#include "TestSuites.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace audiobatch::tests
{
/// Covers the preview player, its device and preference policy, and the waveform axis, not GUI rendering.
class PreviewPlayerTests final : public juce::UnitTest
{
public:
    /// Registers a lightweight suite with the existing factory-driven runner.
    PreviewPlayerTests() : juce::UnitTest("Preview player", "AudioBatch") {}

    void runTest() override;

private:
    void testAsioDeviceSelection();
    void testCueStartPosition();
    void testDeviceOwnership();
    void testInitialPosition();
    void testInspectorStatusRemoval();
    void testInspectorDetailsLayout();

    void testLoudnessAxisIntervals();
    void testOutputStatusFormatting();
    void testOverloadPreferences();
    void testPreferences();
    void testPreviewFitHeight();
    void testPreviewViewModes();
    void testPreviewWidths();
    void testSettingsDeviceFailure();
    void testSettingsGeometry();
    void verifyDisabledLoudnessRange(
        AudioSettingsComponent& panel,
        const juce::Component& content,
        juce::TextEditor& minimumEditor
    );
    void verifyOverloadSettingsGroup(const juce::Component& content, const juce::GroupComponent& previewGroup);
};

/// Runs the device-independent player and persistence checks.
void PreviewPlayerTests::runTest()
{
    testCueStartPosition();
    testInitialPosition();
    testDeviceOwnership();
    testAsioDeviceSelection();
    testSettingsDeviceFailure();
    testOutputStatusFormatting();
    testPreferences();
    testOverloadPreferences();
    testSettingsGeometry();
    testInspectorStatusRemoval();
    testInspectorDetailsLayout();
    testPreviewViewModes();
    testLoudnessAxisIntervals();
    testPreviewFitHeight();
    testPreviewWidths();
}

/// Checks the status bar text of the output device for fractional rates and values that are not known yet.
void PreviewPlayerTests::testOutputStatusFormatting()
{
    beginTest("Output status text leaves out unknown values and keeps fractional sample rates");
    expectEquals(
        formatAudioOutputStatus({.deviceName = "Speakers", .sampleRate = 44'100.0, .bufferSize = 256, .open = true}),
        juce::String("Speakers, 44.1 kHz, 256 samples")
    );
    expectEquals(
        formatAudioOutputStatus({.deviceName = "Speakers", .sampleRate = 0.0, .bufferSize = 0, .open = false}),
        juce::String("Speakers")
    );
    expectEquals(
        formatAudioOutputStatus({.deviceName = {}, .sampleRate = 96'000.0, .bufferSize = 0, .open = false}),
        juce::String("Default output, 96 kHz")
    );
}

/// Checks that the startup preview height fits the controls of the layout that the workspace width selects.
void PreviewPlayerTests::testPreviewFitHeight()
{
    beginTest("The startup preview height fits the controls of the wide and the one-column layouts");
    expectEquals(layout::widePreviewFitHeight, 282);
    expectEquals(layout::narrowPreviewFitHeight, 348);

    const auto wideBreakpoint = (layout::widePreview::minimumHalfWidth * 2) + (layout::compactPreview::padding * 2);
    expectEquals(layout::expandedPreviewFitHeight(wideBreakpoint), layout::widePreviewFitHeight);
    expectEquals(layout::expandedPreviewFitHeight(wideBreakpoint - 1), layout::narrowPreviewFitHeight);
    expectEquals(
        layout::expandedPreviewFitHeight(layout::defaultWindowWidth - (layout::mainContentPadding * 2)),
        layout::widePreviewFitHeight
    );
    expectEquals(layout::expandedPreviewFitHeight(layout::minimumWindowWidth), layout::narrowPreviewFitHeight);
    expectEquals(layout::expandedPreviewFitHeight(0), layout::narrowPreviewFitHeight);
    expectEquals(layout::expandedPreviewFitHeight(wideBreakpoint + 1), layout::widePreviewFitHeight);

    beginTest("Short windows clamp the controls fit while preserving the table minimum and manual preference");
    expectEquals(layout::constrainedPreviewHeight(348, 530), 284);
    expectEquals(layout::constrainedPreviewHeight(348, 1000), 348);
    expectEquals(layout::constrainedPreviewHeight(282, 530), 282);
    expectEquals(layout::constrainedPreviewHeight(1000, 1000), 700);
    expectEquals(layout::constrainedPreviewHeight(100, 530), 230);
    // A user-picked split does not re-fit when crossing the wide breakpoint or collapsing and expanding.
    for (const auto preferred : {250, 300, 420}) {
        expectEquals(layout::constrainedPreviewHeight(preferred, 1000), preferred);
        static_cast<void>(layout::constrainedPreviewHeight(preferred, 530));
        expectEquals(layout::constrainedPreviewHeight(preferred, 1000), preferred);
    }
}

/// Checks scaled defaults, independent bounds, compact compatibility, and transient window constraints.
void PreviewPlayerTests::testPreviewWidths()
{
    beginTest("Preview defaults narrow the original responsive controls by five percent and information by ten");
    const auto narrow = layout::previewColumns({0, 0, 1200, 300});
    expectEquals(narrow.controls.getWidth(), 199);
    expectEquals(narrow.information.getWidth(), 238);
    expectEquals(narrow.waveform.getWidth(), 743);
    expect(!narrow.wide);
    const auto wide = layout::previewColumns({0, 0, 1900, 300});
    expectEquals(wide.controls.getWidth(), 428);
    expectEquals(wide.information.getWidth(), 440);
    expectEquals(wide.waveform.getWidth(), 1012);
    expect(wide.wide);
    expect(!layout::previewColumns({0, 0, 1579, 300}).wide);
    expect(layout::previewColumns({0, 0, 1580, 300}).wide);

    beginTest("Changing either scale changes only that column and the waveform, never its sibling width");
    for (const auto width : {800, 1200, 1580, 1900, 4000}) {
        const juce::Rectangle<int> bounds {0, 0, width, 300};
        const auto defaults = layout::previewColumns(bounds);
        const auto controls = layout::previewColumns(bounds, true, 1.10, 0.90);
        expectEquals(controls.information.getWidth(), defaults.information.getWidth());
        expectEquals(controls.information.getX() - controls.controls.getRight(), layout::sectionSpacing);
        const auto information = layout::previewColumns(bounds, true, 0.95, 1.10);
        expectEquals(information.controls.getWidth(), defaults.controls.getWidth());
        expectEquals(information.waveform.getX() - information.information.getRight(), layout::sectionSpacing);
        const auto minimum = layout::previewColumns(bounds, true, -1.0, -1.0);
        const auto maximum = layout::previewColumns(bounds, true, 100.0, 100.0);
        const auto baseline = layout::previewColumnBaseline(bounds);
        expectEquals(minimum.controls.getWidth(), juce::roundToInt(baseline.controls.getWidth() * 0.855));
        expectEquals(minimum.information.getWidth(), juce::roundToInt(baseline.information.getWidth() * 0.81));
        expectEquals(maximum.controls.getWidth(), juce::roundToInt(baseline.controls.getWidth() * 1.10));
        expectEquals(maximum.information.getWidth(), juce::roundToInt(baseline.information.getWidth() * 1.10));
        expect(maximum.waveform.getWidth() >= 140);
        const auto compact = layout::previewColumns(bounds, false, 1.10, 1.10);
        expectEquals(compact.controls.getWidth(), layout::previewColumnBaseline(bounds, false).controls.getWidth());
        expect(!compact.wide);
    }

    beginTest("Nonfinite scales fall back independently and undersized windows keep every rectangle nonnegative");
    const auto invalid = layout::previewColumns(
        {0, 0, 1200, 300}, true, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()
    );
    expect(invalid.controls == narrow.controls);
    expect(invalid.information == narrow.information);
    for (const auto width : {0, 1, 20, 100, 140, 141, 159, 160, 300, 600, 800}) {
        const auto columns = layout::previewColumns({0, 0, width, 300}, true, 1.10, 1.10);
        for (const auto rectangle : {columns.controls, columns.information, columns.waveform}) {
            expect(rectangle.getWidth() >= 0);
            expect(rectangle.getHeight() >= 0);
            expect(rectangle.getX() >= 0);
            expect(rectangle.getRight() <= width);
        }
        expect(columns.waveform.getWidth() >= std::min(width, 140));
    }
}

/// Checks numeric tick layout without constructing or rendering a GUI component.
void PreviewPlayerTests::testLoudnessAxisIntervals()
{
    beginTest("Loudness ticks use even intervals instead of independently rounded labels");
    expectEquals(layout::waveform::loudnessTickInterval(-24, -4, 230), 4);
    expectEquals(layout::waveform::loudnessTickInterval(-23, -5, 230), 4);
    expectEquals(layout::waveform::loudnessTickInterval(-24, -4, 48), 10);
    expectEquals(layout::waveform::loudnessTickInterval(-60, 6, 48), 20);
    expectEquals(layout::waveform::loudnessTickInterval(-6, -4, 230), 1);
    expectEquals(layout::waveform::loudnessTickInterval(5, 6, 48), 1);

    beginTest("Every supported loudness range has uniform in-range ticks at compact and expanded heights");
    for (int minimum = -60; minimum < 6; ++minimum) {
        for (int maximum = minimum + 1; maximum <= 6; ++maximum) {
            for (const auto height : {48, 64, 128, 230, 480}) {
                const auto interval = layout::waveform::loudnessTickInterval(minimum, maximum, height);
                const auto first = static_cast<int>(std::ceil(static_cast<double>(minimum) / interval)) * interval;
                expect(first >= minimum && first <= maximum);
                expect(interval <= 20);
                expect(interval == 1 || interval % 2 == 0);
                if (maximum - minimum > interval) {
                    expect(
                        static_cast<double>(interval * height) / (maximum - minimum)
                        >= static_cast<double>(layout::waveform::loudnessFontHeight)
                    );
                }
                int previous = first - interval;
                for (int value = first; value <= maximum; value += interval) {
                    expectEquals(value - previous, interval);
                    expect(value >= minimum);
                    expect(interval == 1 || value % 2 == 0);
                    previous = value;
                }
            }
        }
    }
}

/// Selects chronological cues rather than hot-cue indices and rejects missing or unsafe start positions.
void PreviewPlayerTests::testCueStartPosition()
{
    const AudioPreviewPreferences enabled {.playFromFirstCuePoint = true};
    serato::Markers markers;

    beginTest("Cue starts require an enabled preference and nonempty markers");
    expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, nullptr, 10.0), 0.0, 0.000001);
    expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 0.0, 0.000001);
    markers.cues.push_back({.positionMilliseconds = 1250, .index = 7, .colour = {}, .name = {}});
    expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 1.25, 0.000001);
    expectWithinAbsoluteError(getPreviewStartPositionSeconds(AudioPreviewPreferences(), &markers, 10.0), 0.0, 0.000001);

    beginTest("Unsorted cues use the earliest time even when another cue has index zero or is after halfway");
    markers.cues.insert(markers.cues.begin(), {.positionMilliseconds = 7000, .index = 0, .colour = {}, .name = {}});
    markers.cues.push_back({.positionMilliseconds = 2500, .index = 1, .colour = {}, .name = {}});
    expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 1.25, 0.000001);

    beginTest("A cue exactly halfway is accepted but one millisecond later falls back to zero");
    markers.cues = {{.positionMilliseconds = 5000, .index = 0, .colour = {}, .name = {}}};
    expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 5.0, 0.000001);
    markers.cues.front().positionMilliseconds = 5001;
    expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 0.0, 0.000001);

    beginTest("Nonpositive and nonfinite durations cannot select a cue start");
    markers.cues.front().positionMilliseconds = 1250;
    for (const auto duration :
         {
             0.0,
             -1.0,
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity(),
         })
    {
        expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, duration), 0.0, 0.000001);
    }

    beginTest("A zero-position cue remains at the beginning even with later cues");
    markers.cues.push_back({.positionMilliseconds = 0, .index = 7, .colour = {}, .name = {}});
    expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 0.0, 0.000001);
}

/// Exercises deferred cue resolution on one player with a simulated output driver.
/// Each load establishes a fresh wait so completion, cancellation, release, and replacement remain independent.
void PreviewPlayerTests::testInitialPosition()
{
    const juce::URL silenceUrl(fixtureFile("silence_stereo_44100_16.wav"));
    TestAudioDeviceState device;
    AudioPreviewPlayer player;
    installTestAudioDevice(player.getDeviceManager(), device);

    beginTest("Resolving a stopped preview positions it once without starting playback");
    expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
    expect(player.load(silenceUrl, true));
    expect(player.hasLoadedSource());
    expect(!player.isPlaying());
    player.completeInitialPosition(0.25);
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);
    expect(!player.isPlaying());
    player.completeInitialPosition(0.5);
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);
    expect(!player.isPlaying());

    beginTest("A pending start is logically playing at zero until its initial position is resolved");
    expect(player.load(silenceUrl, true));
    player.start();
    expect(player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.0, 0.000001);
    player.completeInitialPosition(0.25);
    expect(player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);
    player.completeInitialPosition(0.5);
    expect(player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);

    beginTest("Stopping during the initial wait cancels the pending start but still allows cue positioning");
    expect(player.load(silenceUrl, true));
    player.start();
    expect(player.isPlaying());
    player.stop();
    expect(!player.isPlaying());
    player.completeInitialPosition(0.25);
    expect(!player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);
    player.start();
    expect(player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);

    beginTest("An explicit seek cancels the initial wait while preserving a pending start");
    expect(player.load(silenceUrl, true));
    player.start();
    player.setPosition(0.375);
    expect(player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.375, 0.000001);
    player.completeInitialPosition(0.25);
    expect(player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.375, 0.000001);

    beginTest("An explicit seek cancels a stopped preview's initial wait without starting it");
    expect(player.load(silenceUrl, true));
    player.setPosition(0.375);
    expect(!player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.375, 0.000001);
    player.completeInitialPosition(0.25);
    expect(!player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.375, 0.000001);

    beginTest("Release clears a pending start and ignores late initial-position completion");
    expect(player.load(silenceUrl, true));
    player.start();
    expect(player.isPlaying());
    player.release();
    expect(!player.hasLoadedSource());
    expect(!player.getLoadedDurationSeconds().has_value());
    expect(!player.isPlaying());
    player.completeInitialPosition(0.25);
    player.start();
    expect(!player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.0, 0.000001);

    beginTest("A replacement load defaults to no wait and discards the previous pending start");
    expect(player.load(silenceUrl, true));
    expect(!player.isPlaying());
    player.start();
    expect(player.isPlaying());
    expect(player.load(silenceUrl));
    expect(player.hasLoadedSource());
    expect(!player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.0, 0.000001);
    player.completeInitialPosition(0.25);
    expect(!player.isPlaying());
    expectWithinAbsoluteError(player.getCurrentPosition(), 0.0, 0.000001);
    player.start();
    expect(player.isPlaying());
    player.stop();
    expect(!player.isPlaying());
    renderTestAudioDevice(device);
    player.updatePlaybackStatus();
    expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
}

/// Exercises the real device manager and player lifetime without native audio hardware.
void PreviewPlayerTests::testDeviceOwnership()
{
    const juce::URL audioUrl(fixtureFile("silence_stereo_44100_16.wav"));
    TestAudioDeviceState device;
    {
        AudioPreviewPlayer player;
        installTestAudioDevice(player.getDeviceManager(), device);
        beginTest("Startup and file selection leave the audio device closed");
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
        expect(player.load(audioUrl));
        player.updatePlaybackStatus();
        expectEquals(device.opens, 0);
        expect(!player.getOutputStatus().open);
        expectEquals(formatAudioOutputStatus(player.getOutputStatus()), juce::String("Default output"));

        beginTest("Playback opens only output channels and stopping releases the driver after its fade");
        player.start();
        expect(player.isPlaying());
        expectEquals(device.opens, 1);
        expectEquals(device.inputChannels, 0);
        expectEquals(device.outputChannels, 2);
        expect(player.getOutputStatus().open);
        expectEquals(
            formatAudioOutputStatus(player.getOutputStatus()), juce::String("Test output, 48 kHz, 512 samples")
        );
        renderTestAudioDevice(device);
        player.stop();
        renderTestAudioDevice(device);
        player.updatePlaybackStatus();
        expect(!player.isPlaying());
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
        expectEquals(device.closes, 1);

        beginTest("A closed output device keeps reporting its last configuration");
        expect(!player.getOutputStatus().open);
        expectEquals(
            formatAudioOutputStatus(player.getOutputStatus()), juce::String("Test output, 48 kHz, 512 samples")
        );

        beginTest("Suspension releases immediately and does not restart until an explicit play");
        const auto position = player.getCurrentPosition();
        player.start();
        expectEquals(device.opens, 2);
        player.setApplicationActive(false);
        expect(!player.isPlaying());
        expect(player.hasLoadedSource());
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
        expectEquals(device.closes, 2);
        expectWithinAbsoluteError(player.getCurrentPosition(), position, 0.000001);
        player.updatePlaybackStatus();
        expectEquals(device.opens, 2);
        player.setApplicationActive(true);
        expect(!player.isPlaying());
        expectEquals(device.opens, 2);

        beginTest("Audio Settings leases the device only while the application is focused");
        player.setDeviceSettingsActive(true);
        expect(player.getDeviceManager().getCurrentAudioDevice() != nullptr);
        expect(!player.isPlaying());
        player.updatePlaybackStatus();
        expect(player.getDeviceManager().getCurrentAudioDevice() != nullptr);
        player.setApplicationActive(false);
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
        player.setApplicationActive(true);
        expect(player.getDeviceManager().getCurrentAudioDevice() != nullptr);
        expect(!player.isPlaying());
        player.setDeviceSettingsActive(false);
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);

        beginTest("EOF releases the output device even without waveform repainting");
        player.setPosition(0.999);
        player.start();
        renderTestAudioDevice(device);
        player.updatePlaybackStatus();
        expect(!player.isPlaying());
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
        expectEquals(device.opens, device.closes);

        beginTest("An unavailable device reports its error and does not leave logical playback active");
        juce::String error;
        player.setErrorCallback([&](const juce::String& message) { error = message; });
        device.openError = "Test output is in use by another application";
        player.start();
        expect(!player.isPlaying());
        expectEquals(error, device.openError);
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
        device.openError.clear();
        player.setErrorCallback({});

        beginTest("Selecting another file while playing keeps the device open");
        player.start();
        expect(player.isPlaying());
        const auto opensWhilePlaying = device.opens;
        const auto closesWhilePlaying = device.closes;
        expect(player.load(audioUrl));
        player.start();
        expect(player.isPlaying());
        expectEquals(device.opens, opensWhilePlaying);
        expectEquals(device.closes, closesWhilePlaying);

        beginTest("Shutdown balances all device opens even if playback is still active");
        player.start();
        expect(player.isPlaying());
        expectEquals(device.opens, device.closes + 1);
    }
    expectEquals(device.opens, device.closes);
    expect(device.callback == nullptr);

    beginTest("Only a device chosen in the settings is saved, and the next session opens it on the first play");
    std::unique_ptr<juce::XmlElement> savedState;
    {
        AudioPreviewPlayer player;
        installTestAudioDevice(player.getDeviceManager(), device);
        expect(player.load(audioUrl));
        player.start();
        expect(player.createDeviceState() == nullptr);
        const auto setup = player.getDeviceManager().getAudioDeviceSetup();
        player.stop();
        renderTestAudioDevice(device);
        player.updatePlaybackStatus();
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
        // JUCE ignores choosing the setup the open device already has, so the choice is made while it is closed.
        expect(player.getDeviceManager().setAudioDeviceSetup(setup, true).isEmpty());
        savedState = player.createDeviceState();
        expect(savedState != nullptr);
    }
    {
        AudioPreviewPlayer player;
        installTestAudioDevice(player.getDeviceManager(), device);
        player.setSavedDeviceState(std::move(savedState));
        expect(player.load(audioUrl));
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
        player.start();
        expect(player.isPlaying());
        expectEquals(player.getOutputStatus().deviceName, juce::String("Test output"));
        const auto restored = player.createDeviceState();
        expect(restored != nullptr);
        if (restored != nullptr) {
            expectEquals(restored->getStringAttribute("audioOutputDeviceName"), juce::String("Test output"));
        }
    }
    expectEquals(device.opens, device.closes);
}

/// Finds connected ASIO hardware without treating the registry default as the only usable interface.
void PreviewPlayerTests::testAsioDeviceSelection()
{
    TestAudioDeviceState device;
    device.typeName = "ASIO";
    device.outputDeviceNames = {"Disconnected default", "Connected output", "Other output"};
    device.unavailableDeviceNames = {"Disconnected default"};
    AudioPreviewPlayer player;
    installTestAudioDevice(player.getDeviceManager(), device);
    juce::String error;
    player.setErrorCallback([&error](const juce::String& message) { error = message; });

    beginTest("An unconfigured ASIO host skips disconnected registered drivers");
    const ScopedLogCapture logs;
    player.setDeviceSettingsActive(true);
    expect(error.isEmpty());
    expectEquals(player.getOutputStatus().deviceName, juce::String("Connected output"));
    expectEquals(device.attemptedDevices.joinIntoString(", "), juce::String("Disconnected default, Connected output"));
    expectEquals(device.inputChannels, 0);
    expectEquals(device.outputChannels, 2);
    expect(logs.getMessages().joinIntoString("\n").contains("ASIO output \"Disconnected default\""));
    expect(player.createDeviceState() == nullptr);

    beginTest("A subsequent open retains the working device rather than probing the default again");
    player.setApplicationActive(false);
    device.attemptedDevices.clear();
    player.setApplicationActive(true);
    expectEquals(device.attemptedDevices.joinIntoString(", "), juce::String("Connected output"));

    beginTest("A selected device that becomes unavailable does not switch to another ASIO output");
    player.setDeviceSettingsActive(false);
    device.unavailableDeviceNames.add("Connected output");
    device.attemptedDevices.clear();
    player.setDeviceSettingsActive(true);
    expect(error.isNotEmpty());
    expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
    expectEquals(device.attemptedDevices.joinIntoString(", "), juce::String("Connected output"));
    player.setErrorCallback({});

    beginTest("A saved selection is retained across failed explicit retries");
    TestAudioDeviceState savedDevice;
    savedDevice.typeName = "ASIO";
    savedDevice.outputDeviceNames = {"Connected default", "Saved output"};
    savedDevice.unavailableDeviceNames = {"Saved output"};
    AudioPreviewPlayer restored;
    installTestAudioDevice(restored.getDeviceManager(), savedDevice);
    auto saved = std::make_unique<juce::XmlElement>("DEVICESETUP");
    saved->setAttribute("deviceType", "ASIO");
    saved->setAttribute("audioOutputDeviceName", "Saved output");
    saved->setAttribute("audioDeviceInChans", "0");
    restored.setSavedDeviceState(std::move(saved));
    int failures = 0;
    restored.setErrorCallback([&failures](const juce::String&) { ++failures; });
    restored.setDeviceSettingsActive(true);
    restored.setDeviceSettingsActive(false);
    restored.setDeviceSettingsActive(true);
    expectEquals(failures, 2);
    expectEquals(savedDevice.attemptedDevices.joinIntoString(", "), juce::String("Saved output, Saved output"));
    savedDevice.unavailableDeviceNames.clear();
    restored.setDeviceSettingsActive(false);
    restored.setDeviceSettingsActive(true);
    expectEquals(restored.getOutputStatus().deviceName, juce::String("Saved output"));
    restored.setErrorCallback({});

    beginTest("Exhausting unavailable ASIO outputs reports a contextual error once");
    TestAudioDeviceState unavailable;
    unavailable.typeName = "ASIO";
    unavailable.outputDeviceNames = {"First disconnected", "Second disconnected"};
    unavailable.unavailableDeviceNames = unavailable.outputDeviceNames;
    AudioPreviewPlayer failed;
    installTestAudioDevice(failed.getDeviceManager(), unavailable);
    int failedAttempts = 0;
    failed.setErrorCallback([&](const juce::String& message) {
        error = message;
        ++failedAttempts;
    });
    failed.setDeviceSettingsActive(true);
    expectEquals(unavailable.attemptedDevices.size(), 2);
    expect(error.contains("Second disconnected"));
    expect(error.contains("No ASIO output could be opened"));
    failed.setApplicationActive(false);
    failed.setApplicationActive(true);
    expectEquals(failedAttempts, 1);
    failed.setErrorCallback({});
}

/// Keeps a failed settings lease from reopening the device every time its error dialog returns focus.
void PreviewPlayerTests::testSettingsDeviceFailure()
{
    TestAudioDeviceState device;
    AudioPreviewPlayer player;
    installTestAudioDevice(player.getDeviceManager(), device);
    device.openError = "No device is connected to the PC.";
    int errors = 0;
    player.setErrorCallback([&errors](const juce::String&) { ++errors; });

    beginTest("An unavailable settings device is reported once across focus changes");
    player.setDeviceSettingsActive(true);
    expectEquals(errors, 1);
    for (int attempt = 0; attempt < 3; ++attempt) {
        player.setApplicationActive(false);
        player.setApplicationActive(true);
        player.setDeviceSettingsActive(true);
        expectEquals(errors, 1);
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
    }

    beginTest("Explicitly reopening settings retries the device and reports a new failure");
    player.setDeviceSettingsActive(false);
    player.setDeviceSettingsActive(true);
    expectEquals(errors, 2);

    beginTest("An explicit retry can recover and restores normal focus-driven device ownership");
    device.openError.clear();
    player.setDeviceSettingsActive(false);
    player.setDeviceSettingsActive(true);
    expect(player.getDeviceManager().getCurrentAudioDevice() != nullptr);
    player.setApplicationActive(false);
    expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
    player.setApplicationActive(true);
    expect(player.getDeviceManager().getCurrentAudioDevice() != nullptr);
    expectEquals(errors, 2);

    beginTest("Choosing a working device in the selector also restores focus-driven ownership");
    player.setDeviceSettingsActive(false);
    device.openError = "No device is connected to the PC.";
    player.setDeviceSettingsActive(true);
    expectEquals(errors, 3);
    device.openError.clear();
    expect(player.getDeviceManager().initialise(0, 2, nullptr, true).isEmpty());
    expect(player.getDeviceManager().getCurrentAudioDevice() != nullptr);
    player.setApplicationActive(false);
    expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
    player.setApplicationActive(true);
    expect(player.getDeviceManager().getCurrentAudioDevice() != nullptr);
    expectEquals(errors, 3);
    player.setErrorCallback({});
}

/// Uses an isolated settings file to verify unset defaults, persistence, and actual continuation policy.
void PreviewPlayerTests::testPreferences()
{
    beginTest("Audio Settings changes keep the menu-only album artwork choice");
    {
        juce::AudioDeviceManager manager;
        AudioSettingsComponent settingsComponent(manager);
        AudioPreviewPreferences shown;
        shown.showAlbumArtwork = false;
        shown.viewMode = PreviewViewMode::spectrogram;
        settingsComponent.setPreferences(shown);
        std::optional<AudioPreviewPreferences> reported;
        settingsComponent.setPreferencesChangedCallback([&reported](const AudioPreviewPreferences& changed) {
            reported = changed;
        });
        juce::ToggleButton* coloursToggle = nullptr;
        std::function<void(juce::Component&)> findToggle = [&](juce::Component& parent) {
            for (auto* child : parent.getChildren()) {
                if (auto* toggle = dynamic_cast<juce::ToggleButton*>(child);
                    toggle != nullptr && toggle->getButtonText() == "Serato waveform colours")
                {
                    coloursToggle = toggle;
                    return;
                }
                findToggle(*child);
            }
        };
        findToggle(settingsComponent);
        expect(coloursToggle != nullptr);
        if (coloursToggle != nullptr && coloursToggle->onClick) {
            // The test runner has no dispatch loop, so the click is applied the way the button would apply it.
            coloursToggle->setToggleState(false, juce::dontSendNotification);
            coloursToggle->onClick();
        }
        expect(reported.has_value());
        if (reported) {
            expect(!reported->showAlbumArtwork);
            expect(!reported->seratoWaveformColours);
            expect(reported->viewMode == PreviewViewMode::spectrogram);
        }
    }

    beginTest("Gain-adjusted metrics synchronize silently and toggle without changing other preferences");
    {
        juce::AudioDeviceManager manager;
        AudioSettingsComponent panel(manager);
        juce::ToggleButton* gainToggle = nullptr;
        juce::ToggleButton* coloursToggle = nullptr;
        const auto& content = *dynamic_cast<juce::Viewport&>(*panel.getChildComponent(0)).getViewedComponent();
        for (auto* child : content.getChildren()) {
            if (auto* toggle = dynamic_cast<juce::ToggleButton*>(child)) {
                if (toggle->getButtonText() == "Gain-adjusted metrics") {
                    gainToggle = toggle;
                } else if (toggle->getButtonText() == "Serato waveform colours") {
                    coloursToggle = toggle;
                }
            }
        }
        expect(gainToggle != nullptr);
        expect(coloursToggle != nullptr);
        if (gainToggle != nullptr && coloursToggle != nullptr) {
            AudioPreviewPreferences canonical {
                .pluginChainEnabled = true,
                .loudnessGraphMinimum = -32.0,
                .loudnessGraphMaximum = -2.0,
                .showAlbumArtwork = false,
                .viewMode = PreviewViewMode::stacked,
                .showSampleOverloads = false,
                .showTruePeakOverloads = false,
                .showOverloadLines = false,
            };
            int callbacks = 0;
            panel.setPreferencesChangedCallback([&](const AudioPreviewPreferences& changed) {
                ++callbacks;
                canonical = changed;
                panel.setPreferences(canonical);
            });
            panel.setPreferences(canonical);
            expectEquals(callbacks, 0);
            expect(gainToggle->getToggleState());
            expect(gainToggle->isEnabled());
            expectEquals(gainToggle->getRadioGroupId(), 0);
            expectEquals(
                gainToggle->getTooltip(),
                juce::String(
                    "Show predicted peaks and estimated loudness after custom gain. "
                    "Loudness assumes unchanged gating. Excludes clipping, limiting, and plugins."
                )
            );
            for (const auto enabled : {false, true}) {
                gainToggle->setToggleState(enabled, juce::dontSendNotification);
                gainToggle->onClick();
                expect(canonical.showGainAdjustedMetrics == enabled);
                expect(canonical.pluginChainEnabled);
                expect(!canonical.showAlbumArtwork);
                expect(canonical.viewMode == PreviewViewMode::stacked);
                expect(!canonical.showSampleOverloads);
                expect(!canonical.showTruePeakOverloads);
                expect(!canonical.showOverloadLines);
                expectEquals(canonical.loudnessGraphMinimum, -32.0);
                expectEquals(canonical.loudnessGraphMaximum, -2.0);
                coloursToggle->setToggleState(false, juce::dontSendNotification);
                coloursToggle->onClick();
                expect(canonical.showGainAdjustedMetrics == enabled);
            }
            expectEquals(callbacks, 4);
        }
    }

    beginTest("Preferences round-trip without changing their unset defaults or auto-starting stopped playback");
    const TemporaryDirectory temporary;
    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    const auto file = temporary.getChildFile("preview-settings.xml");
    {
        juce::PropertiesFile settings(file, options);
        const auto defaults = loadAudioPreviewPreferences(settings);
        expect(!defaults.pluginChainEnabled);
        expect(defaults.latencyCompensated);
        expect(!defaults.continuePlayback);
        expect(!defaults.playFromFirstCuePoint);
        expect(!AudioPreviewPreferences().playFromFirstCuePoint);
        expect(defaults.seratoWaveformColours);
        expect(!defaults.loudnessGraphEnabled);
        expectEquals(defaults.loudnessGraphMinimum, -24.0);
        expectEquals(defaults.loudnessGraphMaximum, -4.0);
        expect(defaults.showAlbumArtwork);
        expect(defaults.showGainAdjustedMetrics);
        expect(AudioPreviewPreferences().showGainAdjustedMetrics);
        settings.setValue("unrelatedSetting", "keep me");
        const AudioPreviewPreferences changed {
            .pluginChainEnabled = true,
            .latencyCompensated = false,
            .continuePlayback = true,
            .playFromFirstCuePoint = true,
            .seratoWaveformColours = false,
            .loudnessGraphEnabled = true,
            .loudnessGraphMinimum = -32.0,
            .loudnessGraphMaximum = -2.0,
            .showAlbumArtwork = false,
            .showGainAdjustedMetrics = false,
        };
        expect(saveAudioPreviewPreferences(settings, changed));
    }
    {
        juce::PropertiesFile settings(file, options);
        const auto saved = loadAudioPreviewPreferences(settings);
        expect(saved.pluginChainEnabled);
        expect(!saved.latencyCompensated);
        expect(saved.continuePlayback);
        expect(saved.playFromFirstCuePoint);
        expect(!saved.seratoWaveformColours);
        expect(!saved.showAlbumArtwork);
        expect(!saved.showGainAdjustedMetrics);
        expect(settings.containsKey("showGainAdjustedMetrics"));
        expectEquals(settings.getValue("unrelatedSetting"), juce::String("keep me"));
        expect(saved.loudnessGraphEnabled);
        expectEquals(saved.loudnessGraphMinimum, -32.0);
        expectEquals(saved.loudnessGraphMaximum, -2.0);
        expect(shouldContinuePreviewPlayback(saved, true, true));
        expect(!shouldContinuePreviewPlayback(saved, false, true));
        expect(!shouldContinuePreviewPlayback(saved, true, false));
        expect(!shouldContinuePreviewPlayback(AudioPreviewPreferences(), true, true));
        auto disabled = saved;
        disabled.playFromFirstCuePoint = false;
        disabled.loudnessGraphEnabled = false;
        disabled.showAlbumArtwork = true;
        disabled.showGainAdjustedMetrics = true;
        expect(saveAudioPreviewPreferences(settings, disabled));
    }
    {
        const juce::PropertiesFile settings(file, options);
        expect(!loadAudioPreviewPreferences(settings).playFromFirstCuePoint);
        const auto disabled = loadAudioPreviewPreferences(settings);
        expect(!disabled.loudnessGraphEnabled);
        expect(disabled.showAlbumArtwork);
        expect(disabled.showGainAdjustedMetrics);
        expectEquals(settings.getValue("unrelatedSetting"), juce::String("keep me"));
        expectEquals(disabled.loudnessGraphMinimum, -32.0);
        expectEquals(disabled.loudnessGraphMaximum, -2.0);
    }

    beginTest("Invalid loudness axis settings are repaired without changing other preferences");
    expect(isValidLoudnessGraphRange(-60.0, 6.0));
    expect(isValidLoudnessGraphRange(5.0, 6.0));
    expect(!isValidLoudnessGraphRange(-4.0, -24.0));
    expect(!isValidLoudnessGraphRange(-4.0, -4.0));
    expect(!isValidLoudnessGraphRange(-4.0, -3.5));
    expect(!isValidLoudnessGraphRange(-24.5, -4.0));
    expect(!isValidLoudnessGraphRange(std::nextafter(-24.0, -23.0), -4.0));
    expect(!isValidLoudnessGraphRange(-24.0, -3.5));
    expect(!isValidLoudnessGraphRange(-61.0, -4.0));
    expect(!isValidLoudnessGraphRange(-24.0, 7.0));
    expect(!isValidLoudnessGraphRange(std::numeric_limits<double>::quiet_NaN(), -4.0));
    expect(!isValidLoudnessGraphRange(-24.0, std::numeric_limits<double>::infinity()));
    for (const auto* invalid : {"nan", "-4junk", "", "-2", "-999", "-61", "-24.5"}) {
        juce::PropertiesFile settings(file, options);
        settings.setValue("loudnessGraphMinimum", invalid);
        settings.setValue("loudnessGraphMaximum", "-4");
        const auto repaired = loadAudioPreviewPreferences(settings);
        expectEquals(repaired.loudnessGraphMinimum, -24.0);
        expectEquals(repaired.loudnessGraphMaximum, -4.0);
        expect(repaired.pluginChainEnabled);
        expect(saveAudioPreviewPreferences(settings, repaired));
        auto rejected = repaired;
        rejected.loudnessGraphMaximum = -40.0;
        expect(!saveAudioPreviewPreferences(settings, rejected));
        expectEquals(loadAudioPreviewPreferences(settings).loudnessGraphMaximum, -4.0);
    }

    beginTest("Integer loudness limits round trip at the supported endpoints");
    {
        juce::PropertiesFile settings(file, options);
        auto endpoints = loadAudioPreviewPreferences(settings);
        endpoints.loudnessGraphMinimum = -60.0;
        endpoints.loudnessGraphMaximum = 6.0;
        expect(saveAudioPreviewPreferences(settings, endpoints));
    }
    {
        juce::PropertiesFile settings(file, options);
        const auto saved = loadAudioPreviewPreferences(settings);
        expectEquals(saved.loudnessGraphMinimum, -60.0);
        expectEquals(saved.loudnessGraphMaximum, 6.0);
        auto fractional = saved;
        fractional.loudnessGraphMaximum = 5.5;
        expect(!saveAudioPreviewPreferences(settings, fractional));
        expectEquals(loadAudioPreviewPreferences(settings).loudnessGraphMaximum, 6.0);

        beginTest("Saved whole-number decimals remain compatible with integer-only limits");
        settings.setValue("loudnessGraphMinimum", "-60.0");
        settings.setValue("loudnessGraphMaximum", "6.0");
        const auto legacy = loadAudioPreviewPreferences(settings);
        expectEquals(legacy.loudnessGraphMinimum, -60.0);
        expectEquals(legacy.loudnessGraphMaximum, 6.0);
        settings.setValue("loudnessGraphMaximum", "7");
        const auto repaired = loadAudioPreviewPreferences(settings);
        expectEquals(repaired.loudnessGraphMinimum, -24.0);
        expectEquals(repaired.loudnessGraphMaximum, -4.0);
    }

    beginTest("Loudness range editing accepts at most two digits and never strips pasted decimals");
    for (const auto* input : {"", "-", "0", "6", "-6", "-60", "60", "-99"}) {
        expect(isValidLoudnessGraphLimitInput(input), input);
    }
    for (const auto* input : {"123", "-123", "006", "6.0", "-2.5", "1-2", "--6", "nan", "6 "}) {
        expect(!isValidLoudnessGraphLimitInput(input), input);
    }
}

/// Keeps all eight overload combinations independent in storage and checkbox synchronization.
/// Unrelated settings edits preserve all three choices.
void PreviewPlayerTests::testOverloadPreferences()
{
    const TemporaryDirectory temporary;
    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    const auto file = temporary.getChildFile("overload-settings.xml");
    const std::array keys {"showSampleOverloads", "showTruePeakOverloads", "showOverloadLines"};
    const std::array labels {"Peak overloads", "True-peak overloads", "Overload vertical lines"};
    const std::array members {
        &AudioPreviewPreferences::showSampleOverloads,
        &AudioPreviewPreferences::showTruePeakOverloads,
        &AudioPreviewPreferences::showOverloadLines,
    };

    beginTest("Absent overload settings and aggregate initialization enable all three independent preferences");
    {
        const juce::PropertiesFile settings(file, options);
        const auto loaded = loadAudioPreviewPreferences(settings);
        expect(AudioPreviewPreferences().showSampleOverloads);
        expect(AudioPreviewPreferences().showTruePeakOverloads);
        expect(AudioPreviewPreferences().showOverloadLines);
        expect(loaded.showSampleOverloads);
        expect(loaded.showTruePeakOverloads);
        expect(loaded.showOverloadLines);
    }

    juce::AudioDeviceManager manager;
    AudioSettingsComponent panel(manager);
    std::array<juce::ToggleButton*, 3> toggles {};
    juce::ComboBox* selector = nullptr;
    const auto& content = *dynamic_cast<juce::Viewport&>(*panel.getChildComponent(0)).getViewedComponent();
    for (auto* child : content.getChildren()) {
        if (auto* toggle = dynamic_cast<juce::ToggleButton*>(child)) {
            const auto* label = std::ranges::find(labels, toggle->getButtonText(), [](const auto* optionText) {
                return juce::String(optionText);
            });
            if (label != labels.end()) {
                toggles[static_cast<std::size_t>(label - labels.begin())] = toggle;
            }
        }
        if (auto* combo = dynamic_cast<juce::ComboBox*>(child)) {
            selector = combo;
        }
    }
    expect(selector != nullptr);
    const auto* missingToggle = std::ranges::find(toggles, nullptr);
    expect(missingToggle == toggles.end());
    if (selector == nullptr || missingToggle != toggles.end()) {
        return;
    }
    panel.setSize(layout::audioSettings::preferredWidth, layout::audioSettings::preferredHeight);
    expectEquals(toggles[0]->getY(), toggles[1]->getY());
    expectEquals(toggles[0]->getRight(), toggles[1]->getX());
    expectEquals(toggles[0]->getBottom(), toggles[2]->getY());
    for (const auto* toggle : toggles) {
        expect(toggle->getWidth() > 0);
        expectEquals(toggle->getHeight(), layout::audioSettings::rowHeight);
    }
    AudioPreviewPreferences canonical;
    int callbacks = 0;
    panel.setPreferencesChangedCallback([&](const AudioPreviewPreferences& changed) {
        ++callbacks;
        canonical = changed;
        panel.setPreferences(canonical);
    });

    beginTest("Every overload combination persists and reflects silently in independent non-radio checkboxes");
    constexpr std::array combinations {
        std::array {false, false, false},
        std::array {false, false, true},
        std::array {false, true, false},
        std::array {false, true, true},
        std::array {true, false, false},
        std::array {true, false, true},
        std::array {true, true, false},
        std::array {true, true, true},
    };
    for (const auto& expected : combinations) {
        canonical = AudioPreviewPreferences {
            .pluginChainEnabled = true,
            .showAlbumArtwork = false,
            .viewMode = PreviewViewMode::spectrogram,
            .showSampleOverloads = expected[0],
            .showTruePeakOverloads = expected[1],
            .showOverloadLines = expected[2],
        };
        {
            juce::PropertiesFile settings(file, options);
            settings.setValue("unrelatedSetting", "keep me");
            expect(saveAudioPreviewPreferences(settings, canonical));
            for (std::size_t index = 0; index < keys.size(); ++index) {
                expect(settings.containsKey(keys[index]));
                expect(settings.getBoolValue(keys[index]) == expected[index]);
            }
        }
        {
            const juce::PropertiesFile settings(file, options);
            const auto loaded = loadAudioPreviewPreferences(settings);
            for (std::size_t index = 0; index < members.size(); ++index) {
                expect(loaded.*members[index] == expected[index]);
            }
            expectEquals(settings.getValue("unrelatedSetting"), juce::String("keep me"));
        }
        const auto beforeSync = callbacks;
        panel.setPreferences(canonical);
        expectEquals(callbacks, beforeSync);
        for (std::size_t index = 0; index < toggles.size(); ++index) {
            expect(toggles[index]->getToggleState() == expected[index]);
            expect(toggles[index]->isEnabled());
            expectEquals(toggles[index]->getRadioGroupId(), 0);
        }
        // Unrelated selector edits must carry all overload choices, including lines with both signals off.
        selector->setSelectedItemIndex(0, juce::dontSendNotification);
        selector->onChange();
        expectEquals(callbacks, beforeSync + 1);
        for (std::size_t index = 0; index < members.size(); ++index) {
            expect(canonical.*members[index] == expected[index]);
        }
        for (std::size_t changed = 0; changed < toggles.size(); ++changed) {
            toggles[changed]->setToggleState(!expected[changed], juce::dontSendNotification);
            toggles[changed]->onClick();
            auto changedExpected = expected;
            changedExpected[changed] = !changedExpected[changed];
            for (std::size_t index = 0; index < members.size(); ++index) {
                expect(canonical.*members[index] == changedExpected[index]);
                expect(toggles[index]->getToggleState() == changedExpected[index]);
            }
            expect(canonical.pluginChainEnabled);
            expect(!canonical.showAlbumArtwork);
            expect(canonical.viewMode == PreviewViewMode::waveform);
            canonical.*members[changed] = expected[changed];
            panel.setPreferences(canonical);
        }
    }
}

/// Verifies stable names, isolated fallback, and the settings selector's round trip through its canonical owner.
void PreviewPlayerTests::testPreviewViewModes()
{
    const std::array modes {
        PreviewViewMode::waveform,
        PreviewViewMode::spectrogram,
        PreviewViewMode::sideBySide,
        PreviewViewMode::stacked,
    };
    const std::array names {"waveform", "spectrogram", "side_by_side", "stacked"};
    const std::array labels {"Waveform", "Spectrogram", "Both: side by side", "Both: stacked"};
    const TemporaryDirectory temporary;
    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    const auto file = temporary.getChildFile("preview-view-settings.xml");

    beginTest("Absent view settings and aggregate initialization default to waveform");
    expect(AudioPreviewPreferences().viewMode == PreviewViewMode::waveform);
    {
        const juce::PropertiesFile settings(file, options);
        expect(loadAudioPreviewPreferences(settings).viewMode == PreviewViewMode::waveform);
    }

    beginTest("Every preview mode has a canonical label and a stable persisted name");
    for (std::size_t index = 0; index < modes.size(); ++index) {
        expectEquals(previewViewModeLabel(modes[index]), juce::String(labels[index]));
        expectEquals(previewViewModeName(modes[index]), juce::String(names[index]));
        expect(previewViewModeFromName(names[index]) == modes[index]);
        {
            juce::PropertiesFile settings(file, options);
            settings.setValue("unrelatedSetting", "keep me");
            const AudioPreviewPreferences preferences {
                .pluginChainEnabled = true,
                .latencyCompensated = false,
                .continuePlayback = true,
                .playFromFirstCuePoint = true,
                .seratoWaveformColours = false,
                .loudnessGraphEnabled = true,
                .loudnessGraphMinimum = -32.0,
                .loudnessGraphMaximum = -2.0,
                .showAlbumArtwork = false,
                .viewMode = modes[index],
            };
            expect(saveAudioPreviewPreferences(settings, preferences));
            expectEquals(settings.getValue("previewViewMode"), juce::String(names[index]));
        }
        const juce::PropertiesFile settings(file, options);
        expect(loadAudioPreviewPreferences(settings).viewMode == modes[index]);
        expectEquals(settings.getValue("unrelatedSetting"), juce::String("keep me"));
    }

    beginTest("Unknown saved names repair only the view mode, preserving other preferences");
    for (const auto* invalid : {"", "unknown", "Spectrogram", "sideBySide", "side_by_side ", "4"}) {
        juce::PropertiesFile settings(file, options);
        settings.setValue("previewViewMode", invalid);
        const auto restored = loadAudioPreviewPreferences(settings);
        expect(restored.viewMode == PreviewViewMode::waveform);
        expect(restored.pluginChainEnabled);
        expect(!restored.latencyCompensated);
        expect(restored.continuePlayback);
        expect(restored.playFromFirstCuePoint);
        expect(!restored.seratoWaveformColours);
        expect(restored.loudnessGraphEnabled);
        expectEquals(restored.loudnessGraphMinimum, -32.0);
        expectEquals(restored.loudnessGraphMaximum, -2.0);
        expect(!restored.showAlbumArtwork);
        expectEquals(settings.getValue("unrelatedSetting"), juce::String("keep me"));
    }

    beginTest("Menu-originated state updates the selector silently and settings changes return a complete snapshot");
    juce::AudioDeviceManager manager;
    AudioSettingsComponent panel(manager);
    juce::ComboBox* selector = nullptr;
    std::function<void(juce::Component&)> findSelector = [&](juce::Component& parent) {
        for (auto* child : parent.getChildren()) {
            if (auto* combo = dynamic_cast<juce::ComboBox*>(child);
                combo != nullptr && combo->getName() == "Preview view")
            {
                selector = combo;
            }
            findSelector(*child);
        }
    };
    findSelector(panel);
    expect(selector != nullptr);
    if (selector == nullptr) {
        return;
    }
    expectEquals(selector->getNumItems(), 4);
    AudioPreviewPreferences canonical {
        .pluginChainEnabled = true,
        .loudnessGraphMinimum = -32.0,
        .loudnessGraphMaximum = -2.0,
        .showAlbumArtwork = false,
    };
    int callbacks = 0;
    panel.setPreferencesChangedCallback([&](const AudioPreviewPreferences& changed) {
        ++callbacks;
        canonical = changed;
        panel.setPreferences(canonical);
    });
    for (std::size_t index = 0; index < modes.size(); ++index) {
        const auto previousCallbacks = callbacks;
        canonical.viewMode = modes[index];
        panel.setPreferences(canonical);
        expectEquals(callbacks, previousCallbacks);
        expectEquals(selector->getText(), juce::String(labels[index]));
        expectEquals(selector->getSelectedItemIndex(), static_cast<int>(index));
        const auto selectedIndex = (index + 1) % modes.size();
        // Invoke synchronously because the lightweight runner has no message dispatch loop.
        selector->setSelectedItemIndex(static_cast<int>(selectedIndex), juce::dontSendNotification);
        selector->onChange();
        expectEquals(callbacks, previousCallbacks + 1);
        expect(canonical.viewMode == modes[selectedIndex]);
        expect(canonical.pluginChainEnabled);
        expect(!canonical.showAlbumArtwork);
        expectEquals(canonical.loudnessGraphMinimum, -32.0);
        expectEquals(canonical.loudnessGraphMaximum, -2.0);
    }
}

/// Verifies settings bounds follow actual controls and reserve an error row only while it contains a message.
void PreviewPlayerTests::testSettingsGeometry()
{
    beginTest("Settings groups share their heading inset and have no empty validation row");
    juce::AudioDeviceManager manager;
    AudioSettingsComponent panel(manager);
    panel.setPreferences(AudioPreviewPreferences {.loudnessGraphEnabled = true});
    panel.setSize(layout::audioSettings::preferredWidth, layout::audioSettings::preferredHeight);
    auto* viewport = dynamic_cast<juce::Viewport*>(panel.getChildComponent(0));
    expect(viewport != nullptr);
    if (viewport == nullptr || viewport->getViewedComponent() == nullptr) {
        return;
    }
    const auto& content = *viewport->getViewedComponent();
    const juce::GroupComponent* deviceGroup = nullptr;
    const juce::GroupComponent* previewGroup = nullptr;
    const juce::AudioDeviceSelectorComponent* deviceSelector = nullptr;
    const juce::ComboBox* previewView = nullptr;
    const juce::ToggleButton* loudnessToggle = nullptr;
    juce::TextEditor* minimumEditor = nullptr;
    const juce::Label* errorLabel = nullptr;
    for (auto* child : content.getChildren()) {
        if (const auto* group = dynamic_cast<const juce::GroupComponent*>(child)) {
            if (group->getName() == "deviceSettings") {
                deviceGroup = group;
            } else if (group->getName() == "previewSettings") {
                previewGroup = group;
            }
        }
        if (const auto* selector = dynamic_cast<const juce::AudioDeviceSelectorComponent*>(child)) {
            deviceSelector = selector;
        }
        if (const auto* combo = dynamic_cast<const juce::ComboBox*>(child)) {
            previewView = combo;
        }
        if (const auto* toggle = dynamic_cast<const juce::ToggleButton*>(child);
            toggle != nullptr && toggle->getButtonText() == "Show loudness graph")
        {
            loudnessToggle = toggle;
        }
        if (auto* editor = dynamic_cast<juce::TextEditor*>(child); editor != nullptr && editor->getText() == "-24") {
            minimumEditor = editor;
        }
        if (const auto* label = dynamic_cast<const juce::Label*>(child); label != nullptr && label->getName().isEmpty())
        {
            errorLabel = label;
        }
    }
    expect(
        deviceGroup != nullptr && previewGroup != nullptr && deviceSelector != nullptr && previewView != nullptr
        && loudnessToggle != nullptr && minimumEditor != nullptr && errorLabel != nullptr
    );
    if (deviceGroup == nullptr || previewGroup == nullptr || deviceSelector == nullptr || previewView == nullptr
        || loudnessToggle == nullptr || minimumEditor == nullptr || errorLabel == nullptr)
    {
        return;
    }
    expectEquals(deviceSelector->getY() - deviceGroup->getY(), layout::audioSettings::GROUP_TOP_INSET);
    expectEquals(deviceGroup->getBottom() - deviceSelector->getBottom(), layout::audioSettings::padding);
    expectEquals(previewGroup->getY() - deviceGroup->getBottom(), layout::audioSettings::SECTION_SPACING);
    expectEquals(
        previewView->getY() - previewGroup->getY(),
        layout::audioSettings::GROUP_TOP_INSET + layout::audioSettings::rangeEditorPadding
    );
    const auto controlLaneWidth
        = previewGroup->getWidth() - layout::audioSettings::padding * 2 - layout::audioSettings::rangeLabelWidth * 2;
    expectEquals(
        previewView->getWidth(),
        juce::roundToInt(controlLaneWidth * layout::audioSettings::PREVIEW_SELECTOR_WIDTH_PROPORTION)
    );
    expect(layout::audioSettings::MINIMUM_WINDOW_HEIGHT < layout::audioSettings::preferredHeight);
    expectEquals(previewGroup->getBottom() - loudnessToggle->getBottom(), layout::audioSettings::padding);
    verifyOverloadSettingsGroup(content, *previewGroup);

    verifyDisabledLoudnessRange(panel, content, *minimumEditor);
    expect(!errorLabel->isVisible());
    expectEquals(errorLabel->getHeight(), 0);
    const auto validHeight = content.getHeight();

    beginTest("Invalid loudness settings add one reachable row and a valid edit reclaims it");
    minimumEditor->setText("6", false);
    minimumEditor->onReturnKey();
    expect(errorLabel->isVisible());
    expect(errorLabel->getText().isNotEmpty());
    expectEquals(content.getHeight(), validHeight + layout::audioSettings::rowHeight);
    expectEquals(errorLabel->getY(), loudnessToggle->getBottom());
    expectEquals(previewGroup->getBottom() - errorLabel->getBottom(), layout::audioSettings::padding);
    minimumEditor->onReturnKey();
    expect(!errorLabel->isVisible());
    expectEquals(content.getHeight(), validHeight);

    beginTest("A shorter settings viewport preserves complete scrollable content");
    panel.setSize(layout::audioSettings::preferredWidth, 180);
    expectEquals(content.getHeight(), validHeight);
    expect(content.getHeight() > viewport->getHeight());
    viewport->setViewPosition(0, content.getHeight());
    expect(viewport->getViewPositionY() > 0);
    expect(
        viewport->getViewPositionY() + viewport->getViewHeight() >= content.getHeight() - layout::audioSettings::padding
    );
}

/// Verifies range editing and publication remain independent of graph visibility.
void PreviewPlayerTests::verifyDisabledLoudnessRange(
    AudioSettingsComponent& panel,
    const juce::Component& content,
    juce::TextEditor& minimumEditor
)
{
    beginTest("Loudness range remains editable while the graph is disabled");
    AudioPreviewPreferences disabledPreferences;
    int rangeCallbacks = 0;
    panel.setPreferencesChangedCallback([&](const auto& preferences) {
        ++rangeCallbacks;
        disabledPreferences = preferences;
        panel.setPreferences(preferences);
    });
    panel.setPreferences(disabledPreferences);
    for (const auto* child : content.getChildren()) {
        if (dynamic_cast<const juce::TextEditor*>(child) != nullptr) {
            expect(child->isEnabled());
        }
        if (const auto* label = dynamic_cast<const juce::Label*>(child);
            label != nullptr && (label->getName() == "loudnessMinimum" || label->getName() == "loudnessMaximum"))
        {
            expect(label->isEnabled());
        }
    }
    minimumEditor.setText("-30", false);
    minimumEditor.onReturnKey();
    expectEquals(rangeCallbacks, 1);
    expectEquals(disabledPreferences.loudnessGraphMinimum, -30.0);
    expect(!disabledPreferences.loudnessGraphEnabled);
    panel.setPreferencesChangedCallback({});
}

/// Verifies all overload controls share a dedicated group separate from playback and loudness preferences.
void PreviewPlayerTests::verifyOverloadSettingsGroup(
    const juce::Component& content,
    const juce::GroupComponent& previewGroup
)
{
    const juce::GroupComponent* overloadGroup = nullptr;
    std::vector<const juce::ToggleButton*> overloadToggles;
    for (const auto* child : content.getChildren()) {
        if (const auto* group = dynamic_cast<const juce::GroupComponent*>(child);
            group != nullptr && group->getName() == "overloadSettings")
        {
            overloadGroup = group;
        }
        if (const auto* toggle = dynamic_cast<const juce::ToggleButton*>(child);
            toggle != nullptr && toggle->getButtonText().containsIgnoreCase("overload"))
        {
            overloadToggles.push_back(toggle);
        }
    }
    expect(overloadGroup != nullptr);
    if (overloadGroup == nullptr) {
        return;
    }
    expectEquals(overloadGroup->getY() - previewGroup.getBottom(), layout::audioSettings::SECTION_SPACING);
    expectEquals(static_cast<int>(overloadToggles.size()), 3);
    for (const auto* toggle : overloadToggles) {
        expect(overloadGroup->getBounds().contains(toggle->getBounds()));
        expect(!previewGroup.getBounds().intersects(toggle->getBounds()));
    }
    expectEquals(content.getHeight() - overloadGroup->getBottom(), layout::audioSettings::padding);
}

/// Keeps lifecycle and foreign cache labels out of the inspector without losing failure detail or operation context.
void PreviewPlayerTests::testInspectorStatusRemoval()
{
    beginTest("Inspector omits lifecycle and cache provenance labels, including cached Rust results");
    AudioInfoPanel panel;
    panel.setSize(480, 600);
    AudioAnalysisRecord record;
    record.fullPath = "/test/track.wav";
    record.fileName = "track.wav";
    record.status = AudioAnalysisStatus::analyzed;
    panel.showRecord(record, {});
    const auto analyzedHeight = panel.getPreferredHeight();
    record.status = AudioAnalysisStatus::cached;
    record.measurementProducer = MeasurementProducer::rust;
    panel.showRecord(record, {});
    expectEquals(panel.getPreferredHeight(), analyzedHeight);
    expectEquals(panel.getDescription(), record.fullPath);
    record.status = AudioAnalysisStatus::pending;
    panel.showRecord(record, {});
    expectEquals(panel.getDescription(), record.fullPath);
    record.status = AudioAnalysisStatus::failed;
    record.errorMessage = "Unable to decode this audio stream";
    panel.showRecord(record, {});
    expectEquals(panel.getDescription(), record.fullPath);
    bool foundError = false;
    std::function<void(const juce::Component&)> inspect = [&](const juce::Component& parent) {
        for (const auto* child : parent.getChildren()) {
            if (const auto* label = dynamic_cast<const juce::Label*>(child)) {
                expect(label->getName() != "Status");
                expect(!label->getText().contains("Cached"));
                expect(label->getText() != "Failed");
                expect(label->getText() != "Analyzed");
                if (label->getName() == "Error") {
                    foundError = label->isVisible() && label->getText() == record.errorMessage;
                }
            }
            inspect(*child);
        }
    };
    inspect(panel);
    expect(foundError);
    panel.showRecord(record, "Writing processed audio");
    expect(panel.getDescription().contains("Writing processed audio"));
}

/// Verifies unlimited tag wrapping, exact preferred sizing, and stable stacked layout without rendering pixels.
void PreviewPlayerTests::testInspectorDetailsLayout()
{
    beginTest("Wrapped inspector text measures its actual layout including newlines and long tokens");
    AudioInfoText wrapped("Album", true);
    expectEquals(wrapped.getPreferredHeight(180), 0);
    const std::array texts {
        juce::String("An album title that continues across many lines without a line limit"),
        juce::String("First line\nSecond line\nThird line\nFourth line"),
        juce::String::repeatedString("LongUnbrokenToken", 30),
        juce::String::fromUTF8("Ääkköset 日本語\nComment with a second line and more words"),
    };
    for (const auto& text : texts) {
        wrapped.setText(text, juce::dontSendNotification);
        for (const auto width : {40, 180, 480}) {
            juce::AttributedString attributed;
            attributed.setJustification(juce::Justification::topLeft);
            attributed.setWordWrap(juce::AttributedString::byChar);
            attributed.append(text, wrapped.getFont(), juce::Colours::white);
            juce::TextLayout textLayout;
            textLayout.createLayout(attributed, static_cast<float>(width));
            const auto expectedHeight = juce::jmax(
                layout::information::rowHeight,
                juce::roundToInt(std::ceil(textLayout.getHeight())) + layout::information::DETAIL_TEXT_PADDING * 2
            );
            expectEquals(wrapped.getPreferredHeight(width), expectedHeight);
            expect(wrapped.getPreferredHeight(width) >= wrapped.getPreferredHeight(480));
            expectEquals(wrapped.getText(), text);
        }
    }
    wrapped.setText(texts[2], juce::dontSendNotification);
    expect(wrapped.getPreferredHeight(40) > layout::information::rowHeight * 3);
    expect(wrapped.getPreferredHeight(40) > wrapped.getPreferredHeight(480));
    expectEquals(wrapped.getPreferredHeight(0), wrapped.getPreferredHeight(1));
    AudioInfoText singleLine("Genre", false);
    singleLine.setText(texts[2], juce::dontSendNotification);
    expectEquals(singleLine.getPreferredHeight(40), layout::information::rowHeight);
    expectEquals(singleLine.getPreferredHeight(480), layout::information::rowHeight);

    beginTest("Single-line values retain full labelled tooltips while narrow bounds clip their visible text");
    singleLine.setValues({
        {.label = "Format", .value = "FLAC"},
        {.label = "Channels", .value = "Stereo"},
        {.label = "Duration", .value = "3:25.4"},
        {.label = "Size", .value = "36.2 MB"},
    });
    singleLine.setSize(20, layout::information::rowHeight);
    expectEquals(singleLine.getText(), juce::String::fromUTF8("FLAC · Stereo · 3:25.4 · 36.2 MB"));
    expectEquals(
        singleLine.getTooltip(), juce::String("Format: FLAC, Channels: Stereo, Duration: 3:25.4, Size: 36.2 MB")
    );
    singleLine.setValues({{.label = "Key", .value = "Gm / 6A"}});
    expectEquals(singleLine.getText(), juce::String("Gm / 6A"));
    expectEquals(singleLine.getTooltip(), juce::String("Key: Gm / 6A"));
    singleLine.setValues({});
    expect(singleLine.getText().isEmpty());
    expect(singleLine.getTooltip().isEmpty());

    beginTest("Preferred inspector height fits complete tag layouts with the same stacked lines in both modes");
    AudioInfoPanel panel;
    panel.setSize(260, 240);
    AudioAnalysisRecord record;
    record.file = juce::File("/test/details.flac");
    record.fileName = "Artist - Title";
    record.fullPath = record.file.getFullPathName();
    record.status = AudioAnalysisStatus::analyzed;
    record.channels = 2;
    record.sampleRate = 44'100;
    record.bitsPerSample = 24;
    record.durationSeconds = 205.4;
    record.fileSize = 36'200'000;
    serato::SeratoData data;
    data.trackInfo = makeTrackInfo(128.0, "Gm");
    data.trackInfo->genre = texts[0];
    data.trackInfo->album = texts[2];
    data.trackInfo->comment = texts[1];
    panel.showRecord(record, {}, &data);
    const juce::Viewport* viewport = nullptr;
    for (const auto* child : panel.getChildren()) {
        if (const auto* candidate = dynamic_cast<const juce::Viewport*>(child)) {
            viewport = candidate;
        }
    }
    expect(viewport != nullptr && viewport->getViewedComponent() != nullptr);
    if (viewport == nullptr || viewport->getViewedComponent() == nullptr) {
        return;
    }
    auto& content = *viewport->getViewedComponent();
    const auto cellNamed = [&](const juce::String& name) -> const juce::Label* {
        for (const auto* child : content.getChildren()) {
            if (const auto* label = dynamic_cast<const juce::Label*>(child);
                label != nullptr && label->getName() == name)
            {
                return label;
            }
        }
        return nullptr;
    };
    const auto* format = cellNamed("Format");
    const auto* audio = cellNamed("Bit depth");
    const auto* track = cellNamed("BPM");
    const auto* genre = dynamic_cast<const AudioInfoText*>(cellNamed("Genre"));
    const auto* album = dynamic_cast<const AudioInfoText*>(cellNamed("Album"));
    const auto* comment = dynamic_cast<const AudioInfoText*>(cellNamed("Comment"));
    const auto* error = dynamic_cast<const AudioInfoText*>(cellNamed("Error"));
    expect(
        format != nullptr && audio != nullptr && track != nullptr && genre != nullptr && album != nullptr
        && comment != nullptr && error != nullptr
    );
    if (format == nullptr || audio == nullptr || track == nullptr || genre == nullptr || album == nullptr
        || comment == nullptr || error == nullptr)
    {
        return;
    }
    const auto verifyLayout = [&] {
        expectEquals(format->getX(), 0);
        expectEquals(audio->getX(), format->getX());
        expectEquals(audio->getWidth(), format->getWidth());
        expectEquals(format->getY(), audio->getBottom());
        expectEquals(track->getY(), format->getY());
        const auto* bitrate = cellNamed("Bitrate");
        expect(bitrate != nullptr);
        if (bitrate != nullptr) {
            expectEquals(track->getRight(), bitrate->getRight());
        }
        expectEquals(genre->getY(), format->getBottom());
        expectEquals(album->getY(), genre->getBottom());
        expectEquals(comment->getY(), album->getBottom());
        expectEquals(genre->getHeight(), layout::information::rowHeight);
        expectEquals(album->getHeight(), album->getPreferredHeight(album->getWidth()));
        expectEquals(comment->getHeight(), comment->getPreferredHeight(comment->getWidth()));
        expectEquals(album->getX(), layout::information::detailFieldWidth);
        expectEquals(album->getRight(), content.getWidth());
        expectEquals(content.getHeight(), comment->getBottom());
        expectEquals(genre->getDescription(), juce::String("Genre: ") + texts[0]);
    };
    verifyLayout();
    const auto narrowHeight = panel.getPreferredHeight();
    panel.setWide(true);
    expectEquals(panel.getPreferredHeight(), narrowHeight - layout::information::metricHeight - layout::sectionSpacing);
    verifyLayout();
    panel.setWide(false);
    panel.setSize(260, panel.getPreferredHeight());
    expectEquals(content.getHeight(), viewport->getHeight());
    verifyLayout();
    panel.setSize(540, 240);
    expect(panel.getPreferredHeight() < narrowHeight);
    verifyLayout();

    beginTest("Pending, failed, cleared, and replacement selections never retain stale technical details or tags");
    record.status = AudioAnalysisStatus::pending;
    panel.showRecord(record, "Analyzing", &data);
    expect(!format->isVisible() && !audio->isVisible());
    expect(track->isVisible() && album->isVisible() && comment->isVisible());
    const auto pendingHeight = panel.getPreferredHeight();
    panel.showRecord(record, "Waiting", &data);
    expectEquals(panel.getPreferredHeight(), pendingHeight);
    record.status = AudioAnalysisStatus::failed;
    record.errorMessage = texts[2];
    panel.showRecord(record, {}, &data);
    expect(error->isVisible());
    expectEquals(error->getText(), record.errorMessage);
    expectEquals(error->getY(), 0);
    expectEquals(track->getY(), error->getBottom());
    expectEquals(
        panel.getPreferredHeight(),
        pendingHeight + error->getPreferredHeight(error->getWidth()) + layout::information::verticalPadding
    );
    panel.clear();
    expect(!format->isVisible() && !audio->isVisible() && !track->isVisible());
    expect(!genre->isVisible() && !album->isVisible() && !comment->isVisible() && !error->isVisible());
    expectEquals(viewport->getViewPositionY(), 0);
    const auto emptyHeight = panel.getPreferredHeight();
    panel.clear();
    expectEquals(panel.getPreferredHeight(), emptyHeight);
}

/// Creates the device-free preview player suite for explicit test registration.
std::unique_ptr<juce::UnitTest> createPreviewPlayerTests()
{
    return std::make_unique<PreviewPlayerTests>();
}
}  // namespace audiobatch::tests
