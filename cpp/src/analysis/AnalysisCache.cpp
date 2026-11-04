/// Measurement and custom gain persistence of AnalysisCache.
/// Implements the load and store paths for analysis records and custom gains using prepared statements,
/// and the removal and pruning of rows for files that changed or no longer exist.
/// Database lifecycle and schema live in AnalysisCacheSchema.cpp, and native previews in AnalysisCacheWaveform.cpp.

#include "analysis/AnalysisCache.h"

#include "analysis/AnalysisCacheSql.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/OverloadHistory.h"
#include "analysis/Sqlite.h"
#include "analysis/SqliteTransaction.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sqlite3.h>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
constexpr int loudnessHistoryMagic {0x5346554c};
constexpr int loudnessHistoryVersion {1};
constexpr std::size_t loudnessHistoryHeaderBytes {32};
/// Practical per-record allocation budget, separate from SQLite's much larger blob limit.
constexpr std::size_t maximumHistoryBytes {16UZ * 1024 * 1024};
constexpr double maximumLoudness {1000.0};
constexpr double maximumDurationError {1.0e-9};
constexpr int maximumMeasurementSampleRate {2'822'400};
/// One cached row, identified by path and the file version it describes.
struct CachedRow {
    /// Path of the file the row describes.
    juce::String path;
    /// Size of the file when the row was stored.
    std::int64_t fileSize {0};
    /// Modification time of the file when the row was stored, in milliseconds since the epoch.
    std::int64_t modifiedTimeMs {0};
    /// The Serato tag kind, -1 for a Rust waveform, -2 for a spectrogram, or empty for an analysis row.
    std::optional<std::int64_t> tagKind;
};

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
            != juce::jmax(1, juce::roundToInt(history.sampleRate / ShortTermLoudnessHistory::updateRateHz)))
    {
        return false;
    }
    const auto expectedCount = history.lengthInSamples / history.samplesPerPoint
        + (history.lengthInSamples % history.samplesPerPoint != 0 ? 1 : 0);
    return count == expectedCount && count >= 0
        && std::cmp_less_equal(count, (maximumHistoryBytes - loudnessHistoryHeaderBytes) / sizeof(float));
}

/// Rejects non-finite readings and values outside the float audio signal's possible LUFS range.
/// The upper bound is deliberately above the roughly +800 LUFS representable by finite float PCM.
bool validHistoryValue(const float value)
{
    return std::isfinite(value) && static_cast<double>(value) >= AudioAnalysisRecord::negativeInfinityLoudness
        && static_cast<double>(value) <= maximumLoudness;
}

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
    output.writeInt(loudnessHistoryMagic);
    output.writeInt(loudnessHistoryVersion);
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
    const void* data,
    const std::size_t bytes,
    const AudioAnalysisRecord& record
)
{
    if (data == nullptr || bytes < loudnessHistoryHeaderBytes || bytes > maximumHistoryBytes) {
        return {};
    }
    juce::MemoryInputStream input(data, bytes, false);
    if (input.readInt() != loudnessHistoryMagic || input.readInt() != loudnessHistoryVersion) {
        return {};
    }
    ShortTermLoudnessHistory history;
    history.sampleRate = input.readDouble();
    history.lengthInSamples = input.readInt64();
    history.samplesPerPoint = input.readInt();
    const auto count = input.readInt();
    if (!validHistoryTiming(history, record, count)
        || bytes != loudnessHistoryHeaderBytes + static_cast<std::size_t>(count) * sizeof(float))
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

/// Converts a status read from the database, or returns an empty optional for a value outside the enum.
/// The database is outside the program's control,
/// and casting an unknown value into the enum would later reach std::unreachable in formatStatus.
std::optional<AudioAnalysisStatus> statusFromColumn(const int value)
{
    if (std::cmp_less(value, std::to_underlying(AudioAnalysisStatus::pending))
        || std::cmp_greater(value, std::to_underlying(AudioAnalysisStatus::failed)))
    {
        return std::nullopt;
    }

    return static_cast<AudioAnalysisStatus>(value);
}
/// Rejects SQLite coercions and values that cannot safely become a shared record.
bool validRecordColumns(sqlite3_stmt& statement)
{
    for (const auto column :
         {
             analysis_column::fileSize,
             analysis_column::modifiedTimeMs,
             analysis_column::lengthInSamples,
             analysis_column::sampleRate,
             analysis_column::channels,
             analysis_column::bitsPerSample,
             analysis_column::status,
             analysis_column::analysisVersion,
             analysis_column::hasCustomGain,
         })
    {
        if (sqlite3_column_type(&statement, column) != SQLITE_INTEGER) {
            return false;
        }
    }
    for (const auto column :
         {
             analysis_column::durationSeconds,
             analysis_column::peakLeft,
             analysis_column::peakRight,
             analysis_column::overallPeak,
             analysis_column::truePeakLeft,
             analysis_column::truePeakRight,
             analysis_column::overallTruePeak,
             analysis_column::maxShortTermLufs,
             analysis_column::integratedLufs,
             analysis_column::customGainDb,
         })
    {
        const auto type = sqlite3_column_type(&statement, column);
        if ((type != SQLITE_FLOAT && type != SQLITE_INTEGER)
            || !std::isfinite(sqlite3_column_double(&statement, column)))
        {
            return false;
        }
    }
    for (const auto column :
         {
             analysis_column::sampleRate,
             analysis_column::channels,
             analysis_column::bitsPerSample,
             analysis_column::status,
             analysis_column::analysisVersion,
             analysis_column::hasCustomGain,
         })
    {
        const auto value = sqlite3_column_int64(&statement, column);
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
            return false;
        }
    }
    for (const auto column :
         {
             analysis_column::peakLeft,
             analysis_column::peakRight,
             analysis_column::overallPeak,
             analysis_column::customGainDb,
         })
    {
        if (std::abs(sqlite3_column_double(&statement, column))
            > static_cast<double>(std::numeric_limits<float>::max()))
        {
            return false;
        }
    }
    return sqlite3_column_type(&statement, analysis_column::fileName) == SQLITE_TEXT
        && sqlite3_column_bytes(&statement, analysis_column::fileName) <= 4096
        && (sqlite3_column_type(&statement, analysis_column::formatName) == SQLITE_TEXT
            || sqlite3_column_type(&statement, analysis_column::formatName) == SQLITE_NULL)
        && sqlite3_column_bytes(&statement, analysis_column::formatName) <= 4096
        && (sqlite3_column_type(&statement, analysis_column::errorMessage) == SQLITE_TEXT
            || sqlite3_column_type(&statement, analysis_column::errorMessage) == SQLITE_NULL)
        && sqlite3_column_bytes(&statement, analysis_column::errorMessage) <= 65'536
        && sqlite3_column_type(&statement, analysis_column::measurementProducer) == SQLITE_TEXT
        && sqlite3_column_bytes(&statement, analysis_column::measurementProducer) <= 4;
}

