/// Tests SpatialAnalyzer: the axis mappings,
/// and where hard-panned, centred, and anti-phase sines land in the spatial spectrogram,
/// with their energy and coherence.

#include "analysis/SpatialAnalyzer.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// The cell of the spectrogram with the most energy, with its energy and mean coherence.
struct Peak {
    int column {0};
    int row {0};
    float energy {0.0F};
    float coherence {0.0F};
};

/// Runs a second of stereo audio through a fresh analyzer and returns its strongest cell.
Peak analyze(const std::vector<float>& left, const std::vector<float>& right)
{
    SpatialAnalyzer analyzer;
    analyzer.prepare(TEST_SAMPLE_RATE);
    analyzer.setPersistence(0.2);
    analyzer.pushSamples(left, right);

    const auto energy = analyzer.getEnergy();
    const auto coherent = analyzer.getCoherentEnergy();
    Peak peak;
    for (std::size_t cell = 0; cell < energy.size(); ++cell) {
        if (energy[cell] > peak.energy) {
            peak.energy = energy[cell];
            peak.coherence = coherent[cell] / energy[cell];
            peak.column = static_cast<int>(cell) % SPECTROGRAM_COLUMNS;
            peak.row = static_cast<int>(cell) / SPECTROGRAM_COLUMNS;
        }
    }
    return peak;
}

/// Returns the column a 1 kHz bin lands in.
int columnOf1kHz()
{
    return static_cast<int>(SpatialAnalyzer::frequencyToColumn(1000.0));
}
}  // namespace

TEST(Analyzer, AxisMappingsRoundTrip)
{
    EXPECT_NEAR(SpatialAnalyzer::frequencyToColumn(20.0), 0.0, 1.0e-9);
    EXPECT_NEAR(SpatialAnalyzer::frequencyToColumn(20'000.0), SPECTROGRAM_COLUMNS, 1.0e-6);
    EXPECT_NEAR(SpatialAnalyzer::columnToFrequency(SpatialAnalyzer::frequencyToColumn(1234.0)), 1234.0, 1.0e-6);
    EXPECT_NEAR(SpatialAnalyzer::positionToRow(1.0), 0.0, 1.0e-12);
    EXPECT_NEAR(SpatialAnalyzer::positionToRow(-1.0), SPECTROGRAM_ROWS - 1, 1.0e-12);
    EXPECT_NEAR(SpatialAnalyzer::rowToPosition(SpatialAnalyzer::positionToRow(0.3)), 0.3, 1.0e-12);
}

TEST(Analyzer, HardLeftSineIsAtTheTop)
{
    const auto sine = makeSine(1000.0, 1.0F, 48'000);
    const auto peak = analyze(sine, std::vector<float>(sine.size(), 0.0F));
    EXPECT_NEAR(peak.column, columnOf1kHz(), 2);
    EXPECT_EQ(peak.row, 0);
    EXPECT_NEAR(peak.coherence, 1.0F, 1.0e-3F);
    EXPECT_GT(peak.energy, 0.3F);
    EXPECT_LT(peak.energy, 1.5F);
}

TEST(Analyzer, HardRightSineIsAtTheBottom)
{
    const auto sine = makeSine(1000.0, 1.0F, 48'000);
    const auto peak = analyze(std::vector<float>(sine.size(), 0.0F), sine);
    EXPECT_NEAR(peak.column, columnOf1kHz(), 2);
    EXPECT_EQ(peak.row, SPECTROGRAM_ROWS - 1);
}

TEST(Analyzer, CentredSineIsInPhaseAtTheCentre)
{
    const auto sine = makeSine(1000.0, 0.5F, 48'000);
    const auto peak = analyze(sine, sine);
    EXPECT_NEAR(peak.row, static_cast<double>(SPECTROGRAM_ROWS - 1) * 0.5, 1.0);
    EXPECT_NEAR(peak.coherence, 1.0F, 1.0e-3F);
}

TEST(Analyzer, AntiPhaseSineIsAtTheCentreWithNegativeCoherence)
{
    const auto sine = makeSine(1000.0, 0.5F, 48'000);
    std::vector<float> inverted(sine.size());
    std::ranges::transform(sine, inverted.begin(), [](const float value) { return -value; });
    const auto peak = analyze(sine, inverted);
    EXPECT_NEAR(peak.row, static_cast<double>(SPECTROGRAM_ROWS - 1) * 0.5, 1.0);
    EXPECT_NEAR(peak.coherence, -1.0F, 1.0e-3F);
}
}  // namespace esgrove::image_control::test
