/// Tests the correlation meter's message-thread history with deterministic elapsed times.
/// Independent exponential expectations cover Persistence smoothing and the selectable smoothed or live readout.
/// Range tests cover high-refresh extrema, bounded rolling expiry, Persistence changes, and ring-buffer wraps.
/// Invalid measurements and timing must clear or reseed history without leaving stale display state.

#include "Parameters.h"
#include "ui/BandStrip.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <utility>

namespace esgrove::image_control::test
{
/// Observes the production fade field without coupling tests to painted pixels or exposing a public display API.
struct BandCorrelationMeterTestAccess final {
    /// Returns the cached field at a signed correlation coordinate that lies on a spatial sample.
    [[nodiscard]] static float intensityAt(const BandCorrelationMeter& meter, const float value)
    {
        const auto sample = static_cast<std::size_t>(
            std::lround((value + 1.0F) * 0.5F * static_cast<float>(meter.rangeIntensity.size() - 1))
        );
        return meter.rangeIntensity.at(sample);
    }

    /// Returns whether all retained visual coverage is absent.
    [[nodiscard]] static bool fieldIsClear(const BandCorrelationMeter& meter)
    {
        return std::ranges::none_of(meter.rangeIntensity, [](const float intensity) { return intensity > 0.0F; });
    }
};

namespace
{
/// Maximum permitted late expiry of a display-observed extreme, in seconds.
constexpr double EXPIRY_RESOLUTION_SECONDS {1.0 / 120.0};
/// Allows float-state rounding accumulated over many exponential updates, not a timing discrepancy.
constexpr double TREND_TOLERANCE {5.0e-6};

/// Checks a present trend against an independently calculated exponential response.
void expectTrend(const BandCorrelationMeter& meter, const double expected)
{
    const auto trend = meter.trendValue();
    if (!trend.has_value()) {
        ADD_FAILURE() << "Expected a valid trend";
        return;
    }
    EXPECT_NEAR(static_cast<double>(trend.value()), expected, TREND_TOLERANCE);
}

/// Checks both extrema without dereferencing an unavailable range after an assertion failure.
void expectRange(const BandCorrelationMeter& meter, const float minimum, const float maximum)
{
    const auto range = meter.recentRange();
    if (!range.has_value()) {
        ADD_FAILURE() << "Expected a valid recent range";
        return;
    }
    EXPECT_FLOAT_EQ(range.value().getStart(), minimum);
    EXPECT_FLOAT_EQ(range.value().getEnd(), maximum);
}
}  // namespace

/// A first observation seeds both indicators directly, even after a nonzero initial elapsed time.
TEST(BandCorrelationMeter, FirstReadingInitializesHistoryWithoutZeroBias)
{
    for (const float value : {-0.75F, 0.0F, 0.5F}) {
        for (const double elapsed : {0.0, 1.0 / 60.0, MAX_PERSISTENCE_SECONDS}) {
            SCOPED_TRACE(value);
            SCOPED_TRACE(elapsed);
            BandCorrelationMeter meter("Low");
            EXPECT_FALSE(meter.trendValue().has_value());
            EXPECT_FALSE(meter.recentRange().has_value());
            meter.setCorrelation(value, elapsed);
            expectTrend(meter, static_cast<double>(value));
            expectRange(meter, value, value);
        }
    }
}

/// The numerical and accessible readouts follow the exponential trend by default.
TEST(BandCorrelationMeter, DefaultReadoutFollowsTheExponentialTrend)
{
    constexpr double persistenceSeconds {0.6};
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-0.75F);
    meter.setCorrelation(0.75F, persistenceSeconds, persistenceSeconds);
    EXPECT_EQ(meter.readoutText(), "+0.20");
    EXPECT_EQ(meter.valueText(), "+0.20");
    expectTrend(meter, 0.75 - 1.5 * std::exp(-1.0));
    expectRange(meter, -0.75F, 0.75F);

