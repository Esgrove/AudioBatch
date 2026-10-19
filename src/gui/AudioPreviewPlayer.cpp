/// Message-thread preview control and a non-blocking audio-device rendering callback.
/// File decoding runs on the read-ahead thread, live processors share their editor instances,
/// and all seeks and device changes preserve the processing source's audible clock.

#include "gui/AudioPreviewPlayer.h"

#include "plugins/PluginChain.h"
#include "utils/AudioFormatUtils.h"
#include "utils/Log.h"

#include <array>
#include <span>
#include <utility>

namespace audiobatch
{
namespace
{
constexpr int readAheadBufferSamples = 32768;
constexpr int readAheadThreadStopTimeoutMilliseconds = 2000;
constexpr juce::uint32 seekBufferTimeoutMilliseconds = 2000;

/// Records read failures on the decoding thread rather than silently turning them into successful playback.
class PreviewReaderSource final : public juce::AudioFormatReaderSource
{
public:
    /// Transfers reader ownership to JUCE and retains the host's thread-safe error flag.
    PreviewReaderSource(juce::AudioFormatReader* audioReader, std::atomic<bool>& failure) :
        juce::AudioFormatReaderSource(audioReader, true),
        readFailure(failure)
    {}

    /// Reads only available source samples and pads EOF, preserving the buffering source's position contract.
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        auto* audioReader = getAudioFormatReader();
        const auto position = getNextReadPosition();
        const auto samples
            = static_cast<int>(juce::jlimit<juce::int64>(0, info.numSamples, audioReader->lengthInSamples - position));
        info.clearActiveBufferRegion();
        if (samples > 0 && !audioReader->read(info.buffer, info.startSample, samples, position, true, true)) {
            readFailure.store(true);
            info.clearActiveBufferRegion();
        }
        setNextReadPosition(position + info.numSamples);
    }

private:
    std::atomic<bool>& readFailure;
};

/// Detects a total cache miss so the host can stop with an explicit error rather than advance through silent audio.
class PreviewBufferingSource final : public juce::BufferingAudioSource
{
public:
    /// Keeps all decoding on the existing time-slice thread and pre-fills before playback starts.
    PreviewBufferingSource(
        juce::PositionableAudioSource* source,
        juce::TimeSliceThread& thread,
        const int bufferSamples,
        std::atomic<bool>& underrun
    ) :
        juce::BufferingAudioSource(source, thread, false, bufferSamples, 2, true),
        underrunFlag(underrun)
    {}

    /// Observes whether JUCE could advance its buffered input cursor.
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        const auto before = getNextReadPosition();
        juce::BufferingAudioSource::getNextAudioBlock(info);
        if (info.numSamples > 0 && getNextReadPosition() == before) {
            underrunFlag.store(true);
        }
    }

private:
    std::atomic<bool>& underrunFlag;
};
}  // namespace

/// Creates a device-free player first. Opening the device is deferred until permission handling completes.
AudioPreviewPlayer::AudioPreviewPlayer()
{
    formatManager.registerBasicFormats();
    readAheadThread.startThread(juce::Thread::Priority::high);
    deviceManager.addAudioCallback(this);
}

/// Disconnects callbacks before destroying file buffers or the read-ahead thread.
AudioPreviewPlayer::~AudioPreviewPlayer()
{
    cancelPendingUpdate();
    deviceManager.removeAudioCallback(this);
    release();
    pluginChain = nullptr;
    readAheadThread.stopThread(readAheadThreadStopTimeoutMilliseconds);
}

/// Requests permission without retaining a dangling player if the application closes during the request.
void AudioPreviewPlayer::requestDeviceAccess()
{
    const juce::WeakReference safeThis(this);
    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, [safeThis](const bool granted) {
        if (safeThis == nullptr) {
            return;
        }
        const auto error = safeThis->deviceManager.initialise(granted ? 2 : 0, 2, nullptr, true, {}, nullptr);
        if (error.isNotEmpty()) {
            safeThis->reportError(error);
        }
    });
}