/// Validates numerical measurements and geometry before adopting data from either implementation.
bool validMeasurements(const AudioAnalysisRecord& record)
{
    const auto validLoudness = [](const double value) {
        return std::isfinite(value) && value >= AudioAnalysisRecord::negativeInfinityLoudness
            && value <= maximumLoudness;
    };
    const auto validPeak = [](const double value) {
        return std::isfinite(value) && std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
    };
    if (record.fileSize < 0 || record.modifiedTimeMs < 0
        || (record.customGain && !std::isfinite(record.customGain->value)))
    {
        return false;
    }
    if (record.hasError()) {
        return true;
    }
    return record.isReady() && record.sampleRate >= 16 && record.sampleRate <= maximumMeasurementSampleRate
        && record.channels >= 1 && record.channels <= 64 && record.bitsPerSample >= 0 && record.bitsPerSample <= 64
        && record.lengthInSamples >= 0 && std::isfinite(record.durationSeconds)
        && std::abs(record.durationSeconds - static_cast<double>(record.lengthInSamples) / record.sampleRate)
        <= maximumDurationError
        && validPeak(static_cast<double>(record.peakLeft)) && validPeak(static_cast<double>(record.peakRight))
        && validPeak(static_cast<double>(record.overallPeak)) && std::isfinite(record.truePeakLeft)
        && std::isfinite(record.truePeakRight) && std::isfinite(record.overallTruePeak) && record.truePeakLeft >= 0.0
        && record.truePeakRight >= 0.0 && record.overallTruePeak >= 0.0 && validLoudness(record.maxShortTermLufs)
        && validLoudness(record.integratedLufs);
}
/// Restores required bounded histories independently of optional waveform and tag artifacts.
bool readRequiredHistories(sqlite3_stmt& statement, AudioAnalysisRecord& record)
{
    if (sqlite3_column_type(&statement, analysis_column::shortTermLoudness) == SQLITE_BLOB
        && sqlite3_column_bytes(&statement, analysis_column::shortTermLoudness)
            <= static_cast<int>(maximumHistoryBytes))
    {
        record.shortTermLoudness = deserializeLoudnessHistory(
            sqlite3_column_blob(&statement, analysis_column::shortTermLoudness),
            static_cast<std::size_t>(sqlite3_column_bytes(&statement, analysis_column::shortTermLoudness)),
            record
        );
    }
    if (!record.shortTermLoudness) {
        utils::logWarn(
            "Invalid cached short-term loudness history for {}, reanalysis required", record.fullPath.quoted()
        );
        return false;
    }
    constexpr auto maximumOverloadBytes = 40 + OverloadHistory::maximumBuckets * 64;
    if (sqlite3_column_type(&statement, analysis_column::overloadHistory) == SQLITE_BLOB
        && sqlite3_column_bytes(&statement, analysis_column::overloadHistory) <= static_cast<int>(maximumOverloadBytes))
    {
        record.overloads = deserializeOverloadHistory(
            std::span(
                static_cast<const std::byte*>(sqlite3_column_blob(&statement, analysis_column::overloadHistory)),
                static_cast<std::size_t>(sqlite3_column_bytes(&statement, analysis_column::overloadHistory))
            )
        );
    }
    if (!record.overloads || !overloadHistoryMatches(*record.overloads, record)) {
        utils::logWarn("Invalid cached overload history for {}, reanalysis required", record.fullPath.quoted());
        return false;
    }
    return true;
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
    constexpr int METRIC_VALUE_BYTES {8};
    const auto expected = column == analysis_column::dcOffsets && record.lengthInSamples == 0
        ? 0
        : record.channels * METRIC_VALUE_BYTES;
    if (sqlite3_column_type(&statement, column) != SQLITE_BLOB || sqlite3_column_bytes(&statement, column) != expected)
    {
        return false;
    }
    juce::MemoryInputStream input(sqlite3_column_blob(&statement, column), static_cast<std::size_t>(expected), false);
    for (int channel = 0; channel < expected / METRIC_VALUE_BYTES; ++channel) {
        if (column == analysis_column::dcOffsets) {
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

/// Reads bounded source statistics, rejecting malformed required data and invalid channel conventions.
bool readSignalMetrics(sqlite3_stmt& statement, AudioAnalysisRecord& record)
{
    if (record.channels < 1 || record.channels > 64) {
        return false;
    }
    auto& metrics = record.signalMetrics;
    for (const auto& [column, destination] :
         {
             std::pair {analysis_column::plrDb, &metrics.plrDb},
             std::pair {analysis_column::psrDb, &metrics.psrDb},
             std::pair {analysis_column::crestFactorDb, &metrics.crestFactorDb},
             std::pair {analysis_column::stereoCorrelation, &metrics.stereoCorrelation},
         })
    {
        if (!readMetricScalar(statement, column, *destination)) {
            return false;
        }
    }
    return readMetricArray(statement, analysis_column::dcOffsets, record)
        && readMetricArray(statement, analysis_column::clipCounts, record)
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
             std::pair {analysis_parameter::plrDb, metrics.plrDb},
             std::pair {analysis_parameter::psrDb, metrics.psrDb},
             std::pair {analysis_parameter::crestFactorDb, metrics.crestFactorDb},
             std::pair {analysis_parameter::stereoCorrelation, metrics.stereoCorrelation},
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
    return sqlite::bindBlob(statement, analysis_parameter::dcOffsets, offsets.getMemoryBlock())
        && sqlite::bindBlob(statement, analysis_parameter::clipCounts, counts.getMemoryBlock());
}

/// Refuses transient and foreign decoder failures rather than publishing them under this implementation's contract.
bool ignoresFailure(const AudioAnalysisRecord& record)
{
    return record.hasError()
        && (record.measurementProducer != MeasurementProducer::cpp
            || !(record.error.has_value() && isRepeatable(*record.error)));
}

/// Binds null failure histories or already validated successful histories without changing measurement provenance.
bool bindHistories(
    sqlite3_stmt& statement,
    const bool failed,
    const juce::MemoryBlock& loudness,
    const juce::MemoryBlock& overloads,
    const AudioAnalysisRecord& record
)
{
    return sqlite::bindNull(statement, analysis_parameter::waveformData)
        && (failed ? sqlite::bindNull(statement, analysis_parameter::shortTermLoudness)
                   : sqlite::bindBlob(statement, analysis_parameter::shortTermLoudness, loudness))
        && (failed ? sqlite::bindNull(statement, analysis_parameter::overloadHistory)
                   : sqlite::bindBlob(statement, analysis_parameter::overloadHistory, overloads))
        && bindSignalMetrics(statement, record);
}

/// Binds the previously observed row version so a different-source writer cannot lose publication.
bool bindObservedSource(sqlite3_stmt& statement, const std::optional<std::pair<std::int64_t, std::int64_t>>& source)
{
    const auto [size, modified] = source.value_or(std::pair<std::int64_t, std::int64_t> {});
    return sqlite::bindInt64(statement, analysis_parameter::observedSourcePresent, static_cast<int>(source.has_value()))
        && sqlite::bindInt64(statement, analysis_parameter::observedFileSize, size)
        && sqlite::bindInt64(statement, analysis_parameter::observedModifiedTimeMs, modified);
}

/// Reads the persisted source version with the caller's connection and returns false only for database errors.
bool readStoredSource(
    sqlite3& connection,
    sqlite::StatementCache& statements,
    const juce::String& path,
    std::optional<std::pair<std::int64_t, std::int64_t>>& source
)
{
    const auto statement
        = statements.borrow(connection, "SELECT file_size, modified_time_ms FROM file_analysis WHERE file_path = ?;");
    if (!statement.isValid() || !sqlite::bindText(*statement, 1, path)) {
        return false;
    }
    const auto result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) {
        source = std::pair {sqlite3_column_int64(statement.get(), 0), sqlite3_column_int64(statement.get(), 1)};
    }
    return result == SQLITE_ROW || result == SQLITE_DONE;
}

/// Removes previews for another source or geometry only after this transaction published measurements.
bool invalidateIncompatiblePreviews(
    sqlite3& connection,
    sqlite::StatementCache& statements,
    const AudioAnalysisRecord& record
)
{
    constexpr int channelsParameter {5};
    constexpr int durationParameter {6};
    constexpr auto waveformSql = R"SQL(
        DELETE FROM rust_waveform_cache WHERE file_path = ?
            AND (file_size <> ? OR modified_time_ms <> ? OR sample_rate <> ?
                OR num_channels <> ? OR duration_seconds <> ?);
    )SQL";
    const auto waveform = statements.borrow(connection, waveformSql);
    if (!waveform.isValid() || !sqlite::bindText(*waveform, 1, record.fullPath)
        || !sqlite::bindInt64(*waveform, 2, record.fileSize) || !sqlite::bindInt64(*waveform, 3, record.modifiedTimeMs)
        || !sqlite::bindInt64(*waveform, 4, record.sampleRate)
        || !sqlite::bindInt64(*waveform, channelsParameter, record.channels)
        || !sqlite::bindDouble(*waveform, durationParameter, record.durationSeconds)
        || !sqlite::stepToCompletion(*waveform))
    {
        return false;
    }
    constexpr auto tagsSql = R"SQL(
        DELETE FROM serato_tag_cache WHERE file_path = ? AND (file_size <> ? OR modified_time_ms <> ?);
    )SQL";
    const auto tags = statements.borrow(connection, tagsSql);
    return tags.isValid() && sqlite::bindText(*tags, 1, record.fullPath) && sqlite::bindInt64(*tags, 2, record.fileSize)
        && sqlite::bindInt64(*tags, 3, record.modifiedTimeMs) && sqlite::stepToCompletion(*tags);
}

