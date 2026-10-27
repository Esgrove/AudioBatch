/// Analysis and waveform persistence of AnalysisCache.
/// Implements the load and store paths for analysis records,
/// waveform previews and custom gain values using prepared statements,
/// and the removal and pruning of rows for files that changed or no longer exist.
/// The database lifecycle and schema live in AnalysisCacheSchema.cpp.

#include "analysis/AnalysisCache.h"

#include "analysis/AnalysisCacheSql.h"
#include "analysis/SqliteTransaction.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

/// One cached row, identified by path and the file version it describes.
struct CachedRow {
    /// Path of the file the row describes.
    juce::String path;
    /// Size of the file when the row was stored.
    std::int64_t fileSize {0};
    /// Modification time of the file when the row was stored, in milliseconds since the epoch.
    std::int64_t modifiedTimeMs {0};
    /// The Serato tag kind of a tag row, or empty for an analysis row.
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
        || juce::roundToInt(history.sampleRate) != record.sampleRate || history.lengthInSamples < 0
        || history.lengthInSamples != record.lengthInSamples
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
        && count <= (std::numeric_limits<int>::max() - static_cast<std::int64_t>(loudnessHistoryHeaderBytes))
            / static_cast<std::int64_t>(sizeof(float));
}

/// Rejects non-finite readings and values outside the float audio signal's possible LUFS range.
/// The upper bound is deliberately above the roughly +800 LUFS representable by finite float PCM.
bool validHistoryValue(const float value)
{
    constexpr float maximumLoudness {1000.0f};
    return std::isfinite(value) && static_cast<double>(value) >= AudioAnalysisRecord::negativeInfinityLoudness
        && value <= maximumLoudness;
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
    if (data == nullptr || bytes < loudnessHistoryHeaderBytes) {
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
               custom_gain_db, has_custom_gain, short_term_loudness
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
    record.errorMessage = sqlite::columnText(*statement, analysis_column::errorMessage);
    // Only failures that repeat for the same bytes are stored.
    if (record.hasError()) {
        record.error = AnalysisError::unsupported;
    }
    if (sqlite3_column_int(statement, analysis_column::hasCustomGain) != 0) {
        record.customGain
            = Decibels {static_cast<float>(sqlite3_column_double(statement, analysis_column::customGainDb))};
    }
    if (!record.hasError()) {
        if (sqlite3_column_type(statement, analysis_column::shortTermLoudness) == SQLITE_BLOB) {
            record.shortTermLoudness = deserializeLoudnessHistory(
                sqlite3_column_blob(statement, analysis_column::shortTermLoudness),
                static_cast<std::size_t>(sqlite3_column_bytes(statement, analysis_column::shortTermLoudness)),
                record
            );
        }
        if (!record.shortTermLoudness) {
            utils::logWarn(
                "Invalid cached short-term loudness history for {}, reanalysis required", record.fullPath.quoted()
            );
            return std::nullopt;
        }
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
    if (record.hasError() && !(record.error.has_value() && isRepeatable(*record.error))) {
        return true;
    }
    const auto loudnessData = serializeLoudnessHistory(record);
    if (!record.hasError() && loudnessData.isEmpty()) {
        utils::logError("Cannot cache analysis without valid loudness history for {}", record.fullPath.quoted());
        return false;
    }
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        INSERT INTO file_analysis (
            file_path, file_name, format_name, file_size, modified_time_ms, length_in_samples,
            duration_seconds, peak_left, peak_right, overall_peak, true_peak_left,
            true_peak_right, overall_true_peak, max_short_term_lufs, integrated_lufs,
            sample_rate, channels, bits_per_sample, status, error_message,
            waveform_data, waveform_version, analysis_version, updated_at_ms,
            custom_gain_db, has_custom_gain, short_term_loudness
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
            short_term_loudness = excluded.short_term_loudness,
            sample_rate = excluded.sample_rate,
            channels = excluded.channels,
            bits_per_sample = excluded.bits_per_sample,
            status = excluded.status,
            error_message = excluded.error_message,
            waveform_data = excluded.waveform_data,
            waveform_version = excluded.waveform_version,
            analysis_version = excluded.analysis_version,
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
            END;
    )SQL";

    const auto prepared = statements.borrow(*database, sql);
    if (!prepared.isValid()) {
        return false;
    }

    auto& statement = *prepared;
    const bool textBound = sqlite::bindText(statement, analysis_parameter::filePath, record.fullPath)
        && sqlite::bindText(statement, analysis_parameter::fileName, record.fileName)
        && sqlite::bindText(statement, analysis_parameter::formatName, record.formatName)
        && sqlite::bindText(statement, analysis_parameter::errorMessage, record.errorMessage);
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
    const bool blobsBound = sqlite::bindNull(statement, analysis_parameter::waveformData)
        && (record.hasError() ? sqlite::bindNull(statement, analysis_parameter::shortTermLoudness)
                              : sqlite::bindBlob(statement, analysis_parameter::shortTermLoudness, loudnessData));

    return textBound && integersBound && numbersBound && blobsBound && sqlite::stepToCompletion(statement);
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

    return Decibels {static_cast<float>(sqlite3_column_double(statement.get(), 0))};
}

/// Updates only the user-controlled gain fields for a file.
/// Returns true when a cached row was updated.
/// A file without a cached row, such as one still waiting for its analysis, keeps no gain in the cache.
bool AnalysisCache::storeCustomGain(const juce::File& file, const std::optional<Decibels> customGain)
{
    return storeCustomGains({{.file = file, .customGain = customGain}}) > 0;
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
        WHERE file_path = ?;
    )SQL";

    const auto statement = statements.borrow(*database, sql);
    if (!statement.isValid()) {
        return 0;
    }

    const auto updatedAtMs = juce::Time::getCurrentTime().toMilliseconds();
    int updatedRows = 0;

    for (const auto& [file, customGain] : updates) {
        sqlite3_reset(statement.get());
        const bool stored
            = sqlite::bindDouble(*statement, 1, static_cast<double>(customGain.value_or(Decibels {}).value))
            && sqlite::bindInt64(*statement, 2, customGain.has_value() ? 1 : 0)
            && sqlite::bindInt64(*statement, 3, updatedAtMs) && sqlite::bindText(*statement, 4, normalizedPath(file))
            && sqlite::stepToCompletion(*statement);

        if (!stored) {
            return 0;
        }

        updatedRows += sqlite3_changes(database.get());
    }

    return transaction.commit() ? updatedRows : 0;
}

/// Removes the analysis, waveform and all independently cached tag kinds for a file, atomically.
/// Returns true when the operation completed.
bool AnalysisCache::removeAnalysis(const juce::File& file)
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
         {"DELETE FROM file_analysis WHERE file_path = ?;", "DELETE FROM serato_tag_cache WHERE file_path = ?;"})
    {
        const auto statement = statements.borrow(*database, sql);
        if (!statement.isValid() || !sqlite::bindText(*statement, 1, normalizedPath(file))
            || !sqlite::stepToCompletion(*statement))
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
            SELECT file_path, file_size, modified_time_ms, tag_kind FROM serato_tag_cache;
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

    const auto* sql = tagKind.has_value()
        ? "DELETE FROM serato_tag_cache WHERE file_path = ? AND file_size = ? AND modified_time_ms = ? AND tag_kind = "
          "?;"
        : "DELETE FROM file_analysis WHERE file_path = ? AND file_size = ? AND modified_time_ms = ?;";

    const auto statement = statements.borrow(*database, sql);
    if (!statement.isValid()) {
        return false;
    }

    const bool bound = sqlite::bindText(*statement, 1, path) && sqlite::bindInt64(*statement, 2, fileSize)
        && sqlite::bindInt64(*statement, 3, modifiedTimeMs)
        && (!tagKind.has_value() || sqlite::bindInt64(*statement, 4, *tagKind));

    return bound && sqlite::stepToCompletion(*statement) && sqlite3_changes(database.get()) > 0;
}