/// Loads a mono/stereo source without starting it or altering the file on disk.
bool AudioPreviewPlayer::load(const juce::URL& audioUrl)
{
    const juce::ScopedLock lock(renderingLock);
    release();
    auto reader = audioUrl.isLocalFile()
        ? utils::createAudioReader(audioUrl.getLocalFile())
        : std::unique_ptr<juce::AudioFormatReader>(formatManager.createReaderFor(
              audioUrl.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress))
          ));
    if (reader == nullptr) {
        reportError("Could not decode the selected audio file");
        return false;
    }
    if (reader->numChannels < 1 || reader->numChannels > 2 || reader->sampleRate <= 0.0) {
        reportError("Audio preview requires a mono or stereo file with a valid sample rate");
        return false;
    }
    readerSource = std::make_unique<PreviewReaderSource>(reader.release(), readFailed);
    prepareInput();
    previewSource.setInputGain(1.0f, true);
    const auto refreshed = refreshPluginChain();
    juce::ignoreUnused(refreshed);
    sendChangeMessage();
    return true;
}

/// Allocates and pre-fills device-rate input buffers outside the audio callback.
void AudioPreviewPlayer::prepareInput()
{
    previewSource.releaseResources();
    previewSource.setInput(nullptr, 0.0, 2);
    resamplingSource.reset();
    bufferingSource.reset();
    if (readerSource == nullptr) {
        return;
    }
    const auto* reader = readerSource->getAudioFormatReader();
    const auto ratio = reader->sampleRate / sampleRate;
    const auto bufferSamples
        = juce::jmax(readAheadBufferSamples, static_cast<int>(std::ceil(blockSize * ratio * 8.0)) + 256);
    bufferingSource
        = std::make_unique<PreviewBufferingSource>(readerSource.get(), readAheadThread, bufferSamples, bufferUnderrun);
    resamplingSource = std::make_unique<juce::ResamplingAudioSource>(bufferingSource.get(), false, 2);
    resamplingSource->setResamplingRatio(ratio);
    previewSource.setInput(
        resamplingSource.get(),
        static_cast<double>(reader->lengthInSamples) / reader->sampleRate,
        static_cast<int>(reader->numChannels)
    );
    previewSource.prepareToPlay(blockSize, sampleRate);
}

/// Closes the file after callbacks have stopped referencing its resampler and buffered reader.
void AudioPreviewPlayer::release()
{
    const juce::ScopedLock lock(renderingLock);
    previewSource.releaseResources();
    previewSource.setInput(nullptr, 0.0, 2);
    resamplingSource.reset();
    bufferingSource.reset();
    readerSource.reset();
    readFailed.store(false);
    bufferUnderrun.store(false);
    sendChangeMessage();
}

/// Attaches or detaches the slot owner while retaining safe processor lifetimes.
void AudioPreviewPlayer::setPluginChain(PluginChain* chain)
{
    const juce::ScopedLock lock(renderingLock);
    pluginChain = chain;
    const auto refreshed = refreshPluginChain();
    juce::ignoreUnused(refreshed);
}

/// Rebuilds the prepared pointer view after a structural or latency change, preserving audible time and intent.
bool AudioPreviewPlayer::refreshPluginChain()
{
    const juce::ScopedLock lock(renderingLock);
    const auto position = previewSource.getCurrentPosition();
    auto resume = previewSource.isPlaying();
    previewSource.stop();
    PreviewChain chain;
    if (pluginChain != nullptr) {
        const auto channels
            = readerSource != nullptr ? static_cast<int>(readerSource->getAudioFormatReader()->numChannels) : 2;
        chain = pluginChain->preparePreview(sampleRate, blockSize, channels, false, pluginPreviewEnabled);
    }
    const auto error = pluginPreviewEnabled ? chain.preparation.errorMessage : juce::String();
    if (error.isNotEmpty()) {
        pluginPreviewEnabled = false;
        resume = false;
    }
    previewSource.setChain(
        pluginPreviewEnabled ? std::move(chain.instances) : std::vector<juce::AudioPluginInstance*>(),
        chain.preparation,
        pluginPreviewEnabled,
        latencyCompensated
    );
    const bool positioned = reposition(position, resume);
    if (error.isNotEmpty()) {
        reportError(error);
    }
    sendChangeMessage();
    return error.isEmpty() && positioned;
}