/// Removes incompatible optional spectra after commit only while the published source and geometry still win.
/// The predicate and deletion share one statement so another connection's newer artifact cannot be removed.
bool invalidateIncompatibleSpectrograms(
    sqlite3& connection,
    sqlite::StatementCache& statements,
    const AudioAnalysisRecord& record
)
{
    constexpr int CHANNELS_PARAMETER {5};
    constexpr int LENGTH_PARAMETER {6};
    constexpr auto sql = R"SQL(
        DELETE FROM spectrogram_cache WHERE file_path = ?1
            AND (file_size <> ?2 OR modified_time_ms <> ?3 OR sample_rate <> ?4
                OR num_channels <> ?5 OR length_in_samples <> ?6)
            AND EXISTS (SELECT 1 FROM file_analysis AS a WHERE a.file_path = ?1
                AND a.file_size = ?2 AND a.modified_time_ms = ?3 AND a.sample_rate = ?4
                AND a.channels = ?5 AND a.length_in_samples = ?6);
    )SQL";
    const auto statement = statements.borrow(connection, sql);
    return statement.isValid() && sqlite::bindText(*statement, 1, record.fullPath)
        && sqlite::bindInt64(*statement, 2, record.fileSize) && sqlite::bindInt64(*statement, 3, record.modifiedTimeMs)
        && sqlite::bindInt64(*statement, 4, record.sampleRate)
        && sqlite::bindInt64(*statement, CHANNELS_PARAMETER, record.channels)
        && sqlite::bindInt64(*statement, LENGTH_PARAMETER, record.lengthInSamples)
        && sqlite::stepToCompletion(*statement);
}
}  // namespace

