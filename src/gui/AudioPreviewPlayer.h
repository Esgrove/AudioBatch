/// Audio playback for the GUI file preview.
/// Declares AudioPreviewPlayer, which owns the audio device, the live renderer, and the read-ahead thread
/// that play the selected file, and keeps their setup and teardown in the required order.

#pragma once

#include "plugins/PluginPreviewSource.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace audiobatch
{
class PluginChain;

/// Plays one audio file at a time through the default output device.
/// Must be created and used on the message thread.
/// Components that hold references to its format manager,
/// playback controller or device manager must be destroyed before it.
class AudioPreviewPlayer :
    public juce::ChangeBroadcaster,
    private juce::AudioIODeviceCallback,
    private juce::AsyncUpdater
{
public:
    AudioPreviewPlayer();

    ~AudioPreviewPlayer();

    AudioPreviewPlayer(const AudioPreviewPlayer&) = delete;
    AudioPreviewPlayer& operator=(const AudioPreviewPlayer&) = delete;
    AudioPreviewPlayer(AudioPreviewPlayer&&) = delete;
    AudioPreviewPlayer& operator=(AudioPreviewPlayer&&) = delete;

    bool load(const juce::URL& audioUrl);

    void release();
    void requestDeviceAccess();
    void stop();
    void start();
    void setPosition(double seconds);
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

    /// Delivers errors on the message thread, never from audio or read-ahead callbacks.
    std::function<void(const juce::String&)> onError;

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
    void prepareInput();
    bool reposition(double seconds, bool resume);
    void reportError(const juce::String& message);

    mutable juce::CriticalSection renderingLock;
    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    PluginPreviewSource previewSource;
    PluginChain* pluginChain = nullptr;

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
    double sampleRate = 48000.0;
    int blockSize = 512;
    bool pluginPreviewEnabled = false;
    bool latencyCompensated = true;
    bool lastReportedPlaying = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE(AudioPreviewPlayer)
    JUCE_LEAK_DETECTOR(AudioPreviewPlayer)
};
}  // namespace audiobatch
