/// Implementation of the preview player:
/// audio device initialisation after the permission request, loading a file into the transport,
/// playback control, and the shutdown order of the transport, audio callback, and read-ahead thread.

#include "gui/AudioPreviewPlayer.h"

#include "utils/AudioFormatUtils.h"
#include "utils/Log.h"

namespace audiobatch
{
namespace
{
/// Number of samples the transport buffers ahead of the playback position on the read-ahead thread.
constexpr int readAheadBufferSamples = 32768;
/// Time in milliseconds to wait for the read-ahead thread to stop during shutdown.
constexpr int readAheadThreadStopTimeoutMilliseconds = 2000;
}  // namespace

/// Registers the audio formats, starts the read-ahead thread, and connects the transport to the audio callback.
/// The device itself is opened later by requestDeviceAccess.
AudioPreviewPlayer::AudioPreviewPlayer()
{
    formatManager.registerBasicFormats();
    readAheadThread.startThread(juce::Thread::Priority::high);
    deviceManager.addAudioCallback(&sourcePlayer);
    sourcePlayer.setSource(&transportSource);
}

/// Stops playback and disconnects the transport before the audio callback and the read-ahead thread go away.
AudioPreviewPlayer::~AudioPreviewPlayer()
{
    utils::logDebug("Application shutdown: stopping audio preview transport");
    release();
    sourcePlayer.setSource(nullptr);
    deviceManager.removeAudioCallback(&sourcePlayer);
    utils::logDebug("Application shutdown: stopping audio preview read-ahead thread");
    readAheadThread.stopThread(readAheadThreadStopTimeoutMilliseconds);
    utils::logDebug("Application shutdown: releasing audio device manager");
}

/// Requests audio input permission and initialises the device manager once it is answered.
/// Input channels are only opened when the permission was granted.
void AudioPreviewPlayer::requestDeviceAccess()
{
    utils::logDebug("Audio preview: requesting record-audio permission");
    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, [this](const bool granted) {
        const int numInputChannels = granted ? 2 : 0;
        const auto initialiseStartedAtMs = juce::Time::getMillisecondCounterHiRes();

        utils::logDebug("Audio preview: permission callback granted={}", granted ? "true" : "false");
        utils::logDebug("Audio preview: device init begin (inputs={}, outputs=2)", numInputChannels);

        const auto initialiseError = deviceManager.initialise(numInputChannels, 2, nullptr, true, {}, nullptr);
        const auto initialiseElapsedMs = juce::Time::getMillisecondCounterHiRes() - initialiseStartedAtMs;

        if (initialiseError.isEmpty()) {
            utils::logDebug("Audio preview: device init succeeded ({:.1f} ms)", initialiseElapsedMs);
        } else {
            utils::logError("Audio preview: device init failed ({:.1f} ms): {}", initialiseElapsedMs, initialiseError);
        }
    });
}

/// Creates a reader for the URL and attaches it to the transport, replacing any previous source.
/// Returns false when no reader could be created, leaving the transport empty.
bool AudioPreviewPlayer::load(const juce::URL& audioUrl)
{
    release();

    auto* reader = audioUrl.isLocalFile()
        ? utils::createAudioReader(audioUrl.getLocalFile()).release()
        : formatManager.createReaderFor(
              audioUrl.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress))
          );

    if (reader == nullptr) {
        return false;
    }

    readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    transportSource.setSource(readerSource.get(), readAheadBufferSamples, &readAheadThread, reader->sampleRate);
    return true;
}

/// Stops playback and closes the loaded file, so the file on disk can be replaced.
void AudioPreviewPlayer::release()
{
    transportSource.stop();
    transportSource.setSource(nullptr);
    readerSource.reset();
}

/// Stops playback and keeps the loaded file.
void AudioPreviewPlayer::stop()
{
    transportSource.stop();
}

/// Starts playback of the loaded file, or stops it when it is already playing.
void AudioPreviewPlayer::togglePlayback()
{
    if (transportSource.isPlaying()) {
        transportSource.stop();
    } else {
        transportSource.start();
    }
}

/// True when a file is loaded into the transport.
bool AudioPreviewPlayer::hasLoadedSource() const noexcept
{
    return readerSource != nullptr;
}

/// True while the transport is playing.
bool AudioPreviewPlayer::isPlaying() const
{
    return transportSource.isPlaying();
}

/// Returns the length of the loaded file in seconds, or nothing when no file is loaded.
/// A reader without a valid sample rate reports a length of zero.
std::optional<double> AudioPreviewPlayer::getLoadedDurationSeconds() const
{
    if (readerSource == nullptr) {
        return std::nullopt;
    }

    const auto* reader = readerSource->getAudioFormatReader();
    return reader->sampleRate > 0.0 ? static_cast<double>(reader->lengthInSamples) / reader->sampleRate : 0.0;
}
}  // namespace audiobatch
