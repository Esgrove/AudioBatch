/// Live preview rendering and sample-accurate audible-position accounting.
/// Preparation allocates scratch storage outside the callback,
/// while rendering consumes bounded input blocks, primes plugin latency,
/// and feeds silence through effect tails.
/// The chain runs in stereo, and the output of a mono file is folded back to mono like the processed file.

#include "plugins/PluginPreviewSource.h"

#include <algorithm>
#include <utility>

namespace audiobatch
{
namespace
{
constexpr int transitionLengthSamples {64};
constexpr double gainRampSeconds {0.005};
}  // namespace

/// Prepares the input and all scratch views. Plugin preparation belongs to the message-thread host.
void PluginPreviewSource::prepareToPlay(const int samplesPerBlockExpected, const double sampleRate)
{
    processingRate = sampleRate;
    blockSize = juce::jmax(1, samplesPerBlockExpected);
    lengthSamples = static_cast<juce::int64>(std::ceil(duration * processingRate));
    inputGain.reset(processingRate, gainRampSeconds);
    allocateBuffers();
    if (input != nullptr) {
        input->prepareToPlay(blockSize, processingRate);
    }
}

/// Releases only input resources. The slot owner releases plugins after rendering is quiescent.
void PluginPreviewSource::releaseResources()
{
    stop();
    if (input != nullptr) {
        input->releaseResources();
    }
}

/// Attaches an inactive decoded/resampled source. The host prepares it before starting playback.
void PluginPreviewSource::setInput(juce::AudioSource* source, const double durationSeconds, const int channels)
{
    stop();
    input = source;
    duration = juce::jmax(0.0, durationSeconds);
    sourceChannels = juce::jlimit(1, 2, channels);
    lengthSamples = static_cast<juce::int64>(std::ceil(duration * processingRate));
    position = 0.0;
    failed = false;
}

/// Publishes a prepared chain while rendering is stopped or locked out by the host.
void PluginPreviewSource::setChain(
    std::vector<juce::AudioPluginInstance*> processors,
    const plugin_processing::ChainPreparation& preparation,
    const bool enabled,
    const bool compensate
)
{
    plugins = std::move(processors);
    wet = enabled;
    compensation = compensate;
    latencySamples = wet ? preparation.latencySamples : 0;
    tailSamples = wet ? preparation.tailSamples : 0;
    processingChannels = juce::jmax(2, preparation.processChannelCount);
    allocateBuffers();
}

/// Allocates the widest bus buffer and per-processor channel views before entering the audio callback.
void PluginPreviewSource::allocateBuffers()
{
    scratch.setSize(processingChannels, blockSize);
    blockView.setDataToReferTo(scratch.getArrayOfWritePointers(), processingChannels, blockSize);
    pluginViews.clear();
    pluginViews.reserve(plugins.size());
    for (const auto* plugin : plugins) {
        const auto channels = juce::jmax(plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
        pluginViews.emplace_back(scratch.getArrayOfWritePointers(), channels, blockSize);
    }
    midi.ensureSize(2048);
}

/// Smooths gain edits during wet playback, or sets a new file's gain before its first audible sample.
void PluginPreviewSource::setInputGain(const LinearGain gain, const bool immediately)
{
    if (immediately) {
        inputGain.setCurrentAndTargetValue(gain.value);
    } else {
        inputGain.setTargetValue(gain.value);
    }
}

/// Updates an effect's current tail estimate without resetting audio or interrupting parameter edits.
void PluginPreviewSource::setTailSamples(const juce::int64 samples)
{
    tailSamples = wet ? juce::jmax<juce::int64>(0, samples) : 0;
}

/// Positions the audible clock. The host separately seeks and flushes the upstream resampler.
void PluginPreviewSource::setPosition(const double seconds)
{
    position = juce::jlimit(0.0, duration, seconds);
    resetPlayback();
}

/// Clears latency and tail accounting without restoring plugin parameters.
void PluginPreviewSource::resetPlayback()
{
    startSample = static_cast<juce::int64>(std::llround(position * processingRate));
    processedSamples = 0;
    emittedSamples = 0;
    preRollRemaining = wet && compensation ? latencySamples : 0;
    transitionSamples = 0;
    failed = false;
    failedPluginIndex = -1;
    stopRequested = false;
    fadeOutRemaining = 0;
    for (auto* plugin : plugins) {
        plugin->reset();
    }
}

/// Starts at the current audible position after the host has positioned the input.
void PluginPreviewSource::start()
{
    resetPlayback();
    playing = input != nullptr && position < duration;
}

/// Stops immediately, discarding the current tail when playback is subsequently restarted.
void PluginPreviewSource::stop()
{
    playing = false;
    stopRequested = false;
    fadeOutRemaining = 0;
}

/// Fades an audible user stop over one short transition instead of introducing a discontinuity.
/// The logical playing state changes immediately, so selecting another file cannot accidentally auto-start it.
void PluginPreviewSource::requestStop()
{
    if (preRollRemaining > 0) {
        stop();
    } else if (playing) {
        stopRequested = true;
        fadeOutRemaining = transitionLengthSamples;
    }
}

/// Reports active playback, including compensation pre-roll and effect-tail draining.
bool PluginPreviewSource::isPlaying() const noexcept
{
    return playing && !stopRequested;
}

/// Keeps the output device alive only until the short user-stop fade has finished rendering.
bool PluginPreviewSource::isStopping() const noexcept
{
    return playing && stopRequested;
}

/// Reports non-finite plugin output for the host to surface outside the audio callback.
bool PluginPreviewSource::hasFailed() const noexcept
{
    return failed;
}

/// Returns the audible file position, held at EOF during tail draining.
double PluginPreviewSource::getCurrentPosition() const noexcept
{
    return position;
}

/// Copies main-bus output, applies transition ramps, and advances the audible clock.
void PluginPreviewSource::copyOutput(
    const juce::AudioSourceChannelInfo& info,
    const int offset,
    const int samples,
    const int signalChannels
)
{
    const auto outputChannels = info.buffer->getNumChannels();
    for (int channel = 0; channel < outputChannels; ++channel) {
        const auto sourceChannel = signalChannels == 1 ? 0 : juce::jmin(channel, signalChannels - 1);
        info.buffer->copyFrom(channel, info.startSample + offset, scratch, sourceChannel, 0, samples);
    }
    if (outputChannels == 1 && signalChannels >= 2) {
        info.buffer->addFrom(0, info.startSample + offset, scratch, 1, 0, samples);
        info.buffer->applyGain(0, info.startSample + offset, samples, 0.5f);
    }
    if (transitionSamples < transitionLengthSamples) {
        const auto rampSamples = juce::jmin(samples, transitionLengthSamples - transitionSamples);
        info.buffer->applyGainRamp(
            info.startSample + offset,
            rampSamples,
            static_cast<float>(transitionSamples + 1) / transitionLengthSamples,
            static_cast<float>(transitionSamples + rampSamples) / transitionLengthSamples
        );
        transitionSamples += rampSamples;
    }
    emittedSamples += samples;
    position = juce::jmin(duration, static_cast<double>(startSample + emittedSamples) / processingRate);
    if (stopRequested) {
        info.buffer->applyGainRamp(
            info.startSample + offset,
            samples,
            static_cast<float>(fadeOutRemaining) / transitionLengthSamples,
            static_cast<float>(fadeOutRemaining - samples) / transitionLengthSamples
        );
        fadeOutRemaining -= samples;
        if (fadeOutRemaining == 0) {
            playing = false;
        }
    }
}

/// Reads the remaining file audio or tail silence and runs one allocation-free processing block.
/// Invalid output stops rendering and is reported by the host outside the audio callback.
int PluginPreviewSource::processInputBlock(const int samples, const juce::int64 sourceRemaining)
{
    scratch.clear();
    const auto inputSamples
        = static_cast<int>(juce::jlimit<juce::int64>(0, samples, sourceRemaining - processedSamples));
    if (inputSamples > 0) {
        input->getNextAudioBlock({&scratch, 0, inputSamples});
    }
    blockView.setDataToReferTo(scratch.getArrayOfWritePointers(), processingChannels, samples);
    auto signalChannels = sourceChannels;
    if (wet) {
        for (int sample = 0; sample < samples; ++sample) {
            const auto gain = inputGain.getNextValue();
            for (int channel = 0; channel < sourceChannels; ++channel) {
                scratch.setSample(channel, sample, scratch.getSample(channel, sample) * gain);
            }
        }
        signalChannels = processBlockThroughChain(blockView, plugins, sourceChannels, midi, pluginViews);
        failedPluginIndex = findFailedProcessCall(plugins);
        // Processing writes a mono file as the average of the chain's channels, so its preview sounds the same.
        if (sourceChannels == 1 && signalChannels >= 2) {
            scratch.addFrom(0, 0, scratch, 1, 0, samples);
            scratch.applyGain(0, 0, samples, 0.5f);
            signalChannels = 1;
        }
    }
    if (failedPluginIndex >= 0 || !isBlockFinite(blockView, samples)) {
        failed = true;
        playing = false;
    }
    return signalChannels;
}

/// Describes why rendering failed, for the host to show after hasFailed returned true.
/// Must run on the message thread with the rendering stopped, since it reads the plugin's name.
juce::String PluginPreviewSource::describeFailure() const
{
    if (failedPluginIndex >= 0 && std::cmp_less(failedPluginIndex, plugins.size())) {
        return describeFailedProcessCall(*plugins[static_cast<std::size_t>(failedPluginIndex)]);
    }
    return "A live plugin produced non-finite audio";
}

/// Renders at most one block's worth of work per output block, including muted pre-roll.
/// All EOF extension is explicit, so JUCE transport fades cannot truncate delayed final samples.
void PluginPreviewSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    if (!playing || input == nullptr) {
        return;
    }

    const auto sourceRemaining = juce::jmax<juce::int64>(0, lengthSamples - startSample);
    const auto renderLength = sourceRemaining + latencySamples + tailSamples;
    int offset = 0;
    while (offset < info.numSamples && playing) {
        const auto remaining = renderLength - processedSamples;
        if (remaining <= 0) {
            playing = false;
            break;
        }
        auto samples = static_cast<int>(juce::jmin<juce::int64>(blockSize, info.numSamples - offset, remaining));
        if (preRollRemaining > 0) {
            samples = static_cast<int>(juce::jmin<juce::int64>(samples, preRollRemaining));
        }
        if (stopRequested) {
            samples = juce::jmin(samples, fadeOutRemaining);
        }
        const auto signalChannels = processInputBlock(samples, sourceRemaining);
        if (failed) {
            info.clearActiveBufferRegion();
            return;
        }
        processedSamples += samples;
        if (preRollRemaining > 0) {
            preRollRemaining -= samples;
        } else {
            copyOutput(info, offset, samples, signalChannels);
        }
        offset += samples;
        if (processedSamples == renderLength) {
            playing = false;
            position = duration;
        }
    }
}
}  // namespace audiobatch
