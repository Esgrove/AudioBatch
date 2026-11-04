/// Builds optional native thumbnail peaks and spectra from one bounded PCM pass on the enrichment worker.
/// Shared streaming builders isolate optional artifact failures without a second decoder or thread.

#include "gui/PreviewArtifactDecoder.h"

#include "analysis/DecodeFailureTracker.h"
#include "analysis/NativeWaveformBuilder.h"
#include "analysis/SpectrogramBuilder.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/ThreadUtils.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

namespace audiobatch
{
namespace
{
constexpr juce::int64 MAXIMUM_POINTS {4'194'304};
constexpr int DECODE_BLOCK_SIZE {16'384};
/// Both preview artifacts share the native thumbnail decoder's supported source-rate ceiling.
constexpr double MAXIMUM_SAMPLE_RATE {768'000.0};

/// Feeds a spectral block, discarding only the optional spectral state if processing throws.
void pushSpectrogram(
    std::unique_ptr<SpectrogramBuilder>& spectrum,
    const juce::AudioBuffer<float>& buffer,
    const int count
)
{
    if (!spectrum) {
        return;
    }
    try {
        spectrum->push(buffer, 0, count);
    } catch (...) {
        spectrum.reset();
        utils::logDebug("Preview spectrogram processing failed: {}", utils::describeCurrentException());
    }
}

/// Feeds the shared waveform accumulator without letting its failures discard independent spectra.
void pushWaveform(
    std::unique_ptr<NativeWaveformBuilder>& waveform,
    const juce::AudioBuffer<float>& buffer,
    const int count,
    const juce::int64 decodedFrames
)
{
    if (!waveform) {
        return;
    }
    try {
        waveform->push(buffer, 0, count);
        if (waveform->framesReceived() != decodedFrames) {
            waveform.reset();
        }
    } catch (...) {
        waveform.reset();
        utils::logDebug("Preview waveform processing failed: {}", utils::describeCurrentException());
    }
}

/// Completes independent artifacts after all PCM has been consumed, with no provisional data persisted.
PreviewAudioArtifacts finishArtifacts(
    std::unique_ptr<NativeWaveformBuilder> waveform,
    std::unique_ptr<SpectrogramBuilder> spectrum
)
{
    PreviewAudioArtifacts artifacts;
    if (waveform) {
        try {
            artifacts.waveform = waveform->finish();
        } catch (...) {
            utils::logDebug("Preview waveform completion failed: {}", utils::describeCurrentException());
        }
    }
    if (spectrum) {
        try {
            artifacts.spectrogram = spectrum->finish();
        } catch (...) {
            utils::logDebug("Preview spectrogram completion failed: {}", utils::describeCurrentException());
        }
    }
    return artifacts;
}
}  // namespace

/// Decodes requested optional artifacts together, never constructing a loudness or true-peak meter.
/// Artifact-specific failures remain independent, while cancellation and repeated read failures discard both.
PreviewAudioArtifacts buildPreviewAudioArtifacts(
    juce::AudioFormatReader& reader,
    const bool needWaveform,
    const bool needSpectrogram,
    const utils::CancellationCheck& isCancelled
)
{
    if ((!needWaveform && !needSpectrogram) || reader.lengthInSamples <= 0
        || (reader.numChannels != 1 && reader.numChannels != 2) || !std::isfinite(reader.sampleRate)
        || reader.sampleRate <= 0.0 || reader.sampleRate > MAXIMUM_SAMPLE_RATE
        || utils::isCancellationRequested(isCancelled))
    {
        return {};
    }
    const auto channels = static_cast<int>(reader.numChannels);
    std::unique_ptr<NativeWaveformBuilder> waveform;
    if (needWaveform && reader.lengthInSamples / samplesPerThumbnailPoint(reader.lengthInSamples) < MAXIMUM_POINTS) {
        try {
            waveform = std::make_unique<NativeWaveformBuilder>(
                channels, static_cast<int>(reader.sampleRate), reader.lengthInSamples
            );
        } catch (...) {
            utils::logDebug("Preview waveform setup failed: {}", utils::describeCurrentException());
        }
    }
    std::unique_ptr<SpectrogramBuilder> spectrum;
    if (needSpectrogram) {
        try {
            spectrum = std::make_unique<SpectrogramBuilder>(channels, static_cast<int>(reader.sampleRate));
        } catch (...) {
            utils::logDebug("Preview spectrogram setup failed: {}", utils::describeCurrentException());
        }
    }
    if (!waveform && !spectrum) {
        return {};
    }
    juce::AudioBuffer<float> buffer(channels, DECODE_BLOCK_SIZE);
    DecodeFailureTracker failures(reader.lengthInSamples);
    for (juce::int64 position = 0; position < reader.lengthInSamples; position += DECODE_BLOCK_SIZE) {
        if (utils::isCancellationRequested(isCancelled)) {
            return {};
        }
        const auto count
            = static_cast<int>(std::min<juce::int64>(DECODE_BLOCK_SIZE, reader.lengthInSamples - position));
        buffer.clear();
        if (!failures.recordBlock(
                utils::readFloatSamples(reader, utils::writePointers(buffer), position, count), position, count
            ))
        {
            return {};
        }
        pushSpectrogram(spectrum, buffer, count);
        pushWaveform(waveform, buffer, count, position + count);
        if (!waveform && !spectrum) {
            return {};
        }
    }
    auto artifacts = finishArtifacts(std::move(waveform), std::move(spectrum));
    return utils::isCancellationRequested(isCancelled) ? PreviewAudioArtifacts {} : std::move(artifacts);
}

/// Rejects source replacement before opening, after opening and after the single optional-artifact decode.
PreviewAudioArtifacts buildPreviewAudioArtifacts(
    const utils::FileIdentity& identity,
    const bool needWaveform,
    const bool needSpectrogram,
    const utils::CancellationCheck& isCancelled
)
{
    const juce::File file(identity.path);
    if ((!needWaveform && !needSpectrogram) || !identity.matches(file) || utils::isCancellationRequested(isCancelled)) {
        return {};
    }
    const auto reader = utils::createAudioReader(file);
    if (!reader || !identity.matches(file)) {
        return {};
    }
    auto artifacts = buildPreviewAudioArtifacts(*reader, needWaveform, needSpectrogram, isCancelled);
    return identity.matches(file) && !utils::isCancellationRequested(isCancelled) ? std::move(artifacts)
                                                                                  : PreviewAudioArtifacts {};
}

/// Preserves waveform-only decoding and native thumbnail compatibility for existing callers.
std::optional<juce::MemoryBlock> buildNativeWaveform(
    juce::AudioFormatReader& reader,
    const utils::CancellationCheck& isCancelled
)
{
    return buildPreviewAudioArtifacts(reader, true, false, isCancelled).waveform;
}

/// Preserves identity-guarded waveform-only decoding without requesting a spectral artifact.
std::optional<juce::MemoryBlock> buildNativeWaveform(
    const utils::FileIdentity& identity,
    const utils::CancellationCheck& isCancelled
)
{
    return buildPreviewAudioArtifacts(identity, true, false, isCancelled).waveform;
}
}  // namespace audiobatch
