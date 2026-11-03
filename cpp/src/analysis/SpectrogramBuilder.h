/// Streaming source-rate STFT construction for an optional spectrogram artifact.
/// Owns bounded PCM, FFT scratch, and time-reduced intensities without retaining decoded audio.

#pragma once

#include "analysis/SpectrogramData.h"

#include <JuceHeader.h>

#include <memory>

namespace audiobatch
{
struct SpectrogramBuilderState;

/// Builds mono or stereo power spectra without changing or mixing the PCM supplied to other analysis.
/// Unsupported geometry, invalid pushes, and nonfinite samples invalidate only this optional artifact.
/// finish is idempotent, returns null for invalid or empty input, and ignores subsequent pushes.
/// Instances belong to one decoding thread and cannot be copied or moved.
class SpectrogramBuilder
{
public:
    SpectrogramBuilder(int channels, int sampleRate);

    ~SpectrogramBuilder();

    SpectrogramBuilder(const SpectrogramBuilder&) = delete;
    SpectrogramBuilder& operator=(const SpectrogramBuilder&) = delete;
    SpectrogramBuilder(SpectrogramBuilder&&) = delete;
    SpectrogramBuilder& operator=(SpectrogramBuilder&&) = delete;

    [[nodiscard]] std::shared_ptr<const SpectrogramData> finish();
    void push(const juce::AudioBuffer<float>& samples, int firstSample, int frames);

private:
    std::unique_ptr<SpectrogramBuilderState> state;
};
}  // namespace audiobatch