/// Runs the function with the open database connection and its prepared statements while holding the cache lock,
/// so code that keeps its own tables in this database, such as the Serato tag cache,
/// shares the connection, its statements, and its serialization with every other cache call.
/// Does nothing when the database cannot be opened.
void AnalysisCache::withDatabase(
    const std::function<void(sqlite3& database, sqlite::StatementCache& statements)>& function
)
{
    const juce::ScopedLock lock(mutex);
    if (database == nullptr && !openUnlocked()) {
        return;
    }

    function(*database, statements);
}

/// Loads the cached analysis record for the given file,
/// or returns an empty optional when there is none for the file's current size, modification time and version.
std::optional<AudioAnalysisRecord> AnalysisCache::getAnalysis(const juce::File& file)
{
    // The file system is queried before taking the lock, so a slow network share does not block other cache calls.
    auto record = AudioAnalysisRecord::fromFile(file);
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return std::nullopt;
    }

    constexpr auto sql = R"SQL(
        SELECT file_name, format_name, file_size, modified_time_ms, length_in_samples,
               duration_seconds, peak_left, peak_right, overall_peak, true_peak_left,
               true_peak_right, overall_true_peak, max_short_term_lufs, integrated_lufs,
               sample_rate, channels, bits_per_sample, status, error_message, analysis_version,
               custom_gain_db, has_custom_gain, short_term_loudness, overload_history, analysis_producer,
               plr_db, psr_db, crest_factor_db, dc_offsets, clip_counts, stereo_correlation
        FROM file_analysis
        WHERE file_path = ?;
    )SQL";

    const auto prepared = statements.borrow(*database, sql);
    if (!prepared.isValid() || !sqlite::bindText(*prepared, 1, normalizedPath(file))) {
        return std::nullopt;
    }

    auto* const statement = prepared.get();

    if (const auto stepResult = sqlite3_step(statement); stepResult != SQLITE_ROW) {
        return std::nullopt;
    }

    if (!validRecordColumns(*statement)) {
        return std::nullopt;
    }
    const auto producer = sqlite::columnText(*statement, analysis_column::measurementProducer);
    if (producer != "cpp" && producer != "rust") {
        return std::nullopt;
    }
    record.measurementProducer = producer == "cpp" ? MeasurementProducer::cpp : MeasurementProducer::rust;
    const auto cachedFileSize = sqlite3_column_int64(statement, analysis_column::fileSize);
    const auto cachedModifiedTime = sqlite3_column_int64(statement, analysis_column::modifiedTimeMs);
    const auto cachedVersion = sqlite3_column_int(statement, analysis_column::analysisVersion);

    if (cachedFileSize != record.fileSize || cachedModifiedTime != record.modifiedTimeMs
        || cachedVersion != analysisVersion)
    {
        return std::nullopt;
    }

    record.fileName = sqlite::columnText(*statement, analysis_column::fileName);
    record.formatName = sqlite::columnText(*statement, analysis_column::formatName);
    record.lengthInSamples = sqlite3_column_int64(statement, analysis_column::lengthInSamples);
    record.durationSeconds = sqlite3_column_double(statement, analysis_column::durationSeconds);
    record.peakLeft = static_cast<float>(sqlite3_column_double(statement, analysis_column::peakLeft));
    record.peakRight = static_cast<float>(sqlite3_column_double(statement, analysis_column::peakRight));
    record.overallPeak = static_cast<float>(sqlite3_column_double(statement, analysis_column::overallPeak));
    record.truePeakLeft = sqlite3_column_double(statement, analysis_column::truePeakLeft);
    record.truePeakRight = sqlite3_column_double(statement, analysis_column::truePeakRight);
    record.overallTruePeak = sqlite3_column_double(statement, analysis_column::overallTruePeak);
    record.maxShortTermLufs = sqlite3_column_double(statement, analysis_column::maxShortTermLufs);
    record.integratedLufs = sqlite3_column_double(statement, analysis_column::integratedLufs);
    record.sampleRate = sqlite3_column_int(statement, analysis_column::sampleRate);
    record.channels = sqlite3_column_int(statement, analysis_column::channels);
    record.bitsPerSample = sqlite3_column_int(statement, analysis_column::bitsPerSample);
    const auto status = statusFromColumn(sqlite3_column_int(statement, analysis_column::status));
    if (!status.has_value()) {
        utils::logWarn("Invalid cached analysis status for {}, reanalysis required", record.fullPath.quoted());
        return std::nullopt;
    }
    record.status = *status;
    if (record.hasError() && record.measurementProducer != MeasurementProducer::cpp) {
        return std::nullopt;
    }
    record.errorMessage = sqlite::columnText(*statement, analysis_column::errorMessage);
    // Only failures that repeat for the same bytes are stored.
    if (record.hasError()) {
        record.error = AnalysisError::unsupported;
    }
    const auto hasCustomGain = sqlite3_column_int64(statement, analysis_column::hasCustomGain);
    if (hasCustomGain != 0 && hasCustomGain != 1) {
        return std::nullopt;
    }
    if (hasCustomGain == 1) {
        record.customGain
            = Decibels {static_cast<float>(sqlite3_column_double(statement, analysis_column::customGainDb))};
    }
    if (!validMeasurements(record)) {
        return std::nullopt;
    }
    if (!record.hasError() && (!readRequiredHistories(*statement, record) || !readSignalMetrics(*statement, record))) {
        return std::nullopt;
    }
    record.fromCache = true;

    if (record.status != AudioAnalysisStatus::failed) {
        record.status = AudioAnalysisStatus::cached;
    }

    return record;
}

