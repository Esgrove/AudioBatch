/// Bounded, source-rate spectrogram data and its portable cache representation.
/// The mel rows and fixed decibel profile describe source audio independently of display colors or gain.

#pragma once

#include <JuceHeader.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace audiobatch
{
struct AudioAnalysisRecord;

/// Column-major mel power maxima, with the lowest frequency row first.
/// Column k combines hop centers in [k * samplesPerColumn, (k + 1) * samplesPerColumn).
/// Centers begin at source sample zero, continue every hopSize samples, and stop before lengthInSamples.
/// Bytes linearly encode [-120, +24] dB with nearest rounding, with ties toward the larger byte.
struct SpectrogramData {
    [[nodiscard]] std::size_t columnCount() const noexcept;

    /// Number of equal-width mel intervals between zero and source Nyquist.
    static constexpr int FREQUENCY_ROWS {512};
    /// Hard bound on stored time columns.
    static constexpr int MAXIMUM_COLUMNS {4096};
    /// Source-rate real FFT window length.
    static constexpr int FFT_SIZE {4096};
    /// Distance between adjacent source window centers.
    static constexpr int HOP_SIZE {1024};
    /// Revision of the window, power, mel support, quantization, and time reduction contract.
    static constexpr int ALGORITHM_VERSION {1};
    /// Revision of the explicit little-endian cache envelope.
    static constexpr int SERIALIZATION_VERSION {1};

    /// Silence and all smaller powers map to byte zero.
    static constexpr double MINIMUM_DECIBELS {-120.0};
    /// Powers at or above this ceiling map to byte 255.
    static constexpr double MAXIMUM_DECIBELS {24.0};

    /// Decoded source sample rate, without resampling.
    int sampleRate {0};
    /// Decoded mono or stereo channel count.
    int channelCount {0};
    /// Actual number of decoded frames, not an estimated reader length.
    std::int64_t lengthInSamples {0};
    /// hopSize times the smallest power of two that bounds the number of columns.
    std::int64_t samplesPerColumn {HOP_SIZE};
    /// Time-major bytes, each column containing frequencyRows low-to-high mel rows.
    std::vector<std::uint8_t> intensities;
};

[[nodiscard]] juce::MemoryBlock serializeSpectrogram(const SpectrogramData& data);
[[nodiscard]] std::shared_ptr<const SpectrogramData> deserializeSpectrogram(std::span<const std::byte> blob);

[[nodiscard]] bool isValidSpectrogramData(const SpectrogramData& data);
[[nodiscard]] bool spectrogramMatches(const SpectrogramData& data, const AudioAnalysisRecord& record);
[[nodiscard]] double frequencyToMel(double frequency) noexcept;
[[nodiscard]] double melToFrequency(double mel) noexcept;
[[nodiscard]] double spectrogramDecibels(std::uint8_t intensity) noexcept;
}  // namespace audiobatch