    meter.setCorrelation(0.75F, persistenceSeconds, persistenceSeconds);
    EXPECT_EQ(meter.readoutText(), "+0.55");
    EXPECT_EQ(meter.valueText(), "+0.55");
    expectTrend(meter, 0.75 - 1.5 * std::exp(-2.0));
}

/// Switching readout mode changes text immediately without resetting or advancing either history indicator.
TEST(BandCorrelationMeter, ReadoutModeChangesPreserveHistoryAndKeepLiveModeImmediate)
{
    constexpr double persistenceSeconds {0.6};
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-0.75F);
    meter.setCorrelation(0.75F, persistenceSeconds, persistenceSeconds);
    const double expected = 0.75 - 1.5 * std::exp(-1.0);
    for (int toggle = 0; toggle < 2; ++toggle) {
        meter.setSmoothedReadout(false);
        EXPECT_EQ(meter.readoutText(), "+0.75");
        EXPECT_EQ(meter.valueText(), "+0.75");
        expectTrend(meter, expected);
        expectRange(meter, -0.75F, 0.75F);
        meter.setSmoothedReadout(true);
        EXPECT_EQ(meter.readoutText(), "+0.20");
        EXPECT_EQ(meter.valueText(), "+0.20");
        expectTrend(meter, expected);
        expectRange(meter, -0.75F, 0.75F);
    }
    meter.setSmoothedReadout(false);
    meter.setCorrelation(-0.25F, 0.0, persistenceSeconds);
    EXPECT_EQ(meter.readoutText(), "-0.25");
    EXPECT_EQ(meter.valueText(), "-0.25");
    expectTrend(meter, expected);
    expectRange(meter, -0.75F, 0.75F);
    meter.setSmoothedReadout(true);
    EXPECT_EQ(meter.readoutText(), "+0.20");
    meter.setCorrelation(-0.25F, persistenceSeconds, persistenceSeconds);
    expectTrend(meter, -0.25 + (expected + 0.25) * std::exp(-1.0));
}

/// Identical live readings keep updating the displayed trend until its rounded text reaches the held value.
TEST(BandCorrelationMeter, RepeatedHeldReadingsConvergeTheDisplayedText)
{
    constexpr double persistenceSeconds {0.1};
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-1.0F);
    for (const char* expected : {"-0.21", "+0.08", "+0.19", "+0.23", "+0.24", "+0.25"}) {
        meter.setCorrelation(0.25F, persistenceSeconds, persistenceSeconds);
        EXPECT_EQ(meter.readoutText(), expected);
        EXPECT_EQ(meter.valueText(), expected);
    }
}

/// A held step reaches the same response after the same real time at low, high, and irregular refresh rates.
TEST(BandCorrelationMeter, HeldStepConvergesIndependentlyOfFrameRate)
{
    constexpr double persistenceSeconds {0.6};
    constexpr double durationSeconds {1.2};
    const double expected = 1.0 - 2.0 * std::exp(-durationSeconds / persistenceSeconds);
    for (const int frames : {1, 18, 36, 72, 144, 288, 1200}) {
        SCOPED_TRACE(frames);
        BandCorrelationMeter meter("Low");
        meter.setCorrelation(-1.0F);
        for (int frame = 0; frame < frames; ++frame) {
            meter.setCorrelation(1.0F, durationSeconds / static_cast<double>(frames), persistenceSeconds);
        }
        expectTrend(meter, expected);
        EXPECT_EQ(meter.readoutText(), "+0.73");
    }

    BandCorrelationMeter irregular("Low");
    irregular.setCorrelation(-1.0F);
    for (const double elapsed : {0.01, 0.19, 0.003, 0.497, 0.5}) {
        irregular.setCorrelation(1.0F, elapsed, persistenceSeconds);
    }
    expectTrend(irregular, expected);
}

/// A zero-duration refresh changes the live value and records its extreme without advancing the trend.
TEST(BandCorrelationMeter, ZeroElapsedRecordsExcursionsWithoutAdvancingTheTrend)
{
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-0.5F);
    meter.setCorrelation(0.8F);
    EXPECT_EQ(meter.readoutText(), "-0.50");
    expectTrend(meter, -0.5);
    expectRange(meter, -0.5F, 0.8F);
    meter.setCorrelation(-0.9F);
    expectTrend(meter, -0.5);
    expectRange(meter, -0.9F, 0.8F);
}