/// Stores the completed analysis record for a file.
/// An existing custom gain is kept while the file is unchanged on disk,
/// and replaced by the record's gain when the file's size or modification time differs,
/// since a custom gain only applies to the version of the file it was set for.
/// A failure is only stored when reading the same bytes again would repeat it.
/// Other failures, such as a cancelled analysis or a transient read error, leave the cache as it was
/// and return true, so the next run tries the file again.
bool AnalysisCache::storeAnalysis(const AudioAnalysisRecord& record)
{
    if (ignoresFailure(record)) {
        return true;
    }
    // Capture the row before filesystem validation, then compare it again inside the write transaction.
    // A writer that publishes a different source while this work waits must not be overwritten.
    std::optional<std::pair<std::int64_t, std::int64_t>> observedSource;
    bool observed = false;
    withDatabase([&](sqlite3& connection, sqlite::StatementCache& preparedStatements) {
        observed = readStoredSource(connection, preparedStatements, record.fullPath, observedSource);
    });
    // Filesystem work must precede the database lock and transaction.
    if (!observed || !validMeasurements(record) || !record.matchesFileOnDisk()) {
        return false;
    }
    const auto loudnessData = serializeLoudnessHistory(record);
    if (!record.hasError() && loudnessData.isEmpty()) {
        utils::logError("Cannot cache analysis without valid loudness history for {}", record.fullPath.quoted());
        return false;
    }
    const auto overloadData = record.overloads && overloadHistoryMatches(*record.overloads, record)
        ? serializeOverloadHistory(*record.overloads)
        : juce::MemoryBlock {};
    if (!record.hasError() && overloadData.isEmpty()) {
        utils::logError("Cannot cache analysis without valid overload history for {}", record.fullPath.quoted());
        return false;
    }
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    sqlite::Transaction transaction(*database);
    if (!transaction.isActive()) {
        return false;
    }
    constexpr auto sql = R"SQL(
        INSERT INTO file_analysis (
            file_path, file_name, format_name, file_size, modified_time_ms, length_in_samples,
            duration_seconds, peak_left, peak_right, overall_peak, true_peak_left,
            true_peak_right, overall_true_peak, max_short_term_lufs, integrated_lufs,
            sample_rate, channels, bits_per_sample, status, error_message,
            waveform_data, waveform_version, analysis_version, updated_at_ms,
            custom_gain_db, has_custom_gain, short_term_loudness, overload_history, analysis_producer,
            plr_db, psr_db, crest_factor_db, dc_offsets, clip_counts, stereo_correlation
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(file_path) DO UPDATE SET
            file_name = excluded.file_name,
            format_name = excluded.format_name,
            file_size = excluded.file_size,
            modified_time_ms = excluded.modified_time_ms,
            length_in_samples = excluded.length_in_samples,
            duration_seconds = excluded.duration_seconds,
            peak_left = excluded.peak_left,
            peak_right = excluded.peak_right,
            overall_peak = excluded.overall_peak,
            true_peak_left = excluded.true_peak_left,
            true_peak_right = excluded.true_peak_right,
            overall_true_peak = excluded.overall_true_peak,
            max_short_term_lufs = excluded.max_short_term_lufs,
            integrated_lufs = excluded.integrated_lufs,
            plr_db = excluded.plr_db, psr_db = excluded.psr_db, crest_factor_db = excluded.crest_factor_db,
            dc_offsets = excluded.dc_offsets, clip_counts = excluded.clip_counts,
            stereo_correlation = excluded.stereo_correlation,
            short_term_loudness = excluded.short_term_loudness,
            overload_history = excluded.overload_history,
            sample_rate = excluded.sample_rate,
            channels = excluded.channels,
            bits_per_sample = excluded.bits_per_sample,
            status = excluded.status,
            error_message = excluded.error_message,
            waveform_data = CASE
                WHEN file_analysis.file_size = excluded.file_size
                    AND file_analysis.modified_time_ms = excluded.modified_time_ms
                    AND file_analysis.length_in_samples = excluded.length_in_samples
                    AND file_analysis.sample_rate = excluded.sample_rate
                    AND file_analysis.channels = excluded.channels
                    AND file_analysis.duration_seconds = excluded.duration_seconds
                    AND file_analysis.waveform_version = 3
                    AND typeof(file_analysis.waveform_data) = 'blob'
                    AND length(file_analysis.waveform_data) BETWEEN 52 AND 16777268
                THEN file_analysis.waveform_data ELSE NULL END,
            waveform_version = CASE
                WHEN file_analysis.file_size = excluded.file_size
                    AND file_analysis.modified_time_ms = excluded.modified_time_ms
                    AND file_analysis.length_in_samples = excluded.length_in_samples
                    AND file_analysis.sample_rate = excluded.sample_rate
                    AND file_analysis.channels = excluded.channels
                    AND file_analysis.duration_seconds = excluded.duration_seconds
                THEN file_analysis.waveform_version ELSE 0 END,
            analysis_version = excluded.analysis_version,
            analysis_producer = excluded.analysis_producer,
            updated_at_ms = excluded.updated_at_ms,
            custom_gain_db = CASE
                WHEN file_analysis.file_size = excluded.file_size
                    AND file_analysis.modified_time_ms = excluded.modified_time_ms
                THEN file_analysis.custom_gain_db
                ELSE excluded.custom_gain_db
            END,
            has_custom_gain = CASE
                WHEN file_analysis.file_size = excluded.file_size
                    AND file_analysis.modified_time_ms = excluded.modified_time_ms
                THEN file_analysis.has_custom_gain
                ELSE excluded.has_custom_gain
            END
        WHERE NOT (excluded.status = 3
            AND file_analysis.file_size = excluded.file_size
            AND file_analysis.modified_time_ms = excluded.modified_time_ms
            AND file_analysis.status IN (1, 2)
            AND file_analysis.analysis_version = excluded.analysis_version
            AND file_analysis.analysis_producer IN ('cpp', 'rust'))
            AND ((file_analysis.file_size = excluded.file_size
                    AND file_analysis.modified_time_ms = excluded.modified_time_ms)
                OR (? AND file_analysis.file_size = ? AND file_analysis.modified_time_ms = ?));
    )SQL";

    const auto prepared = statements.borrow(*database, sql);
    if (!prepared.isValid()) {
        return false;
    }

    auto& statement = *prepared;
    const bool textBound = sqlite::bindText(statement, analysis_parameter::filePath, record.fullPath)
        && sqlite::bindText(statement, analysis_parameter::fileName, record.fileName)
        && sqlite::bindText(statement, analysis_parameter::formatName, record.formatName)
        && sqlite::bindText(statement, analysis_parameter::errorMessage, record.errorMessage)
        && sqlite::bindText(statement,
                            analysis_parameter::measurementProducer,
                            record.measurementProducer == MeasurementProducer::cpp ? "cpp" : "rust");
    const bool integersBound = sqlite::bindInt64(statement, analysis_parameter::fileSize, record.fileSize)
        && sqlite::bindInt64(statement, analysis_parameter::modifiedTimeMs, record.modifiedTimeMs)
        && sqlite::bindInt64(statement, analysis_parameter::lengthInSamples, record.lengthInSamples)
        && sqlite::bindInt64(statement, analysis_parameter::sampleRate, record.sampleRate)
        && sqlite::bindInt64(statement, analysis_parameter::channels, record.channels)
        && sqlite::bindInt64(statement, analysis_parameter::bitsPerSample, record.bitsPerSample)
        && sqlite::bindInt64(statement, analysis_parameter::status, std::to_underlying(record.status))
        && sqlite::bindInt64(statement, analysis_parameter::waveformVersion, 0)
        && sqlite::bindInt64(statement, analysis_parameter::analysisVersion, analysisVersion)
        && sqlite::bindInt64(statement, analysis_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds())
        && sqlite::bindInt64(statement, analysis_parameter::hasCustomGain, record.customGain.has_value() ? 1 : 0);
    const bool numbersBound = sqlite::bindDouble(statement, analysis_parameter::durationSeconds, record.durationSeconds)
        && sqlite::bindDouble(statement, analysis_parameter::peakLeft, static_cast<double>(record.peakLeft))
        && sqlite::bindDouble(statement, analysis_parameter::peakRight, static_cast<double>(record.peakRight))
        && sqlite::bindDouble(statement, analysis_parameter::overallPeak, static_cast<double>(record.overallPeak))
        && sqlite::bindDouble(statement, analysis_parameter::truePeakLeft, record.truePeakLeft)
        && sqlite::bindDouble(statement, analysis_parameter::truePeakRight, record.truePeakRight)
        && sqlite::bindDouble(statement, analysis_parameter::overallTruePeak, record.overallTruePeak)
        && sqlite::bindDouble(statement, analysis_parameter::maxShortTermLufs, record.maxShortTermLufs)
        && sqlite::bindDouble(statement, analysis_parameter::integratedLufs, record.integratedLufs)
        && sqlite::bindDouble(statement,
                              analysis_parameter::customGainDb,
                              static_cast<double>(record.customGain.value_or(Decibels {}).value));
    const bool blobsBound = bindHistories(statement, record.hasError(), loudnessData, overloadData, record);

    const bool sourceBound = bindObservedSource(statement, observedSource);
    if (!(textBound && integersBound && numbersBound && blobsBound && sourceBound
          && sqlite::stepToCompletion(statement)))
    {
        return false;
    }
    const bool published = sqlite3_changes(database.get()) > 0;
    if (published && !invalidateIncompatiblePreviews(*database, statements, record)) {
        return false;
    }
    if (!transaction.commit()) {
        return false;
    }
    if (published && !invalidateIncompatibleSpectrograms(*database, statements, record)) {
        utils::logWarn("Cannot remove incompatible cached spectrograms for {}", record.fullPath.quoted());
    }
    return true;
}