/// Toggles the real-time chain, returning false if its complete configuration could not be prepared.
bool AudioPreviewPlayer::setPluginPreviewEnabled(const bool enabled)
{
    const juce::ScopedLock lock(renderingLock);
    pluginPreviewEnabled = enabled;
    return refreshPluginChain();
}

/// Changes compensation at the same audible position, re-priming only when playback is active.
void AudioPreviewPlayer::setLatencyCompensated(const bool enabled)
{
    const juce::ScopedLock lock(renderingLock);
    latencyCompensated = enabled;
    const auto refreshed = refreshPluginChain();
    juce::ignoreUnused(refreshed);
}

/// Applies custom input gain to the wet path without changing dry preview or normalization.
void AudioPreviewPlayer::setInputGain(const float linearGain, const bool immediately)
{
    const juce::ScopedLock lock(renderingLock);
    previewSource.setInputGain(linearGain, immediately);
}

/// Flushes decoder/resampler history before resuming from an audible position.
bool AudioPreviewPlayer::reposition(const double seconds, const bool resume)
{
    previewSource.stop();
    previewSource.setPosition(seconds);
    if (bufferingSource != nullptr && resamplingSource != nullptr) {
        const auto* reader = readerSource->getAudioFormatReader();
        bufferingSource->setNextReadPosition(
            static_cast<juce::int64>(std::llround(previewSource.getCurrentPosition() * reader->sampleRate))
        );
        resamplingSource->flushBuffers();
        if (resume) {
            const auto inputSamples = static_cast<int>(std::ceil(blockSize * reader->sampleRate / sampleRate)) + 32;
            if (!bufferingSource->waitForNextAudioBlockReady({nullptr, 0, inputSamples}, seekBufferTimeoutMilliseconds))
            {
                reportError("Timed out buffering audio preview after a seek");
                return false;
            }
            previewSource.start();
        }
    }
    return true;
}

/// Starts or restarts playback without allowing the waveform to bypass latency compensation.
void AudioPreviewPlayer::start()
{
    const juce::ScopedLock lock(renderingLock);
    if (readerSource == nullptr) {
        return;
    }
    const auto* reader = readerSource->getAudioFormatReader();
    const auto duration = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
    const auto position = previewSource.getCurrentPosition();
    const auto positioned = reposition(position >= duration ? 0.0 : position, true);
    juce::ignoreUnused(positioned);
    sendChangeMessage();
}

/// Marks playback stopped immediately while fading the remaining callback audio to silence.
void AudioPreviewPlayer::stop()
{
    const juce::ScopedLock lock(renderingLock);
    previewSource.requestStop();
    sendChangeMessage();
}

/// Seeks while preserving whether playback was active, including during compensation pre-roll.
void AudioPreviewPlayer::setPosition(const double seconds)
{
    const juce::ScopedLock lock(renderingLock);
    const auto positioned = reposition(seconds, previewSource.isPlaying());
    juce::ignoreUnused(positioned);
    sendChangeMessage();
}

/// Toggles playback using the same path as waveform and cue interactions.
void AudioPreviewPlayer::togglePlayback()
{
    if (isPlaying()) {
        stop();
    } else {
        start();
    }
}

/// Reports whether a source file is open.
bool AudioPreviewPlayer::hasLoadedSource() const noexcept
{
    return readerSource != nullptr;
}

/// Reports logical playback rather than the input reader's advanced position.
bool AudioPreviewPlayer::isPlaying() const
{
    const juce::ScopedLock lock(renderingLock);
    return previewSource.isPlaying();
}

/// Reports the actual wet-path setting, including a failed attempt to enable a plugin.
bool AudioPreviewPlayer::isPluginPreviewEnabled() const noexcept
{
    return pluginPreviewEnabled;
}

/// Returns the audible file position for the waveform, held at EOF while tails drain.
double AudioPreviewPlayer::getCurrentPosition() const
{
    const juce::ScopedLock lock(renderingLock);
    return previewSource.getCurrentPosition();
}

/// Returns the source duration independently of plugin latency and tail.
std::optional<double> AudioPreviewPlayer::getLoadedDurationSeconds() const
{
    if (readerSource == nullptr) {
        return std::nullopt;
    }
    const auto* reader = readerSource->getAudioFormatReader();
    return static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
}

