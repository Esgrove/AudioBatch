/// Audio playback for the GUI file preview.
/// Declares AudioPreviewPlayer, which owns the audio device, the transport, and the read-ahead thread
/// that play the selected file, and keeps their setup and teardown in the required order.

#pragma once

#include <JuceHeader.h>

#include <memory>
#include <optional>

namespace audiobatch
{
/// Plays one audio file at a time through the default output device.
/// Must be created and used on the message thread.
/// Components that hold references to its format manager,
/// transport or device manager must be destroyed before it.
class AudioPreviewPlayer
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
    void togglePlayback();

    [[nodiscard]] bool hasLoadedSource() const noexcept;
    [[nodiscard]] bool isPlaying() const;
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

    /// Returns the transport, which the waveform follows and which broadcasts playback state changes.
    [[nodiscard]] juce::AudioTransportSource& getTransport() noexcept
    {
        return transportSource;
    }

private:
    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    juce::AudioSourcePlayer sourcePlayer;
    juce::AudioTransportSource transportSource;

    /// Reader source of the loaded file, or nullptr when nothing is loaded.
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;

    /// Reads ahead from the file so playback does not decode on the audio thread.
    juce::TimeSliceThread readAheadThread {"audio file preview"};

    JUCE_LEAK_DETECTOR(AudioPreviewPlayer)
};
}  // namespace audiobatch