/// Loads the custom gain stored for a file when the stored row has the given size and modification time,
/// so a fresh analysis of an unchanged file keeps the gain the user set.
/// Returns an empty optional when the file has no row or no custom gain,
/// or when the row describes another version of the file.
std::optional<Decibels> AnalysisCache::getCustomGain(
    const juce::File& file,
    const std::int64_t fileSize,
    const std::int64_t modifiedTimeMs
)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return std::nullopt;
    }

    constexpr auto sql = R"SQL(
        SELECT custom_gain_db FROM file_analysis
        WHERE file_path = ? AND file_size = ? AND modified_time_ms = ? AND has_custom_gain = 1;
    )SQL";

    const auto statement = statements.borrow(*database, sql);
    if (!statement.isValid() || !sqlite::bindText(*statement, 1, normalizedPath(file))
        || !sqlite::bindInt64(*statement, 2, fileSize) || !sqlite::bindInt64(*statement, 3, modifiedTimeMs)
        || sqlite3_step(statement.get()) != SQLITE_ROW)
    {
        return std::nullopt;
    }

    const auto type = sqlite3_column_type(statement.get(), 0);
    const auto gain = sqlite3_column_double(statement.get(), 0);
    return (type == SQLITE_INTEGER || type == SQLITE_FLOAT) && std::isfinite(gain)
            && std::abs(gain) <= static_cast<double>(std::numeric_limits<float>::max())
        ? std::optional {Decibels {static_cast<float>(gain)}}
        : std::nullopt;
}

