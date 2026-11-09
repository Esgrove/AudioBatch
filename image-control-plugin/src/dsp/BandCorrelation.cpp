/// Implements time-weighted, centered stereo correlation without allocating or locking during audio processing.
/// Readings are published once per convolution partition and are invalid during silence or one-channel audio.

#include "dsp/BandCorrelation.h"

#include "Parameters.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>

namespace esgrove::image_control
{
namespace
{
/// Integration time of the channel moments, in seconds.
constexpr double CORRELATION_SECONDS {0.15};
/// Minimum centered channel power for a meaningful reading, equivalent to -100 dBFS RMS.
constexpr double MIN_CORRELATION_POWER {1.0e-10};
}  // namespace

/// Starts every meter as unavailable.
BandCorrelation::BandCorrelation()
{
    static_assert(std::atomic<float>::is_always_lock_free);
    reset();
}

/// Returns the latest independent meter readings without touching audio-thread-owned state.
BandCorrelations BandCorrelation::readings() const noexcept
{
    BandCorrelations result {};
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        result[band] = published[band].load(std::memory_order_relaxed);
    }
    return result;
}

/// Precomputes the sample weight so the integration time does not depend on sample rate or block size.
void BandCorrelation::prepare(const double sampleRate) noexcept
{
    sampleWeight = -std::expm1(-1.0 / (sampleRate * CORRELATION_SECONDS));
    reset();
}

/// Publishes normalized centered covariance, bounded to the mathematical correlation range.
void BandCorrelation::publish() noexcept
{
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const auto& state = moments[band];
        const double leftVariance = std::max(0.0, state.leftPower - state.leftMean * state.leftMean);
        const double rightVariance = std::max(0.0, state.rightPower - state.rightMean * state.rightMean);
        float result = std::numeric_limits<float>::quiet_NaN();
        if (leftVariance > MIN_CORRELATION_POWER && rightVariance > MIN_CORRELATION_POWER) {
            const double covariance = state.crossPower - state.leftMean * state.rightMean;
            const double correlation = covariance / std::sqrt(leftVariance * rightVariance);
            if (std::isfinite(correlation)) {
                result = static_cast<float>(std::clamp(correlation, -1.0, 1.0));
            }
        }
        published[band].store(result, std::memory_order_relaxed);
    }
}

/// Accumulates a processed band sample before global Delta and Mono monitoring.
void BandCorrelation::push(const std::size_t band, const float left, const float right) noexcept
{
    auto& state = moments[band];
    if (!std::isfinite(left) || !std::isfinite(right)) {
        state = {};
        return;
    }
    const auto leftValue = static_cast<double>(left);
    const auto rightValue = static_cast<double>(right);
    state.leftMean += sampleWeight * (leftValue - state.leftMean);
    state.rightMean += sampleWeight * (rightValue - state.rightMean);
    state.leftPower += sampleWeight * (leftValue * leftValue - state.leftPower);
    state.rightPower += sampleWeight * (rightValue * rightValue - state.rightPower);
    state.crossPower += sampleWeight * (leftValue * rightValue - state.crossPower);
}

/// Clears signal history while preserving the prepared integration rate.
void BandCorrelation::reset() noexcept
{
    moments = {};
    for (auto& reading : published) {
        reading.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
    }
}
}  // namespace esgrove::image_control