/// Records device configuration without preparing a VST3/AU from the device's callback thread.
void AudioPreviewPlayer::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    deviceReady.store(false);
    pendingRate.store(device->getCurrentSampleRate());
    pendingBlockSize.store(device->getCurrentBufferSizeSamples());
    triggerAsyncUpdate();
}

/// Gates rendering while the device is stopped or being replaced.
void AudioPreviewPlayer::audioDeviceStopped()
{
    deviceReady.store(false);
}

/// Prepares the new device context on the message thread while callbacks emit silence rather than wait.
void AudioPreviewPlayer::handleAsyncUpdate()
{
    const juce::ScopedLock lock(renderingLock);
    const auto position = previewSource.getCurrentPosition();
    const auto resume = previewSource.isPlaying();
    const auto newRate = pendingRate.load();
    const auto newBlockSize = pendingBlockSize.load();
    if (newRate <= 0.0 || newBlockSize <= 0) {
        reportError("The audio device reported an invalid playback configuration");
        return;
    }
    sampleRate = newRate;
    blockSize = newBlockSize;
    prepareInput();
    const bool refreshed = refreshPluginChain();
    const bool positioned = reposition(position, resume && refreshed);
    juce::ignoreUnused(positioned);
    deviceReady.store(true);
    sendChangeMessage();
}

/// Renders only when all resources are ready. Message-thread lifecycle work never blocks this callback.
void AudioPreviewPlayer::audioDeviceIOCallbackWithContext(
    const float* const* /*inputChannels*/,
    int /*inputChannelCount*/,
    float* const* outputChannels,
    const int outputChannelCount,
    const int sampleCount,
    const juce::AudioIODeviceCallbackContext& /*context*/
)
{
    const std::span outputs(outputChannels, static_cast<std::size_t>(outputChannelCount));
    for (auto* channel : outputs) {
        if (channel != nullptr) {
            juce::FloatVectorOperations::clear(channel, sampleCount);
        }
    }
    const juce::ScopedTryLock lock(renderingLock);
    if (!lock.isLocked() || !deviceReady.load()) {
        return;
    }
    std::array<float*, 2> activeChannels {};
    int activeCount = 0;
    for (auto* channel : outputs) {
        if (channel != nullptr && activeCount < 2) {
            activeChannels[static_cast<std::size_t>(activeCount++)] = channel;
        }
    }
    if (activeCount > 0) {
        juce::AudioBuffer<float> output(activeChannels.data(), activeCount, sampleCount);
        previewSource.getNextAudioBlock({&output, 0, sampleCount});
    }
}

/// Moves audio/read-thread failures and EOF notifications onto the message thread.
/// The waveform's VBlankAttachment calls this at the current display's refresh rate, including while idle or hidden.
void AudioPreviewPlayer::updatePlaybackStatus()
{
    const juce::ScopedLock lock(renderingLock);
    if (pluginChain != nullptr) {
        pluginChain->updatePreviewStatus();
        if (pluginPreviewEnabled) {
            previewSource.setTailSamples(pluginChain->getPreviewTailSamples());
        }
    }
    const bool decodeFailed = readFailed.exchange(false);
    const bool underrun = bufferUnderrun.exchange(false);
    if (decodeFailed || underrun || previewSource.hasFailed()) {
        previewSource.stop();
        const bool invalidOutput = previewSource.hasFailed();
        previewSource.setPosition(previewSource.getCurrentPosition());
        reportError(
            invalidOutput
                ? "A live plugin produced non-finite audio. Preview was stopped."
                : "Audio preview could not read the source fast enough or decoding failed. Preview was stopped."
        );
    }
    if (lastReportedPlaying != previewSource.isPlaying()) {
        lastReportedPlaying = previewSource.isPlaying();
        sendChangeMessage();
    }
}

/// Logs every failure and gives the GUI an explicit message-thread notification.
void AudioPreviewPlayer::reportError(const juce::String& message)
{
    utils::logError("Audio preview: {}", message);
    if (onError) {
        onError(message);
    }
}
}  // namespace audiobatch