/// Updates only the user-controlled gain fields for a file.
/// Returns true when a cached row was updated.
/// A file without a cached row, such as one still waiting for its analysis, keeps no gain in the cache.
bool AnalysisCache::storeCustomGain(const juce::File& file, const std::optional<Decibels> customGain)
{
    return storeCustomGains({
               {
                   .file = file,
                   .customGain = customGain,
                   .fileSize = file.getSize(),
                   .modifiedTimeMs = file.getLastModificationTime().toMilliseconds(),
               },
           })
        > 0;
}

/// Updates the user-controlled gain fields of several files in one transaction with one prepared statement,
/// so moving the gain slider with many rows selected costs one commit per slider step instead of one per file.
/// Either every update is stored or none is.
/// Returns the number of cached rows that were updated.
/// A file without a cached row, such as one still waiting for its analysis, keeps no gain in the cache.
int AnalysisCache::storeCustomGains(const std::vector<CustomGainUpdate>& updates)
{
    if (updates.empty()) {
        return 0;
    }
    // Compatibility callers capture identity synchronously. Queued GUI edits supply their original identity.
    auto capturedUpdates = updates;
    for (auto& update : capturedUpdates) {
        if (update.customGain && !std::isfinite(update.customGain->value)) {
            return 0;
        }
        if (update.fileSize < 0 || update.modifiedTimeMs < 0) {
            update.fileSize = update.file.getSize();
            update.modifiedTimeMs = update.file.getLastModificationTime().toMilliseconds();
        }
    }

    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return 0;
    }

    sqlite::Transaction transaction(*database);
    if (!transaction.isActive()) {
        return 0;
    }

    constexpr auto sql = R"SQL(
        UPDATE file_analysis
        SET custom_gain_db = ?, has_custom_gain = ?, updated_at_ms = ?
        WHERE file_path = ? AND file_size = ? AND modified_time_ms = ?;
    )SQL";

    const auto statement = statements.borrow(*database, sql);
    if (!statement.isValid()) {
        return 0;
    }

    const auto updatedAtMs = juce::Time::getCurrentTime().toMilliseconds();
    int updatedRows = 0;

    for (const auto& [file, customGain, fileSize, modifiedTimeMs] : capturedUpdates) {
        sqlite3_reset(statement.get());
        const bool stored
            = sqlite::bindDouble(*statement, 1, static_cast<double>(customGain.value_or(Decibels {}).value))
            && sqlite::bindInt64(*statement, 2, customGain.has_value() ? 1 : 0)
            && sqlite::bindInt64(*statement, 3, updatedAtMs) && sqlite::bindText(*statement, 4, normalizedPath(file))
            && sqlite::bindInt64(*statement, 5, fileSize) && sqlite::bindInt64(*statement, 6, modifiedTimeMs)
            && sqlite::stepToCompletion(*statement);

        if (!stored) {
            return 0;
        }

        updatedRows += sqlite3_changes(database.get());
    }

    return transaction.commit() ? updatedRows : 0;
}

/// Captures the persisted source version, then removes only that version in a short transaction.
/// Callers with an earlier captured record should use the record overload instead.
bool AnalysisCache::removeAnalysis(const juce::File& file)
{
    auto captured = AudioAnalysisRecord::fromFile(file);
    withDatabase([&](sqlite3& connection, sqlite::StatementCache& preparedStatements) {
        const auto statement = preparedStatements.borrow(
            connection,
            "SELECT file_size, modified_time_ms FROM file_analysis WHERE file_path = ?1 "
            "UNION ALL SELECT file_size, modified_time_ms FROM serato_tag_cache WHERE file_path = ?1 "
            "UNION ALL SELECT file_size, modified_time_ms FROM rust_waveform_cache WHERE file_path = ?1 "
            "UNION ALL SELECT file_size, modified_time_ms FROM spectrogram_cache WHERE file_path = ?1 LIMIT 1;"
        );
        if (statement.isValid() && sqlite::bindText(*statement, 1, normalizedPath(file))
            && sqlite3_step(statement.get()) == SQLITE_ROW)
        {
            captured.fileSize = sqlite3_column_int64(statement.get(), 0);
            captured.modifiedTimeMs = sqlite3_column_int64(statement.get(), 1);
        }
    });
    return removeAnalysis(captured);
}

