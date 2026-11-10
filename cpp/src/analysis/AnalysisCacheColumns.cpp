/// Codecs of the shared analysis cache's measurement columns.
/// The loudness history is an explicit little-endian header with IEEE float readings,
/// the source-statistics scalars are nullable finite doubles,
/// and the channel arrays hold one little-endian value per channel.
/// Each reader validates against its record, so a malformed row reads as a cache miss instead of a partial record.

#include "analysis/AnalysisCacheColumns.h"

#include "analysis/AnalysisCacheSql.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/Sqlite.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sqlite3.h>
#include <utility>

namespace audiobatch
{
namespace
{
/// Bytes of one DC offset or clip count in the channel arrays.
constexpr int METRIC_VALUE_BYTES {8};

/// Header fields of the shared loudness history blob, "LUFS" in little-endian ASCII, its version, and its size.
constexpr int LOUDNESS_HISTORY_MAGIC {0x5346554c};
constexpr int LOUDNESS_HISTORY_VERSION {1};
constexpr std::size_t LOUDNESS_HISTORY_HEADER_BYTES {32};

/// Checks timing and count before allocating or serializing cached history.
bool validHistoryTiming(
    const ShortTermLoudnessHistory& history,
    const AudioAnalysisRecord& record,
    const std::int64_t count
)
{
    if (!std::isfinite(history.sampleRate) || history.sampleRate < 1.0
        || history.sampleRate > static_cast<double>(std::numeric_limits<int>::max())
        || std::bit_cast<std::uint64_t>(history.sampleRate)
            != std::bit_cast<std::uint64_t>(static_cast<double>(record.sampleRate))
        || history.lengthInSamples < 0 || history.lengthInSamples != record.lengthInSamples
        || !juce::approximatelyEqual(
            static_cast<double>(history.lengthInSamples) / history.sampleRate, record.durationSeconds
        )
        || history.samplesPerPoint
            != juce::jmax(1, juce::roundToInt(history.sampleRate / ShortTermLoudnessHistory::UPDATE_RATE_HZ)))
    {
        return false;
    }
    const auto expectedCount = history.lengthInSamples / history.samplesPerPoint
        + (history.lengthInSamples % history.samplesPerPoint != 0 ? 1 : 0);
    return count == expectedCount && count >= 0
        && std::cmp_less_equal(count, (MAXIMUM_HISTORY_BYTES - LOUDNESS_HISTORY_HEADER_BYTES) / sizeof(float));
}

/// Rejects non-finite readings and values outside the float audio signal's possible LUFS range.
/// The upper bound is deliberately above the roughly +800 LUFS representable by finite float PCM.
bool validHistoryValue(const float value)
{
    return std::isfinite(value) && static_cast<double>(value) >= AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS
        && static_cast<double>(value) <= MAXIMUM_CACHED_LOUDNESS;
}

/// Reads nullable finite scalars without SQLite text coercions.
bool readMetricScalar(sqlite3_stmt& statement, const int column, std::optional<double>& destination)
{
    const auto type = sqlite3_column_type(&statement, column);
    if (type == SQLITE_NULL) {
        return true;
    }
    const auto value = sqlite3_column_double(&statement, column);
    if ((type != SQLITE_FLOAT && type != SQLITE_INTEGER) || !std::isfinite(value)) {
        return false;
    }
    destination = value;
    return true;
}

/// Reads a required little-endian channel array after checking its exact bounded size.
bool readMetricArray(sqlite3_stmt& statement, const int column, AudioAnalysisRecord& record)
{
    const auto expected = column == analysis_column::DC_OFFSETS && record.lengthInSamples == 0
        ? 0
        : record.channels * METRIC_VALUE_BYTES;
    if (sqlite3_column_type(&statement, column) != SQLITE_BLOB || sqlite3_column_bytes(&statement, column) != expected)
    {
        return false;
    }
    juce::MemoryInputStream input(sqlite3_column_blob(&statement, column), static_cast<std::size_t>(expected), false);
    for (int channel = 0; channel < expected / METRIC_VALUE_BYTES; ++channel) {
        if (column == analysis_column::DC_OFFSETS) {
            const auto value = input.readDouble();
            if (!std::isfinite(value) || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
                return false;
            }
            record.signalMetrics.dcOffsets.push_back(value);
        } else {
            const auto value = input.readInt64();
            if (value < 0 || value > record.lengthInSamples) {
                return false;
            }
            record.signalMetrics.clipCounts.push_back(static_cast<std::uint64_t>(value));
        }
    }
    return true;
}
}  // namespace

/// Writes an explicit little-endian header and IEEE float readings, never native struct padding.
juce::MemoryBlock serializeLoudnessHistory(const AudioAnalysisRecord& record)
{
    juce::MemoryBlock data;
    if (!record.shortTermLoudness) {
        return data;
    }
    const auto& history = *record.shortTermLoudness;
    if (!validHistoryTiming(history, record, static_cast<std::int64_t>(history.values.size()))
        || !std::ranges::all_of(history.values, validHistoryValue))
    {
        return data;
    }
    juce::MemoryOutputStream output(data, false);
    output.writeInt(LOUDNESS_HISTORY_MAGIC);
    output.writeInt(LOUDNESS_HISTORY_VERSION);
    output.writeDouble(history.sampleRate);
    output.writeInt64(history.lengthInSamples);
    output.writeInt(history.samplesPerPoint);
    output.writeInt(static_cast<int>(history.values.size()));
    for (const auto value : history.values) {
        output.writeFloat(value);
    }
    return data;
}

/// Validates the complete payload against its analysis record before allocating its sample vector.
std::shared_ptr<const ShortTermLoudnessHistory> deserializeLoudnessHistory(
    const std::span<const std::byte> data,
    const AudioAnalysisRecord& record
)
{
    if (data.size() < LOUDNESS_HISTORY_HEADER_BYTES || data.size() > MAXIMUM_HISTORY_BYTES) {
        return {};
    }
    juce::MemoryInputStream input(data.data(), data.size(), false);
    if (input.readInt() != LOUDNESS_HISTORY_MAGIC || input.readInt() != LOUDNESS_HISTORY_VERSION) {
        return {};
    }
    ShortTermLoudnessHistory history;
    history.sampleRate = input.readDouble();
    history.lengthInSamples = input.readInt64();
    history.samplesPerPoint = input.readInt();
    const auto count = input.readInt();
    if (!validHistoryTiming(history, record, count)
        || data.size() != LOUDNESS_HISTORY_HEADER_BYTES + static_cast<std::size_t>(count) * sizeof(float))
    {
        return {};
    }
    history.values.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const auto value = input.readFloat();
        if (!validHistoryValue(value)) {
            return {};
        }
        history.values.push_back(value);
    }
    return std::make_shared<const ShortTermLoudnessHistory>(std::move(history));
}

