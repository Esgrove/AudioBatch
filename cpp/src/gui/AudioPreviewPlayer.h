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
class LiveChain;

/// Name and configuration of the preview output device, shown in the status bar.
/// A value that is not known yet is empty or zero.
struct AudioOutputStatus {
    /// Name of the output device.
    juce::String deviceName;
    /// Sample rate of the device in hertz.
    double sampleRate {};
    /// Buffer size of the device in samples.
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
    void setLiveChain(LiveChain* chain);
    bool setPluginPreviewEnabled(bool enabled);
    void setLatencyCompensated(bool enabled);
    void setInputGain(LinearGain gain, bool immediately = false);
    void setSavedDeviceState(std::unique_ptr<juce::XmlElement> state);
    [[nodiscard]] std::unique_ptr<juce::XmlElement> createDeviceState() const;
    bool refreshPluginChain();
    void togglePlayback();
    void updatePlaybackStatus();

    [[nodiscard]] bool hasLoadedSource() const noexcept;
    [[nodiscard]] bool isPlaying() const;
    [[nodiscard]] bool isStopping() const;
    [[nodiscard]] bool isPluginPreviewEnabled() const noexcept;
    [[nodiscard]] double getCurrentPosition() const;
    [[nodiscard]] std::optional<double> getLoadedDurationSeconds() const;
    [[nodiscard]] std::optional<juce::int64> getLoadedLengthInSamples() const;
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

    /// Serializes the message thread's changes to the sources and the chain with the audio callback,
    /// which only try-locks it.
    /// The live chain shares it.
    mutable juce::CriticalSection renderingLock;
    /// The output device, opened only while something plays or the audio settings need it.
    juce::AudioDeviceManager deviceManager;
    /// Creates the readers of remote preview files, and of the waveform thumbnail.
    juce::AudioFormatManager formatManager;
    /// Renders the file through the live chain at the device rate.
    PluginPreviewSource previewSource;
    /// The slot owner, which can be destroyed or detached independently of the player.
    juce::WeakReference<LiveChain> liveChain;

    /// Reader source of the loaded file, or nullptr when nothing is loaded.
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    /// Buffers the decoded file ahead of playback on the read-ahead thread, or null when nothing is loaded.
    std::unique_ptr<juce::BufferingAudioSource> bufferingSource;
    /// Converts the buffered file to the device rate, or null when nothing is loaded.
    std::unique_ptr<juce::ResamplingAudioSource> resamplingSource;

    /// Reads ahead from the file so playback does not decode on the audio thread.
    juce::TimeSliceThread readAheadThread {"audio file preview"};
    /// True once the input and the chain are prepared for the open device, so the callback may render.
    std::atomic<bool> deviceReady {false};
    /// Set by the read-ahead thread when decoding the file fails.
    std::atomic<bool> readFailed {false};
    /// Set by the audio callback when the buffered input ran out.
    std::atomic<bool> bufferUnderrun {false};
    /// Sample rate of the device that is starting, until the message thread prepares for it.
    std::atomic<double> pendingRate {0.0};
    /// Block size of the device that is starting, until the message thread prepares for it.
    std::atomic<int> pendingBlockSize {512};

    /// Playback state of the preview source, published under the rendering lock after every change
    /// and after every rendered block, so the message thread reads it without taking the lock.
    /// The audio callback only try-locks, and outputs silence for every block in which the lock is busy.
    std::atomic<double> publishedPosition {0.0};
    std::atomic<bool> publishedPlaying {false};
    std::atomic<bool> publishedStopping {false};
    std::atomic<bool> publishedFailed {false};

    /// Device state saved by an earlier session, used and cleared by the first open.
    std::unique_ptr<juce::XmlElement> savedDeviceState;

    /// Tail length last applied to the preview source, so an unchanged tail does not take the rendering lock.
    juce::int64 appliedTailSamples {-1};
    /// Sample rate the input and the chain are prepared for.
    double sampleRate {utils::defaultSampleRate};
    /// Block size the input and the chain are prepared for.
    int blockSize {512};
    /// True while the preview plays through the plugin chain.
    bool pluginPreviewEnabled {false};
    /// True to render the latency pre-roll, so the audio lines up with the waveform position.
    bool latencyCompensated {true};
    /// The playing state the listeners were last told about.
    bool lastReportedPlaying {false};
    /// False while the application is in the background, where it gives up the device.
    bool applicationActive {true};
    /// True while the audio settings dialog is shown, which keeps the device open.
    bool deviceSettingsActive {false};

    /// Holds a new file silent until its asynchronous cue metadata determines the initial position.
    /// Explicit seeks cancel the wait, and start/stop retain the user's latest playback intent.
    bool waitingForInitialPosition {false};
    bool startWhenInitialPositionReady {false};

    JUCE_DECLARE_WEAK_REFERENCEABLE(AudioPreviewPlayer)
    JUCE_LEAK_DETECTOR(AudioPreviewPlayer)
};
}  // namespace audiobatch