/// Loads the cached thumbnail waveform data for the given file,
/// or returns an empty optional when there is none for the file's current size, modification time and version.
std::optional<juce::MemoryBlock> AnalysisCache::getWaveformData(const juce::File& file)
{
    // The file system is queried before taking the lock, so a slow network share does not block other cache calls.
    const auto fileSize = file.getSize();
    const auto modifiedTimeMs = file.getLastModificationTime().toMilliseconds();
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return std::nullopt;
    }

    constexpr auto sql = R"SQL(
        SELECT file_size, modified_time_ms, waveform_data, waveform_version, analysis_version
        FROM file_analysis
        WHERE file_path = ?;
    )SQL";

    const auto prepared = statements.borrow(*database, sql);
    if (!prepared.isValid() || !sqlite::bindText(*prepared, 1, normalizedPath(file))) {
        return std::nullopt;
    }

    auto* const statement = prepared.get();

    if (sqlite3_step(statement) != SQLITE_ROW) {
        return std::nullopt;
    }

    const auto cachedFileSize = sqlite3_column_int64(statement, 0);
    const auto cachedModifiedTime = sqlite3_column_int64(statement, 1);
    const auto cachedWaveformVersion = sqlite3_column_int(statement, 3);
    const auto cachedAnalysisVersion = sqlite3_column_int(statement, 4);

    if (cachedFileSize != fileSize || cachedModifiedTime != modifiedTimeMs || cachedWaveformVersion != waveformVersion
        || cachedAnalysisVersion != analysisVersion)
    {
        return std::nullopt;
    }

    auto waveformData = sqlite::columnBlob(*statement, 2);
    return waveformData.isEmpty() ? std::nullopt : std::optional {std::move(waveformData)};
}

/// Stores serialized waveform preview data for a file.
bool AnalysisCache::storeWaveformData(const juce::File& file, const juce::MemoryBlock& waveformData)
{
    // The file system is queried before taking the lock, so a slow network share does not block other cache calls.
    const auto fileSize = file.getSize();
    const auto modifiedTimeMs = file.getLastModificationTime().toMilliseconds();
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        UPDATE file_analysis
        SET waveform_data = ?,
            waveform_version = ?,
            updated_at_ms = ?
        WHERE file_path = ?
          AND file_size = ?
          AND modified_time_ms = ?
          AND analysis_version = ?;
    )SQL";

    const auto statement = statements.borrow(*database, sql);
    if (!statement.isValid()) {
        return false;
    }

    const bool bound = sqlite::bindBlob(*statement, waveform_parameter::waveformData, waveformData)
        && sqlite::bindInt64(*statement, waveform_parameter::waveformVersion, waveformVersion)
        && sqlite::bindInt64(*statement, waveform_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds())
        && sqlite::bindText(*statement, waveform_parameter::filePath, normalizedPath(file))
        && sqlite::bindInt64(*statement, waveform_parameter::fileSize, fileSize)
        && sqlite::bindInt64(*statement, waveform_parameter::modifiedTimeMs, modifiedTimeMs)
        && sqlite::bindInt64(*statement, waveform_parameter::analysisVersion, analysisVersion);

    return bound && sqlite::stepToCompletion(*statement) && sqlite3_changes(database.get()) > 0;
}
}  // namespace audiobatch
