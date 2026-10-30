/// Device-free tests for the preview player around the renderer:
/// the cue start position, the deferred initial position, the ownership of the simulated output device,
/// the output device status shown in the status bar, the persisted preview preferences, and the loudness axis ticks.
/// The committed silence fixture exercises playback without opening hardware or third-party plugins.

#include "gui/AudioBatchComponentState.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/components/AudioSettingsComponent.h"
#include "gui/components/StatusBar.h"
#include "gui/GuiLayout.h"
#include "serato/SeratoTagTypes.h"
#include "TestHelpers.h"
#include "TestSuites.h"

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace audiobatch::tests
{
/// Covers the preview player, its device and preference policy, and the waveform axis, not GUI rendering.
class PreviewPlayerTests final : public juce::UnitTest
{
public:
    /// Registers a lightweight suite with the existing factory-driven runner.
    PreviewPlayerTests() : juce::UnitTest("Preview player", "AudioBatch") {}

    /// Runs the device-independent player and persistence checks.
    void runTest() override
    {
        testCueStartPosition();
        testInitialPosition();
        testDeviceOwnership();
        testAsioDeviceSelection();
        testSettingsDeviceFailure();
        testOutputStatusFormatting();
        testPreferences();
        testLoudnessAxisIntervals();
        testPreviewFitHeight();
    }

private:
    /// Checks that the startup preview height fits the controls of the layout that the workspace width selects.
    void testPreviewFitHeight()
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

    /// Checks numeric tick layout without constructing or rendering a GUI component.
    void testLoudnessAxisIntervals()
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
    void testCueStartPosition()
    {
        const AudioPreviewPreferences enabled {.playFromFirstCuePoint = true};
        serato::Markers markers;

        beginTest("Cue starts require an enabled preference and nonempty markers");
        expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, nullptr, 10.0), 0.0, 0.000001);
        expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 0.0, 0.000001);
        markers.cues.push_back({.positionMilliseconds = 1250, .index = 7, .colour = {}, .name = {}});
        expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 1.25, 0.000001);
        expectWithinAbsoluteError(
            getPreviewStartPositionSeconds(AudioPreviewPreferences(), &markers, 10.0), 0.0, 0.000001
        );

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
    void testInitialPosition()
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
    void testDeviceOwnership()
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
    void testAsioDeviceSelection()
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
        expectEquals(
            device.attemptedDevices.joinIntoString(", "), juce::String("Disconnected default, Connected output")
        );
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
    void testSettingsDeviceFailure()
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

    /// Checks the status bar text of the output device for fractional rates and values that are not known yet.
    void testOutputStatusFormatting()
    {
        beginTest("Output status text leaves out unknown values and keeps fractional sample rates");
        expectEquals(
            formatAudioOutputStatus(
                {.deviceName = "Speakers", .sampleRate = 44'100.0, .bufferSize = 256, .open = true}
            ),
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

    /// Uses an isolated settings file to verify unset defaults, persistence, and actual continuation policy.
    void testPreferences()
    {
        beginTest("Audio Settings changes keep the menu-only album artwork choice");
        {
            juce::AudioDeviceManager manager;
            AudioSettingsComponent settingsComponent(manager);
            AudioPreviewPreferences shown;
            shown.showAlbumArtwork = false;
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
            expect(saveAudioPreviewPreferences(settings, disabled));
        }
        {
            const juce::PropertiesFile settings(file, options);
            expect(!loadAudioPreviewPreferences(settings).playFromFirstCuePoint);
            const auto disabled = loadAudioPreviewPreferences(settings);
            expect(!disabled.loudnessGraphEnabled);
            expect(disabled.showAlbumArtwork);
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
};

/// Creates the device-free preview player suite for explicit test registration.
std::unique_ptr<juce::UnitTest> createPreviewPlayerTests()
{
    return std::make_unique<PreviewPlayerTests>();
}
}  // namespace audiobatch::tests