/// Removes the captured analysis, waveforms, spectrogram revisions and matching independently cached tags atomically.
/// Delayed completion cannot delete a newer source row or its preview.
bool AnalysisCache::removeAnalysis(const AudioAnalysisRecord& record)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    sqlite::Transaction transaction(*database);
    if (!transaction.isActive()) {
        return false;
    }

    for (const auto* sql :
         {
             "DELETE FROM file_analysis WHERE file_path = ? AND file_size = ? AND modified_time_ms = ?;",
             "DELETE FROM serato_tag_cache WHERE file_path = ? AND file_size = ? AND modified_time_ms = ?;",
             "DELETE FROM rust_waveform_cache WHERE file_path = ? AND file_size = ? AND modified_time_ms = ?;",
             "DELETE FROM spectrogram_cache WHERE file_path = ? AND file_size = ? AND modified_time_ms = ?;",
         })
    {
        const auto statement = statements.borrow(*database, sql);
        if (!statement.isValid() || !sqlite::bindText(*statement, 1, record.fullPath)
            || !sqlite::bindInt64(*statement, 2, record.fileSize)
            || !sqlite::bindInt64(*statement, 3, record.modifiedTimeMs) || !sqlite::stepToCompletion(*statement))
        {
            return false;
        }
    }

    return transaction.commit();
}

/// Deletes the rows of files that no longer exist, for example after they were trashed or renamed.
/// Rows whose folder is missing are kept, since that usually means an unmounted drive, not deleted files.
/// A drive whose empty mount point folder stays behind, such as after an unclean eject,
/// looks like deleted files in that folder, so the rows of files directly in it are removed.
/// A row is only deleted while it still describes the version that was checked,
/// so a file recreated in the meantime keeps its new row.
/// The file system checks run without holding the database lock, so other cache calls are not blocked by them.
/// Stops early, keeping the remaining rows, when shouldStop returns true.
/// Returns the number of deleted rows across analysis and tag tables, including tag-only files.
int AnalysisCache::pruneMissingFiles(const std::function<bool()>& shouldStop)
{
    std::vector<CachedRow> cachedRows;

    {
        const juce::ScopedLock lock(mutex);

        if (database == nullptr && !openUnlocked()) {
            return 0;
        }

        constexpr auto sql = R"SQL(
            SELECT file_path, file_size, modified_time_ms, NULL FROM file_analysis
            UNION ALL
            SELECT file_path, file_size, modified_time_ms, tag_kind FROM serato_tag_cache
            UNION ALL
            SELECT file_path, file_size, modified_time_ms, -1 FROM rust_waveform_cache
            UNION ALL
            SELECT DISTINCT file_path, file_size, modified_time_ms, -2 FROM spectrogram_cache;
        )SQL";
        const auto prepared = sqlite::prepare(*database, sql);
        if (prepared == nullptr) {
            return 0;
        }

        auto* const statement = prepared.get();

        while (sqlite3_step(statement) == SQLITE_ROW) {
            cachedRows.push_back({
                .path = sqlite::columnText(*statement, 0),
                .fileSize = sqlite3_column_int64(statement, 1),
                .modifiedTimeMs = sqlite3_column_int64(statement, 2),
                .tagKind = sqlite3_column_type(statement, 3) == SQLITE_NULL
                    ? std::nullopt
                    : std::optional {sqlite3_column_int64(statement, 3)},
            });
        }
    }

    int removedRows = 0;

    for (const auto& [path, fileSize, modifiedTimeMs, tagKind] : cachedRows) {
        if (shouldStop != nullptr && shouldStop()) {
            break;
        }

        if (!juce::File::isAbsolutePath(path)) {
            continue;
        }

        // A missing folder usually means an unmounted drive or network share rather than deleted files,
        // and those rows hold custom gains that would be lost for good.
        const juce::File file(path);
        if (file.existsAsFile() || !file.getParentDirectory().isDirectory()) {
            continue;
        }

        if (removeRowVersion(path, fileSize, modifiedTimeMs, tagKind)) {
            ++removedRows;
        }
    }

    if (removedRows > 0) {
        utils::logInfo("Removed {} cached analysis and tag rows for files that no longer exist", removedRows);
    }

    return removedRows;
}

/// Deletes an analysis row, or the specified tag kind, only while its checked size and time still match.
/// Returns true when a row was deleted.
bool AnalysisCache::removeRowVersion(
    const juce::String& path,
    const std::int64_t fileSize,
    const std::int64_t modifiedTimeMs,
    const std::optional<std::int64_t> tagKind
)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr) {
        return false;
    }

    sqlite::Transaction transaction(*database);
    if (!transaction.isActive()) {
        return false;
    }
    const bool rustWaveform = tagKind == std::optional<std::int64_t> {-1};
    const bool spectrogram = tagKind == std::optional<std::int64_t> {-2};
    const char* sql = "DELETE FROM file_analysis WHERE file_path = ? AND file_size = ? AND modified_time_ms = ?;";
    if (rustWaveform) {
        sql = "DELETE FROM rust_waveform_cache WHERE file_path = ? AND file_size = ? AND modified_time_ms = ?;";
    } else if (spectrogram) {
        sql = "DELETE FROM spectrogram_cache WHERE file_path = ? AND file_size = ? AND modified_time_ms = ?;";
    } else if (tagKind.has_value()) {
        sql = "DELETE FROM serato_tag_cache WHERE file_path = ? AND file_size = ? AND modified_time_ms = ? AND "
              "tag_kind = ?;";
    }

    const auto statement = statements.borrow(*database, sql);
    if (!statement.isValid()) {
        return false;
    }

    const bool bound = sqlite::bindText(*statement, 1, path) && sqlite::bindInt64(*statement, 2, fileSize)
        && sqlite::bindInt64(*statement, 3, modifiedTimeMs)
        && (!tagKind.has_value() || rustWaveform || spectrogram || sqlite::bindInt64(*statement, 4, *tagKind));

    if (!bound || !sqlite::stepToCompletion(*statement)) {
        return false;
    }
    const bool removed = sqlite3_changes(database.get()) > 0;
    return transaction.commit() && removed;
}

}  // namespace audiobatch
