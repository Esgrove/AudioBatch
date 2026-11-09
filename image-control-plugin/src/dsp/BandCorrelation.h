/// Declares the bounded, sample-rate-aware correlation meters shared by the imager and editor.
/// Each band publishes an independent atomic reading. NaN means insufficient energy in either channel.

#pragma once

#include "Parameters.h"

#include <array>
#include <atomic>
#include <cstddef>

namespace esgrove::image_control
{
/// One signed correlation per band, or NaN when the measurement is unavailable.
using BandCorrelations = std::array<float, BAND_COUNT>;

/// Exponentially weighted channel moments for one band's processed stereo signal.
struct CorrelationMoments {
    /// Mean of the left channel.
    double leftMean {0.0};
    /// Mean of the right channel.
    double rightMean {0.0};
    /// Second moment of the left channel.
    double leftPower {0.0};
    /// Second moment of the right channel.
    double rightPower {0.0};
    /// Cross moment of the two channels.
    double crossPower {0.0};
};

/// Audio-thread-owned moment accumulators with lock-free, independently readable band results.
class BandCorrelation
{
public:
    BandCorrelation();

    [[nodiscard]] BandCorrelations readings() const noexcept;

    void prepare(double sampleRate) noexcept;
    void publish() noexcept;
    void push(std::size_t band, float left, float right) noexcept;
    void reset() noexcept;

private:
    /// Only the audio thread accesses these accumulators.
    std::array<CorrelationMoments, BAND_COUNT> moments {};
    /// Each atomic is an independent meter value, not a synchronized multi-band snapshot.
    std::array<std::atomic<float>, BAND_COUNT> published {};
    /// Weight of one new sample, fixed during preparation.
    double sampleWeight {0.0};
};
}  // namespace esgrove::image_control