/// Repeated identical readings still advance smoothing and age an old extreme out of the trailing window.
TEST(BandCorrelationMeter, UnchangedReadingsAdvanceTrendAndExpireTheRange)
{
    constexpr double persistenceSeconds {0.1};
    constexpr double refreshSeconds {0.025};
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-1.0F);
    for (int frame = 0; frame < 3; ++frame) {
        meter.setCorrelation(0.25F, refreshSeconds, persistenceSeconds);
    }
    expectRange(meter, -1.0F, 0.25F);
    expectTrend(meter, 0.25 - 1.25 * std::exp(-0.075 / persistenceSeconds));
    for (int frame = 0; frame < 2; ++frame) {
        meter.setCorrelation(0.25F, refreshSeconds, persistenceSeconds);
    }
    expectRange(meter, 0.25F, 0.25F);
    expectTrend(meter, 0.25 - 1.25 * std::exp(-0.125 / persistenceSeconds));
}

/// Multiple observations in one bucket preserve both extrema, even after more refreshes than history buckets.
TEST(BandCorrelationMeter, HighRefreshRetainsBothExcursionsSharingOneBucket)
{
    constexpr double refreshSeconds {EXPIRY_RESOLUTION_SECONDS * 0.125};
    constexpr double persistenceSeconds {1.0};
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(0.1F);
    for (const float value : {-0.9F, 0.8F, -0.3F, 0.2F}) {
        meter.setCorrelation(value, refreshSeconds, persistenceSeconds);
    }
    expectRange(meter, -0.9F, 0.8F);
    for (int frame = 0; frame < 600; ++frame) {
        meter.setCorrelation(0.2F, refreshSeconds, persistenceSeconds);
    }
    expectRange(meter, -0.9F, 0.8F);
}

/// Extrema never disappear early and expire within one 1/120-second resolution at different bucket phases.
TEST(BandCorrelationMeter, RollingExpiryIsBoundedByOneHundredTwentiethOfASecond)
{
    constexpr double persistenceSeconds {0.1};
    for (const double phase : {0.125, 0.375, 0.625, 0.875}) {
        SCOPED_TRACE(phase);
        BandCorrelationMeter meter("Low");
        meter.setCorrelation(-1.0F, phase * EXPIRY_RESOLUTION_SECONDS, persistenceSeconds);
        meter.setCorrelation(0.0F, persistenceSeconds - EXPIRY_RESOLUTION_SECONDS * 0.01, persistenceSeconds);
        expectRange(meter, -1.0F, 0.0F);
        meter.setCorrelation(0.0F, EXPIRY_RESOLUTION_SECONDS * 1.01, persistenceSeconds);
        expectRange(meter, 0.0F, 0.0F);
    }
}

/// The lower and upper extremes age out independently rather than clearing the whole range together.
TEST(BandCorrelationMeter, OldMinimumAndMaximumExpireIndependently)
{
    constexpr double persistenceSeconds {0.1};
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-1.0F);
    meter.setCorrelation(1.0F, 0.05, persistenceSeconds);
    meter.setCorrelation(0.0F, 0.025, persistenceSeconds);
    expectRange(meter, -1.0F, 1.0F);
    meter.setCorrelation(0.0F, 0.035, persistenceSeconds);
    expectRange(meter, 0.0F, 1.0F);
    meter.setCorrelation(0.0F, 0.05, persistenceSeconds);
    expectRange(meter, 0.0F, 0.0F);
}

