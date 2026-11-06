/// Declares and implements the signal helpers the ImageControl tests share:
/// sines, deterministic noise, and RMS measurement over a range of samples.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <span>
#include <vector>

namespace esgrove::image_control::test
{
/// Sample rate of the test signals.
constexpr double TEST_SAMPLE_RATE {48'000.0};

/// Returns a sine of the given frequency and amplitude.
inline std::vector<float> makeSine(const double frequency, const float amplitude, const std::size_t samples)
{
    std::vector<float> signal(samples);
    const double step = 2.0 * std::numbers::pi * frequency / TEST_SAMPLE_RATE;
    for (std::size_t sample = 0; sample < samples; ++sample) {
        signal[sample] = amplitude * static_cast<float>(std::sin(step * static_cast<double>(sample)));
    }
    return signal;
}

/// Returns uniform noise between -amplitude and amplitude from a fixed seed, so runs repeat exactly.
inline std::vector<float> makeNoise(const float amplitude, const std::size_t samples, const unsigned seed)
{
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> distribution(-amplitude, amplitude);
    std::vector<float> signal(samples);
    for (auto& value : signal) {
        value = distribution(generator);
    }
    return signal;
}

/// Returns the RMS of a span of samples.
inline double rms(const std::span<const float> samples)
{
    double sum = 0.0;
    for (const float value : samples) {
        sum += static_cast<double>(value) * static_cast<double>(value);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

/// Builds a buffer from one or two channels of samples of the same length.
inline juce::AudioBuffer<float> makeBuffer(const std::vector<std::vector<float>>& channels)
{
    juce::AudioBuffer<float> buffer(static_cast<int>(channels.size()), static_cast<int>(channels.front().size()));
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
        buffer.copyFrom(static_cast<int>(channel), 0, channels[channel].data(), buffer.getNumSamples());
    }
    return buffer;
}

/// Returns the samples of one channel of a buffer.
inline std::span<const float> channelOf(const juce::AudioBuffer<float>& buffer, const int channel)
{
    return {buffer.getReadPointer(channel), static_cast<std::size_t>(buffer.getNumSamples())};
}
}  // namespace esgrove::image_control::test