/// Reads bounded source statistics, rejecting malformed required data and invalid channel conventions.
/// Mono records must report a correlation of +1, and only mono or stereo records may report one at all.
bool readSignalMetrics(sqlite3_stmt& statement, AudioAnalysisRecord& record)
{
    if (record.channels < 1 || record.channels > MAXIMUM_CACHED_CHANNELS) {
        return false;
    }
    auto& metrics = record.signalMetrics;
    for (const auto& [column, destination] :
         {
             std::pair {analysis_column::PLR_DB, &metrics.plrDb},
             std::pair {analysis_column::PSR_DB, &metrics.psrDb},
             std::pair {analysis_column::CREST_FACTOR_DB, &metrics.crestFactorDb},
             std::pair {analysis_column::STEREO_CORRELATION, &metrics.stereoCorrelation},
         })
    {
        if (!readMetricScalar(statement, column, *destination)) {
            return false;
        }
    }
    return readMetricArray(statement, analysis_column::DC_OFFSETS, record)
        && readMetricArray(statement, analysis_column::CLIP_COUNTS, record)
        && (!metrics.stereoCorrelation || (std::abs(*metrics.stereoCorrelation) <= 1.0 && record.channels <= 2))
        && (record.channels != 1
            || (metrics.stereoCorrelation && juce::approximatelyEqual(*metrics.stereoCorrelation, 1.0)));
}

/// Binds nullable source ratios and bounded channel arrays in the shared little-endian layout.
bool bindSignalMetrics(sqlite3_stmt& statement, const AudioAnalysisRecord& record)
{
    const auto& metrics = record.signalMetrics;
    for (const auto& [parameter, value] :
         {
             std::pair {analysis_parameter::PLR_DB, metrics.plrDb},
             std::pair {analysis_parameter::PSR_DB, metrics.psrDb},
             std::pair {analysis_parameter::CREST_FACTOR_DB, metrics.crestFactorDb},
             std::pair {analysis_parameter::STEREO_CORRELATION, metrics.stereoCorrelation},
         })
    {
        if (!(value ? sqlite::bindDouble(statement, parameter, *value) : sqlite::bindNull(statement, parameter))) {
            return false;
        }
    }
    juce::MemoryOutputStream offsets;
    for (const auto value : metrics.dcOffsets) {
        offsets.writeDouble(value);
    }
    juce::MemoryOutputStream counts;
    for (const auto value : metrics.clipCounts) {
        counts.writeInt64(static_cast<std::int64_t>(value));
    }
    return sqlite::bindBlob(statement, analysis_parameter::DC_OFFSETS, offsets.getMemoryBlock())
        && sqlite::bindBlob(statement, analysis_parameter::CLIP_COUNTS, counts.getMemoryBlock());
}
}  // namespace audiobatch