/// Shortening Persistence recomputes the range immediately, and increasing it reveals retained observations.
TEST(BandCorrelationMeter, PersistenceChangesReuseHistoryWithoutResettingTheTrend)
{
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-1.0F, 0.0, MAX_PERSISTENCE_SECONDS);
    meter.setCorrelation(0.6F, 0.5, MAX_PERSISTENCE_SECONDS);
    meter.setCorrelation(0.2F, 0.1, MAX_PERSISTENCE_SECONDS);
    expectRange(meter, -1.0F, 0.6F);
    const auto previousTrend = meter.trendValue();
    if (!previousTrend.has_value()) {
        ADD_FAILURE() << "Expected a valid trend before changing Persistence";
        return;
    }

    meter.setCorrelation(0.2F, 0.0, MIN_PERSISTENCE_SECONDS);
    expectRange(meter, 0.2F, 0.2F);
    expectTrend(meter, static_cast<double>(previousTrend.value()));
    meter.setCorrelation(0.2F, 0.0, 1.0);
    expectRange(meter, -1.0F, 0.6F);
    expectTrend(meter, static_cast<double>(previousTrend.value()));

    meter.setCorrelation(0.2F, 0.2, 0.1);
    expectRange(meter, 0.2F, 0.2F);
    expectTrend(meter, 0.2 + (static_cast<double>(previousTrend.value()) - 0.2) * std::exp(-2.0));
}

/// Even the longest window cannot resurrect observations older than maximum Persistence plus its expiry resolution.
TEST(BandCorrelationMeter, IncreasingPersistenceDoesNotResurrectExpiredHistory)
{
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-1.0F);
    meter.setCorrelation(0.4F, MAX_PERSISTENCE_SECONDS, MIN_PERSISTENCE_SECONDS);
    meter.setCorrelation(0.4F, EXPIRY_RESOLUTION_SECONDS * 2.0, MIN_PERSISTENCE_SECONDS);
    meter.setCorrelation(0.4F, 0.0, MAX_PERSISTENCE_SECONDS);
    expectRange(meter, 0.4F, 0.4F);
}

/// Finite Persistence is clamped to its supported range, while nonfinite values select the default time constant.
TEST(BandCorrelationMeter, PersistenceValidationAppliesToTrendAndRange)
{
    constexpr auto cases = std::array {
        std::pair {-100.0, MIN_PERSISTENCE_SECONDS},
        std::pair {0.0, MIN_PERSISTENCE_SECONDS},
        std::pair {MIN_PERSISTENCE_SECONDS * 0.5, MIN_PERSISTENCE_SECONDS},
        std::pair {MIN_PERSISTENCE_SECONDS, MIN_PERSISTENCE_SECONDS},
        std::pair {DEFAULT_PERSISTENCE_SECONDS, DEFAULT_PERSISTENCE_SECONDS},
        std::pair {MAX_PERSISTENCE_SECONDS, MAX_PERSISTENCE_SECONDS},
        std::pair {MAX_PERSISTENCE_SECONDS * 2.0, MAX_PERSISTENCE_SECONDS},
        std::pair {std::numeric_limits<double>::max(), MAX_PERSISTENCE_SECONDS},
        std::pair {std::numeric_limits<double>::quiet_NaN(), DEFAULT_PERSISTENCE_SECONDS},
        std::pair {std::numeric_limits<double>::infinity(), DEFAULT_PERSISTENCE_SECONDS},
        std::pair {-std::numeric_limits<double>::infinity(), DEFAULT_PERSISTENCE_SECONDS},
    };
    constexpr double elapsedSeconds {MIN_PERSISTENCE_SECONDS * 0.5};
    for (const auto& [supplied, effective] : cases) {
        SCOPED_TRACE(supplied);
        BandCorrelationMeter meter("Low");
        meter.setCorrelation(-1.0F);
        meter.setCorrelation(1.0F, elapsedSeconds, supplied);
        expectTrend(meter, 1.0 - 2.0 * std::exp(-elapsedSeconds / effective));
        expectRange(meter, -1.0F, 1.0F);
        meter.setCorrelation(1.0F, effective, supplied);
        expectTrend(meter, 1.0 - 2.0 * std::exp(-(elapsedSeconds + effective) / effective));
        expectRange(meter, 1.0F, 1.0F);
    }
}

