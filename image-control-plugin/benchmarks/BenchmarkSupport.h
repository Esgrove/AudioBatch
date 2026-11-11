/// Shares benchmark options, workload configurations, timing types, and entry points across the CPU probes.
#pragma once

#include "Parameters.h"
#include <string_view>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ratio>
#include <string>
#include <vector>

namespace esgrove::image_control::benchmark
{
/// Monotonic wall clock used for callback durations and paced automation.
using Clock = std::chrono::steady_clock;
/// Floating-point seconds keep audio and wall-clock intervals in the same unit.
using Seconds = std::chrono::duration<double>;
/// Callback and display work are reported in microseconds.
using Microseconds = std::chrono::duration<double, std::micro>;

/// The complete callback modes, including a host bypass independent of the bypass parameter.
enum class Mode : std::uint8_t {
    neutral,
    centred,
    panned,
    delta,
    bypass,
    hostBypass,
    mono,
    narrowed,
    recoveryPartial,
    recoveryAll,
    solo,
    soloDelta,
};

/// The settled pointer state selects which software overlay paths are exercised without a desktop peer.
enum class OverlayHover : std::uint8_t {
    none,
    band,
    crossover,
};

/// Work limits and focused selection keep short investigations separate from the complete coverage matrix.
struct Options {
    int callbacks {512};
    int repetitions {3};
    int refreshes {600};
    int maximumWorkloadSeconds {10};
    double componentAudioSeconds {15.0};
    double pacedSeconds {0.32};
    std::string label {"unlabelled"};
    bool components {true};
    bool focused {false};
    bool processor {true};
    bool refreshSettingsOnly {false};
    bool worker {true};
};

/// Quantiles use nearest rank, and max remains an observed sample rather than an extrapolated bound.
struct Distribution {
    double p50 {0.0};
    double p95 {0.0};
    double p99 {0.0};
    double max {0.0};
};

inline constexpr int MAXIMUM_BLOCK_SIZE {512};
inline constexpr int REFRESH_RATE {120};
inline constexpr int VIEW_WIDTH {1000};
inline constexpr int VIEW_HEIGHT {420};
inline constexpr double MICROSECONDS_PER_MILLISECOND {1000.0};
inline constexpr BandGains MID_GAINS {0.9F, 1.0F, 1.1F, 0.8F};
inline constexpr BandGains SIDE_GAINS {0.5F, 1.2F, 1.4F, 0.7F};
inline constexpr BandPans PANS {0.0F, -0.3F, 0.2F, 0.5F};
inline constexpr BandPans CENTRED_PANS {};
inline constexpr BandWidths UNITY_WIDTHS {1.0F, 1.0F, 1.0F, 1.0F};
inline constexpr BandWidths NARROW_WIDTHS {0.0F, 0.25F, 0.5F, 0.75F};
inline constexpr BandRecoveries RECOVERY_OFF {};
inline constexpr BandRecoveries ALL_RECOVERY {1.0F, 0.75F, 0.5F, 1.0F};
inline constexpr float OUTPUT_GAIN {0.9F};

std::vector<float> makeNoise(std::size_t samples, unsigned seed);
std::string_view modeName(Mode mode);
const BandWidths& modeWidths(Mode mode);
const BandRecoveries& modeRecoveries(Mode mode);
Distribution distribution(std::vector<double> values);

void timeProcessorMatrix(const Options& options);
void timeFocusedProcessor(const Options& options);
void timeImager(
    const Options& options,
    const BandPans& pans,
    std::string_view name,
    const BandWidths& widths = UNITY_WIDTHS,
    const BandRecoveries& recoveries = RECOVERY_OFF
);
void timeImagerSetters(const Options& options);
void timeEditorViewSettings(const Options& options);
void timeSpectrogramAnalysis(const Options& options);
void timeSpectrogramAnalyzerOnly(const Options& options);
void timeSpectrogramParticlesOnly(const Options& options);
void timeSpectrogramOverlays(const Options& options, OverlayHover hover = OverlayHover::none);
void timeBandCorrelationMeters(const Options& options, bool changing, double persistenceSeconds);
}  // namespace esgrove::image_control::benchmark
