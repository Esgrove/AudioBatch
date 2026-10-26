/// Audio playback for the GUI file preview.
/// Declares AudioPreviewPlayer, which owns the audio device, the live renderer, and the read-ahead thread
/// that play the selected file, and keeps their setup and teardown in the required order.

#pragma once

#include "plugins/PluginPreviewSource.h"
#include "utils/AudioFormatUtils.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace audiobatch
{
class PluginChain;

/// Name and configuration of the preview output device, shown in the status bar.
/// A value that is not known yet is empty or zero.
struct AudioOutputStatus {
    juce::String deviceName;
    double sampleRate {};
    int bufferSize {};
    /// True while the device is open, which is only during preview playback or while the audio settings are shown.
    bool open {};
};

/// Plays one audio file at a time through the default output device.
/// Must be created and used on the message thread.
/// Components that hold references to its format manager,
/// playback controller or device manager must be destroyed before it.
class AudioPreviewPlayer :
    public juce::ChangeBroadcaster,
    private juce::AudioIODeviceCallback,
    private juce::AsyncUpdater,
    private juce::Timer
{
public:
    AudioPreviewPlayer();

    ~AudioPreviewPlayer() override;

    AudioPreviewPlayer(const AudioPreviewPlayer&) = delete;
    AudioPreviewPlayer& operator=(const AudioPreviewPlayer&) = delete;
    AudioPreviewPlayer(AudioPreviewPlayer&&) = delete;
    AudioPreviewPlayer& operator=(AudioPreviewPlayer&&) = delete;

    [[nodiscard]] bool load(const juce::URL& audioUrl, bool waitForInitialPosition = false);

    void release();
    void suspend();
    void setApplicationActive(bool active);
    void setDeviceSettingsActive(bool active);
    void stop();
    void start();
    void setPosition(double seconds);
    void completeInitialPosition(double seconds);
    void setErrorCallback(std::function<void(const juce::String&)> callback);
    void setPluginChain(PluginChain* chain);
    bool setPluginPreviewEnabled(bool enabled);
    void setLatencyCompensated(bool enabled);
    void setInputGain(float linearGain, bool immediately = false);
    bool refreshPluginChain();
    void togglePlayback();
    void updatePlaybackStatus();

    [[nodiscard]] bool hasLoadedSource() const noexcept;
    [[nodiscard]] bool isPlaying() const;
    [[nodiscard]] bool isPluginPreviewEnabled() const noexcept;
    [[nodiscard]] double getCurrentPosition() const;
    [[nodiscard]] std::optional<double> getLoadedDurationSeconds() const;
    [[nodiscard]] AudioOutputStatus getOutputStatus() const;

    /// Returns the device manager, used by the audio settings dialog.
    [[nodiscard]] juce::AudioDeviceManager& getDeviceManager() noexcept
    {
        return deviceManager;
    }

    /// Returns the format manager that creates the preview readers, also used by the waveform thumbnail.
    [[nodiscard]] juce::AudioFormatManager& getFormatManager() noexcept
    {
        return formatManager;
    }

    /// Serializes message-thread plugin lifecycle work with the non-blocking device callback.
    [[nodiscard]] juce::CriticalSection& getRenderingLock() noexcept
    {
        return renderingLock;
    }

private:
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannels,
        int inputChannelCount,
        float* const* outputChannels,
        int outputChannelCount,
        int sampleCount,
        const juce::AudioIODeviceCallbackContext& context
    ) override;
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void closeAudioDevice();
    void prepareInput();
    void publishState();
    bool openAudioDevice();
    bool reposition(double seconds, bool resume);
    void reportError(const juce::String& message) const;

    /// Delivers errors on the message thread, never from audio or read-ahead callbacks.
    std::function<void(const juce::String&)> onError;

    mutable juce::CriticalSection renderingLock;
    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    PluginPreviewSource previewSource;
    /// The slot owner, which can be destroyed or detached independently of the player.
    juce::WeakReference<PluginChain> pluginChain;

    /// Reader source of the loaded file, or nullptr when nothing is loaded.
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    std::unique_ptr<juce::BufferingAudioSource> bufferingSource;
    std::unique_ptr<juce::ResamplingAudioSource> resamplingSource;

    /// Reads ahead from the file so playback does not decode on the audio thread.
    juce::TimeSliceThread readAheadThread {"audio file preview"};
    std::atomic<bool> deviceReady {false};
    std::atomic<bool> readFailed {false};
    std::atomic<bool> bufferUnderrun {false};
    std::atomic<double> pendingRate {0.0};
    std::atomic<int> pendingBlockSize {512};

    /// Playback state of the preview source, published under the rendering lock after every change
    /// and after every rendered block, so the message thread reads it without taking the lock.
    /// The audio callback only try-locks, and outputs silence for every block in which the lock is busy.
    std::atomic<double> publishedPosition {0.0};
    std::atomic<bool> publishedPlaying {false};
    std::atomic<bool> publishedStopping {false};
    std::atomic<bool> publishedFailed {false};

    /// Tail length last applied to the preview source, so an unchanged tail does not take the rendering lock.
    juce::int64 appliedTailSamples {-1};
    double sampleRate {utils::defaultSampleRate};
    int blockSize {512};
    bool pluginPreviewEnabled {false};
    bool latencyCompensated {true};
    bool lastReportedPlaying {false};
    bool applicationActive {true};
    bool deviceSettingsActive {false};

    /// Holds a new file silent until its asynchronous cue metadata determines the initial position.
    /// Explicit seeks cancel the wait, and start/stop retain the user's latest playback intent.
    bool waitingForInitialPosition {false};
    bool startWhenInitialPositionReady {false};

    JUCE_DECLARE_WEAK_REFERENCEABLE(AudioPreviewPlayer)
    JUCE_LEAK_DETECTOR(AudioPreviewPlayer)
};
}  // namespace audiobatch