/// NaN and either infinity remove both history indicators, and the next finite reading seeds them afresh.
TEST(BandCorrelationMeter, NonfiniteReadingsClearAndReseedHistory)
{
    for (const float invalid :
         {
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(),
         })
    {
        BandCorrelationMeter meter("Low");
        meter.setCorrelation(-0.8F);
        meter.setCorrelation(0.9F, 0.1, MAX_PERSISTENCE_SECONDS);
        meter.setCorrelation(invalid, 0.01, MAX_PERSISTENCE_SECONDS);
        EXPECT_EQ(meter.valueText(), "Unavailable");
        EXPECT_FALSE(meter.trendValue().has_value());
        EXPECT_FALSE(meter.recentRange().has_value());
        EXPECT_TRUE(BandCorrelationMeterTestAccess::fieldIsClear(meter));
        meter.setCorrelation(0.4F, 0.1, MAX_PERSISTENCE_SECONDS);
        expectTrend(meter, 0.4);
        expectRange(meter, 0.4F, 0.4F);
    }
}

/// Explicit clearing is idempotent and prevents old observations reappearing when the window grows.
TEST(BandCorrelationMeter, ClearHistoryRemovesLiveTrendAndRetainedExtrema)
{
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-1.0F);
    meter.setCorrelation(1.0F, 0.1);
    for (int clear = 0; clear < 2; ++clear) {
        meter.clearHistory();
        EXPECT_EQ(meter.valueText(), "Unavailable");
        EXPECT_FALSE(meter.trendValue().has_value());
        EXPECT_FALSE(meter.recentRange().has_value());
    }
    meter.setCorrelation(0.25F, 0.0, MAX_PERSISTENCE_SECONDS);
    expectTrend(meter, 0.25);
    expectRange(meter, 0.25F, 0.25F);
}

/// Finite out-of-range readings saturate before entering either history indicator or the live readout.
TEST(BandCorrelationMeter, AllIndicatorsStayWithinSignedUnitBounds)
{
    for (const float sign : {-1.0F, 1.0F}) {
        BandCorrelationMeter meter("Low");
        meter.setCorrelation(sign * std::numeric_limits<float>::max());
        expectTrend(meter, static_cast<double>(sign));
        expectRange(meter, sign, sign);
        EXPECT_EQ(meter.readoutText(), sign < 0.0F ? "-1.00" : "+1.00");
        meter.setCorrelation(-sign * std::numeric_limits<float>::max(), DEFAULT_PERSISTENCE_SECONDS);
        expectTrend(meter, static_cast<double>(sign) * (2.0 * std::exp(-1.0) - 1.0));
        expectRange(meter, -1.0F, 1.0F);
        EXPECT_EQ(meter.readoutText(), sign < 0.0F ? "+0.26" : "-0.26");
    }
}

/// Both zero signs and an elapsed time exactly at the maximum remain valid updates rather than history resets.
TEST(BandCorrelationMeter, ValidElapsedEndpointsPreserveTheExistingTrend)
{
    for (const double elapsed : {0.0, -0.0, MAX_PERSISTENCE_SECONDS}) {
        SCOPED_TRACE(elapsed);
        BandCorrelationMeter meter("Low");
        meter.setCorrelation(-1.0F);
        meter.setCorrelation(1.0F, elapsed, MAX_PERSISTENCE_SECONDS);
        expectTrend(meter, 1.0 - 2.0 * std::exp(-elapsed / MAX_PERSISTENCE_SECONDS));
        EXPECT_EQ(meter.readoutText(), elapsed > 0.0 ? "+0.26" : "-1.00");
    }
}

/// Invalid or excessively long gaps reseed from the live reading rather than carrying stale trend or extrema.
TEST(BandCorrelationMeter, InvalidElapsedTimeAndLongGapsResetHistory)
{
    for (const double elapsed :
         {
             -0.001,
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity(),
             std::nextafter(MAX_PERSISTENCE_SECONDS, std::numeric_limits<double>::infinity()),
             MAX_PERSISTENCE_SECONDS * 2.0,
             std::numeric_limits<double>::max(),
         })
    {
        SCOPED_TRACE(elapsed);
        BandCorrelationMeter meter("Low");
        meter.setCorrelation(-1.0F);
        meter.setCorrelation(0.5F, 0.05, MAX_PERSISTENCE_SECONDS);
        meter.setCorrelation(-0.25F, elapsed, MAX_PERSISTENCE_SECONDS);
        EXPECT_EQ(meter.readoutText(), "-0.25");
        expectTrend(meter, -0.25);
        expectRange(meter, -0.25F, -0.25F);
        EXPECT_FLOAT_EQ(BandCorrelationMeterTestAccess::intensityAt(meter, -1.0F), 0.0F);
        EXPECT_FLOAT_EQ(BandCorrelationMeterTestAccess::intensityAt(meter, 0.5F), 0.0F);
        meter.setCorrelation(0.25F, 0.1, MAX_PERSISTENCE_SECONDS);
        expectTrend(meter, 0.25 - 0.5 * std::exp(-0.1 / MAX_PERSISTENCE_SECONDS));
        expectRange(meter, -0.25F, 0.25F);
    }
}

/// Many ring wraps agree with exact and resolution-extended windows from an independent observation log.
TEST(BandCorrelationMeter, RepeatedRingWrapsPreserveBoundedRollingExtrema)
{
    constexpr double refreshSeconds {1.0 / 64.0};
    constexpr int readingCount {32'768};
    constexpr std::array windows {MIN_PERSISTENCE_SECONDS, 0.125, 0.5, MAX_PERSISTENCE_SECONDS};
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(0.0F);
    std::deque<std::pair<double, float>> observations {{0.0, 0.0F}};
    for (int reading = 1; reading <= readingCount; ++reading) {
        const double now = static_cast<double>(reading) * refreshSeconds;
        const double persistence = windows.at(static_cast<std::size_t>(reading / 257) % windows.size());
        float value = static_cast<float>(reading % 11 - 5) * 0.05F;
        if (reading % 257 == 0) {
            value = -1.0F;
        } else if (reading % 509 == 0) {
            value = 1.0F;
        }
        meter.setCorrelation(value, refreshSeconds, persistence);
        observations.emplace_back(now, value);
        while (observations.front().first < now - MAX_PERSISTENCE_SECONDS - EXPIRY_RESOLUTION_SECONDS) {
            observations.pop_front();
        }
        if (reading % 127 != 0 && reading != readingCount) {
            continue;
        }
        SCOPED_TRACE(reading);
        float exactMinimum = value;
        float exactMaximum = value;
        float extendedMinimum = value;
        float extendedMaximum = value;
        for (const auto& [time, observed] : observations) {
            if (time >= now - persistence) {
                exactMinimum = std::min(exactMinimum, observed);
                exactMaximum = std::max(exactMaximum, observed);
            }
            if (time >= now - persistence - EXPIRY_RESOLUTION_SECONDS) {
                extendedMinimum = std::min(extendedMinimum, observed);
                extendedMaximum = std::max(extendedMaximum, observed);
            }
        }
        const auto range = meter.recentRange();
        if (!range.has_value()) {
            ADD_FAILURE() << "Expected a valid recent range";
            return;
        }
        EXPECT_GE(range.value().getStart(), extendedMinimum);
        EXPECT_LE(range.value().getStart(), exactMinimum);
        EXPECT_GE(range.value().getEnd(), exactMaximum);
        EXPECT_LE(range.value().getEnd(), extendedMaximum);
        const auto trend = meter.trendValue();
        if (!trend.has_value()) {
            ADD_FAILURE() << "Expected a valid trend";
            return;
        }
        EXPECT_TRUE(std::isfinite(trend.value()));
        EXPECT_LE(std::abs(trend.value()), 1.0F);
    }
    meter.clearHistory();
    meter.setCorrelation(0.75F, 0.0, MAX_PERSISTENCE_SECONDS);
    expectTrend(meter, 0.75);
    expectRange(meter, 0.75F, 0.75F);
}

/// An isolated old excursion fades smoothly while its exact range remains held, and is gone at the same expiry.
TEST(BandCorrelationMeter, RangeFieldFadesBeforeExpiryWithoutExtendingPersistence)
{
    constexpr double persistenceSeconds {0.8};
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-1.0F, 0.0, persistenceSeconds);
    EXPECT_FLOAT_EQ(BandCorrelationMeterTestAccess::intensityAt(meter, -1.0F), 1.0F);
    for (int step = 1; step <= 4; ++step) {
        meter.setCorrelation(1.0F, 0.2, persistenceSeconds);
        const double remaining = 1.0 - static_cast<double>(step) * 0.25;
        const double expected = remaining * remaining * (3.0 - 2.0 * remaining);
        EXPECT_NEAR(BandCorrelationMeterTestAccess::intensityAt(meter, -1.0F), expected, 1.0e-6);
        EXPECT_FLOAT_EQ(BandCorrelationMeterTestAccess::intensityAt(meter, 1.0F), 1.0F);
        if (step < 4) {
            expectRange(meter, -1.0F, 1.0F);
        }
    }
    EXPECT_FLOAT_EQ(BandCorrelationMeterTestAccess::intensityAt(meter, -1.0F), 0.0F);
}

/// Refresh counts and bucket phases do not quantize the visual release of an isolated excursion.
TEST(BandCorrelationMeter, RangeFieldUsesObservationAgeIndependentlyOfRefreshRate)
{
    constexpr double persistenceSeconds {0.8};
    constexpr double observationPhaseSeconds {0.003};
    for (const int frames : {1, 24, 60, 144, 1000}) {
        SCOPED_TRACE(frames);
        BandCorrelationMeter meter("Low");
        meter.setCorrelation(-1.0F, observationPhaseSeconds, persistenceSeconds);
        for (int frame = 0; frame < frames; ++frame) {
            meter.setCorrelation(1.0F, persistenceSeconds * 0.5 / static_cast<double>(frames), persistenceSeconds);
        }
        EXPECT_NEAR(BandCorrelationMeterTestAccess::intensityAt(meter, -1.0F), 0.5F, 1.0e-6F);
    }
}

/// A recent minimum and maximum fade independently, rather than applying one opacity to a moving rectangle.
TEST(BandCorrelationMeter, RangeFieldHasIndependentFadingEdgesAndSoftSpatialFalloff)
{
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-1.0F, 0.0, 1.0);
    meter.setCorrelation(1.0F, 0.25, 1.0);
    meter.setCorrelation(0.0F, 0.25, 1.0);
    EXPECT_NEAR(BandCorrelationMeterTestAccess::intensityAt(meter, -1.0F), 0.5F, 1.0e-6F);
    EXPECT_NEAR(BandCorrelationMeterTestAccess::intensityAt(meter, 1.0F), 0.84375F, 1.0e-6F);
    EXPECT_FLOAT_EQ(BandCorrelationMeterTestAccess::intensityAt(meter, 0.0F), 1.0F);
    meter.clearHistory();
    EXPECT_TRUE(BandCorrelationMeterTestAccess::fieldIsClear(meter));
    meter.setCorrelation(0.0F);
    EXPECT_FLOAT_EQ(BandCorrelationMeterTestAccess::intensityAt(meter, 0.0F), 1.0F);
    const float feather = BandCorrelationMeterTestAccess::intensityAt(meter, 0.03125F);
    EXPECT_GT(feather, 0.0F);
    EXPECT_LT(feather, 1.0F);
    EXPECT_FLOAT_EQ(BandCorrelationMeterTestAccess::intensityAt(meter, 0.0625F), 0.0F);
}

/// Changing Persistence immediately reselects and reweights retained history without extending the selected window.
TEST(BandCorrelationMeter, RangeFieldReusesRetainedHistoryWhenPersistenceChanges)
{
    BandCorrelationMeter meter("Low");
    meter.setCorrelation(-1.0F, 0.0, MAX_PERSISTENCE_SECONDS);
    meter.setCorrelation(1.0F, 0.4, MAX_PERSISTENCE_SECONDS);
    meter.setCorrelation(1.0F, 0.0, 0.2);
    EXPECT_FLOAT_EQ(BandCorrelationMeterTestAccess::intensityAt(meter, -1.0F), 0.0F);
    meter.setCorrelation(1.0F, 0.0, 0.8);
    EXPECT_NEAR(BandCorrelationMeterTestAccess::intensityAt(meter, -1.0F), 0.5F, 1.0e-6F);
    meter.setCorrelation(std::numeric_limits<float>::quiet_NaN());
    EXPECT_TRUE(BandCorrelationMeterTestAccess::fieldIsClear(meter));
    meter.setCorrelation(1.0F, 0.0, MAX_PERSISTENCE_SECONDS);
    EXPECT_FLOAT_EQ(BandCorrelationMeterTestAccess::intensityAt(meter, -1.0F), 0.0F);
}

/// A separate observation log validates filled spatial coverage and independent age weighting across ring wraps.
TEST(BandCorrelationMeter, RangeFieldMatchesIndependentObservationCoverage)
{
    constexpr double refreshSeconds {1.0 / 64.0};
    constexpr std::array windows {MIN_PERSISTENCE_SECONDS, 0.25, DEFAULT_PERSISTENCE_SECONDS, MAX_PERSISTENCE_SECONDS};
    constexpr std::array probes {-1.0F, -0.5F, 0.0F, 0.5F, 1.0F};
    BandCorrelationMeter meter("Low");
    std::deque<std::pair<double, float>> observations;
    for (int reading = 0; reading < 2048; ++reading) {
        const double now = static_cast<double>(reading) * refreshSeconds;
        const double persistence = windows.at(static_cast<std::size_t>(reading / 257) % windows.size());
        const auto value = static_cast<float>(std::sin(static_cast<double>(reading) * 0.031));
        meter.setCorrelation(value, reading > 0 ? refreshSeconds : 0.0, persistence);
        observations.emplace_back(now, value);
        while (observations.front().first < now - MAX_PERSISTENCE_SECONDS) {
            observations.pop_front();
        }
        if (reading % 31 != 0) {
            continue;
        }
        SCOPED_TRACE(reading);
        for (const float probe : probes) {
            double expected = 0.0;
            for (const auto& [time, observed] : observations) {
                const double remaining = std::clamp(1.0 - (now - time) / persistence, 0.0, 1.0);
                const double opacity = remaining * remaining * (3.0 - 2.0 * remaining);
                const float distance
                    = std::max({std::min(observed, value) - probe, probe - std::max(observed, value), 0.0F});
                const double coverage = std::clamp(1.0 - static_cast<double>(distance) / 0.06, 0.0, 1.0);
                expected = std::max(expected, opacity * coverage * coverage * (3.0 - 2.0 * coverage));
            }
            EXPECT_NEAR(BandCorrelationMeterTestAccess::intensityAt(meter, probe), expected, 1.0e-6);
        }
    }
}

/// Software painting traverses unavailable, ranged, endpoint, and cleared states without pixel comparisons.
TEST(BandCorrelationMeter, SoftwarePaintPreservesHistoryState)
{
    BandCorrelationMeter meter("Low");
    meter.setSize(240, BAND_CORRELATION_HEIGHT);
    const juce::Image image(juce::Image::ARGB, meter.getWidth(), meter.getHeight(), true);
    juce::Graphics graphics(image);
    meter.paint(graphics);
    EXPECT_FALSE(meter.trendValue().has_value());
    EXPECT_FALSE(meter.recentRange().has_value());
    meter.setCorrelation(-1.0F);
    meter.paint(graphics);
    meter.setCorrelation(1.0F, DEFAULT_PERSISTENCE_SECONDS);
    meter.paint(graphics);
    EXPECT_EQ(meter.readoutText(), "+0.26");
    expectTrend(meter, 1.0 - 2.0 * std::exp(-1.0));
    expectRange(meter, -1.0F, 1.0F);
    meter.clearHistory();
    meter.paint(graphics);
    EXPECT_FALSE(meter.trendValue().has_value());
    EXPECT_FALSE(meter.recentRange().has_value());
}
}  // namespace esgrove::image_control::test
