/// Analysis, tag and waveform persistence of AnalysisCache.
/// Implements the load and store paths for analysis records, Serato tag results,
/// waveform previews and custom gain values using prepared statements,
/// and the removal and pruning of rows for files that changed or no longer exist.
/// The database lifecycle and schema live in AnalysisCacheSchema.cpp.

#include "analysis/AnalysisCache.h"

#include "analysis/AnalysisCacheSql.h"
#include "serato/SeratoTags.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <sqlite3.h>
#include <utility>
#include <vector>

namespace audiobatch
{
/// Returns an independently versioned stable tag result, or a miss for stale or corrupt data.
/// Validates file identity after the read. Unknown kinds and transient outcomes are never returned.
std::optional<serato::TagReadResult> AnalysisCache::getSeratoTag(
    const FileIdentity& identity,
    const serato::TagKind kind
)
{
    if (!serato::supportsTagKind(kind) || !juce::File::isAbsolutePath(identity.path)) {
        return std::nullopt;
    }
    const juce::File file(identity.path);
    if (!identity.matches(file)) {
        return std::nullopt;
    }
    serato::TagReadResult result {.kind = kind, .status = serato::ReadStatus::absent, .overview = std::nullopt};
    juce::MemoryBlock data;
    {
        const juce::ScopedLock lock(mutex);
        if (database == nullptr && !openUnlocked()) {
            return std::nullopt;
        }
        constexpr auto sql = R"SQL(
                 SELECT file_size, modified_time_ms, parser_version, result_state, decoded_data, updated_at_ms,
                     file_path, tag_kind
            FROM serato_tag_cache WHERE file_path = ? AND tag_kind = ?;
        )SQL";
        sqlite3_stmt* statement = nullptr;
        if (!prepare(sql, statement)) {
            return std::nullopt;
        }
        bindText(statement, 1, identity.path);
        sqlite3_bind_int64(statement, 2, static_cast<std::uint32_t>(kind));
        bool valid = sqlite3_step(statement) == SQLITE_ROW;
        for (const auto column :
             std::array {
                 serato_column::fileSize,
                 serato_column::modifiedTimeMs,
                 serato_column::parserVersion,
                 serato_column::resultState,
                 serato_column::updatedAtMs,
                 serato_column::tagKind,
             })
        {
            valid = valid && sqlite3_column_type(statement, column) == SQLITE_INTEGER;
        }
        valid = valid && sqlite3_column_int64(statement, serato_column::fileSize) == identity.fileSize
            && sqlite3_column_int64(statement, serato_column::modifiedTimeMs) == identity.modifiedTimeMs
            && sqlite3_column_int64(statement, serato_column::parserVersion) == serato::parserVersion(kind)
            && sqlite3_column_int64(statement, serato_column::updatedAtMs) > 0
            && sqlite3_column_type(statement, serato_column::filePath) == SQLITE_TEXT
            && columnText(statement, serato_column::filePath) == identity.path
            && sqlite3_column_int64(statement, serato_column::tagKind) == static_cast<std::uint32_t>(kind);
        const auto state = sqlite3_column_int64(statement, serato_column::resultState);
        if (valid && state == static_cast<int>(serato::ReadStatus::present)) {
            const auto bytes = sqlite3_column_bytes(statement, serato_column::decodedData);
            valid = sqlite3_column_type(statement, serato_column::decodedData) == SQLITE_BLOB && bytes > 0
                && std::cmp_less_equal(bytes, maximumOverviewBytes);
            if (valid) {
                data = columnBlob(statement, serato_column::decodedData);
            }
        } else {
            valid = valid
                && (state == static_cast<int>(serato::ReadStatus::absent)
                    || state == static_cast<int>(serato::ReadStatus::unsupported)
                    || state == static_cast<int>(serato::ReadStatus::invalid))
                && sqlite3_column_type(statement, serato_column::decodedData) == SQLITE_NULL;
        }
        sqlite3_finalize(statement);
        if (!valid) {
            return std::nullopt;
        }
        result.status = static_cast<serato::ReadStatus>(state);
    }
    if (result.status == serato::ReadStatus::present) {
        const auto decoded = serato::deserializeTag(kind, data);
        if (!decoded) {
            return std::nullopt;
        }
        result = *decoded;
    }
    return identity.matches(file) ? std::optional {std::move(result)} : std::nullopt;
}

/// Stores independently serialized tag data or a stable negative without requiring an analysis row.
/// Serializes outside the database lock and checks the captured identity before writing.
/// Returns false for stale identities, inconsistent results, transient outcomes or database failures.
bool AnalysisCache::storeSeratoTag(const FileIdentity& identity, const serato::TagReadResult& result)
{
    if (!serato::supportsTagKind(result.kind) || result.status < serato::ReadStatus::present
        || result.status > serato::ReadStatus::cancelled || !juce::File::isAbsolutePath(identity.path))
    {
        return false;
    }
    juce::MemoryBlock data;
    switch (result.status) {
        case serato::ReadStatus::present:
            data = serato::serializeTag(result);
            if (data.isEmpty() || data.getSize() > maximumOverviewBytes) {
                return false;
            }
            break;
        case serato::ReadStatus::absent:
        case serato::ReadStatus::unsupported:
        case serato::ReadStatus::invalid:
            if (result.overview || result.analysis || result.autotags || result.beatGrid || result.markers
                || result.trackInfo)
            {
                return false;
            }
            break;
        case serato::ReadStatus::ioError:
        case serato::ReadStatus::cancelled:
            return false;
    }
    const juce::File file(identity.path);
    if (!identity.matches(file)) {
        return false;
    }
    const juce::ScopedLock lock(mutex);
    if (database == nullptr && !openUnlocked()) {
        return false;
    }
    constexpr auto sql = R"SQL(
        INSERT INTO serato_tag_cache (
            file_path, tag_kind, file_size, modified_time_ms, parser_version, result_state, decoded_data, updated_at_ms
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(file_path, tag_kind) DO UPDATE SET
            file_size = excluded.file_size, modified_time_ms = excluded.modified_time_ms,
            parser_version = excluded.parser_version, result_state = excluded.result_state,
            decoded_data = excluded.decoded_data, updated_at_ms = excluded.updated_at_ms;
    )SQL";
    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
        return false;
    }
    bindText(statement, serato_parameter::filePath, identity.path);
    sqlite3_bind_int64(statement, serato_parameter::tagKind, static_cast<std::uint32_t>(result.kind));
    sqlite3_bind_int64(statement, serato_parameter::fileSize, identity.fileSize);
    sqlite3_bind_int64(statement, serato_parameter::modifiedTimeMs, identity.modifiedTimeMs);
    sqlite3_bind_int64(statement, serato_parameter::parserVersion, serato::parserVersion(result.kind));
    sqlite3_bind_int(statement, serato_parameter::resultState, static_cast<int>(result.status));
    if (result.status == serato::ReadStatus::present) {
        bindBlob(statement, serato_parameter::decodedData, data);
    } else {
        sqlite3_bind_null(statement, serato_parameter::decodedData);
    }
    sqlite3_bind_int64(statement, serato_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds());
    const auto completed = stepToCompletion(statement);
    sqlite3_finalize(statement);
    return completed;
}

/// Loads a cached analysis record for the given file when present and current.
bool AnalysisCache::getAnalysis(const juce::File& file, AudioAnalysisRecord& record)
{
    // The file system is queried before taking the lock, so a slow network share does not block other cache calls.
    auto fileRecord = AudioAnalysisRecord::fromFile(file);
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        SELECT file_name, format_name, file_size, modified_time_ms, length_in_samples,
               duration_seconds, peak_left, peak_right, overall_peak, true_peak_left,
               true_peak_right, overall_true_peak, max_short_term_lufs, integrated_lufs,
               sample_rate, channels, bits_per_sample, status, error_message, analysis_version,
               custom_gain_db, has_custom_gain
        FROM file_analysis
        WHERE file_path = ?;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
        return false;
    }

    bindText(statement, 1, normalizedPath(file));

    if (const auto stepResult = sqlite3_step(statement); stepResult != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return false;
    }

    const auto cachedFileSize = sqlite3_column_int64(statement, analysis_column::fileSize);
    const auto cachedModifiedTime = sqlite3_column_int64(statement, analysis_column::modifiedTimeMs);
    const auto cachedVersion = sqlite3_column_int(statement, analysis_column::analysisVersion);

    if (cachedFileSize != fileRecord.fileSize || cachedModifiedTime != fileRecord.modifiedTimeMs
        || cachedVersion != analysisVersion)
    {
        sqlite3_finalize(statement);
        return false;
    }

    record = std::move(fileRecord);
    record.fileName = columnText(statement, analysis_column::fileName);
    record.formatName = columnText(statement, analysis_column::formatName);
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
    record.status = static_cast<AudioAnalysisStatus>(sqlite3_column_int(statement, analysis_column::status));
    record.errorMessage = columnText(statement, analysis_column::errorMessage);
    record.customGainDb = static_cast<float>(sqlite3_column_double(statement, analysis_column::customGainDb));
    record.hasCustomGain = sqlite3_column_int(statement, analysis_column::hasCustomGain) != 0;
    record.fromCache = true;

    if (record.status != AudioAnalysisStatus::failed) {
        record.status = AudioAnalysisStatus::cached;
    }

    sqlite3_finalize(statement);
    return true;
}

/// Stores the completed analysis record for a file.
/// An existing custom gain is kept while the file is unchanged on disk,
/// and replaced by the record's gain when the file's size or modification time differs,
/// since a custom gain only applies to the version of the file it was set for.
bool AnalysisCache::storeAnalysis(const AudioAnalysisRecord& record)
{
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
            custom_gain_db, has_custom_gain
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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

    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
        return false;
    }

    bindText(statement, analysis_parameter::filePath, record.fullPath);
    bindText(statement, analysis_parameter::fileName, record.fileName);
    bindText(statement, analysis_parameter::formatName, record.formatName);
    sqlite3_bind_int64(statement, analysis_parameter::fileSize, record.fileSize);
    sqlite3_bind_int64(statement, analysis_parameter::modifiedTimeMs, record.modifiedTimeMs);
    sqlite3_bind_int64(statement, analysis_parameter::lengthInSamples, record.lengthInSamples);
    sqlite3_bind_double(statement, analysis_parameter::durationSeconds, record.durationSeconds);
    sqlite3_bind_double(statement, analysis_parameter::peakLeft, static_cast<double>(record.peakLeft));
    sqlite3_bind_double(statement, analysis_parameter::peakRight, static_cast<double>(record.peakRight));
    sqlite3_bind_double(statement, analysis_parameter::overallPeak, static_cast<double>(record.overallPeak));
    sqlite3_bind_double(statement, analysis_parameter::truePeakLeft, record.truePeakLeft);
    sqlite3_bind_double(statement, analysis_parameter::truePeakRight, record.truePeakRight);
    sqlite3_bind_double(statement, analysis_parameter::overallTruePeak, record.overallTruePeak);
    sqlite3_bind_double(statement, analysis_parameter::maxShortTermLufs, record.maxShortTermLufs);
    sqlite3_bind_double(statement, analysis_parameter::integratedLufs, record.integratedLufs);
    sqlite3_bind_int(statement, analysis_parameter::sampleRate, record.sampleRate);
    sqlite3_bind_int(statement, analysis_parameter::channels, record.channels);
    sqlite3_bind_int(statement, analysis_parameter::bitsPerSample, record.bitsPerSample);
    sqlite3_bind_int(statement, analysis_parameter::status, static_cast<int>(record.status));
    bindText(statement, analysis_parameter::errorMessage, record.errorMessage);
    sqlite3_bind_null(statement, analysis_parameter::waveformData);
    sqlite3_bind_int(statement, analysis_parameter::waveformVersion, 0);
    sqlite3_bind_int(statement, analysis_parameter::analysisVersion, analysisVersion);
    sqlite3_bind_int64(statement, analysis_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds());
    sqlite3_bind_double(statement, analysis_parameter::customGainDb, static_cast<double>(record.customGainDb));
    sqlite3_bind_int(statement, analysis_parameter::hasCustomGain, record.hasCustomGain ? 1 : 0);

    const auto completed = stepToCompletion(statement);
    sqlite3_finalize(statement);
    return completed;
}

/// Loads the custom gain stored for a file when the stored row has the given size and modification time,
/// so a fresh analysis of an unchanged file keeps the gain the user set.
/// Returns false when the file has no row, no custom gain, or the row describes another version of the file.
bool AnalysisCache::getCustomGain(
    const juce::File& file,
    const std::int64_t fileSize,
    const std::int64_t modifiedTimeMs,
    float& customGainDb
)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        SELECT custom_gain_db FROM file_analysis
        WHERE file_path = ? AND file_size = ? AND modified_time_ms = ? AND has_custom_gain = 1;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
        return false;
    }

    bindText(statement, 1, normalizedPath(file));
    sqlite3_bind_int64(statement, 2, fileSize);
    sqlite3_bind_int64(statement, 3, modifiedTimeMs);

    const bool found = sqlite3_step(statement) == SQLITE_ROW;
    if (found) {
        customGainDb = static_cast<float>(sqlite3_column_double(statement, 0));
    }

    sqlite3_finalize(statement);
    return found;
}

/// Updates only the user-controlled gain fields for a file.
/// Returns true when a cached row was updated.
/// A file without a cached row, such as one still waiting for its analysis, keeps no gain in the cache.
bool AnalysisCache::storeCustomGain(const juce::File& file, const float customGainDb, const bool hasCustomGain)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        UPDATE file_analysis
        SET custom_gain_db = ?, has_custom_gain = ?, updated_at_ms = ?
        WHERE file_path = ?;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
        return false;
    }

    sqlite3_bind_double(statement, 1, static_cast<double>(customGainDb));
    sqlite3_bind_int(statement, 2, hasCustomGain ? 1 : 0);
    sqlite3_bind_int64(statement, 3, juce::Time::getCurrentTime().toMilliseconds());
    bindText(statement, 4, normalizedPath(file));

    const auto completed = stepToCompletion(statement);
    const auto updatedRows = sqlite3_changes(database);
    sqlite3_finalize(statement);
    return completed && updatedRows > 0;
}

/// Removes the analysis, waveform and all independently cached tag kinds for a file, atomically.
/// Returns true when the operation completed.
bool AnalysisCache::removeAnalysis(const juce::File& file)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    for (const auto* sql :
         {"DELETE FROM file_analysis WHERE file_path = ?;", "DELETE FROM serato_tag_cache WHERE file_path = ?;"})
    {
        sqlite3_stmt* statement = nullptr;
        if (!prepare(sql, statement)) {
            static_cast<void>(execute("ROLLBACK;"));
            return false;
        }
        bindText(statement, 1, normalizedPath(file));
        const auto completed = stepToCompletion(statement);
        sqlite3_finalize(statement);
        if (!completed) {
            static_cast<void>(execute("ROLLBACK;"));
            return false;
        }
    }
    if (execute("COMMIT;")) {
        return true;
    }
    static_cast<void>(execute("ROLLBACK;"));
    return false;
}

namespace
{
/// One cached row, identified by path and the file version it describes.
struct CachedRow {
    juce::String path;
    std::int64_t fileSize {0};
    std::int64_t modifiedTimeMs {0};
    std::optional<std::int64_t> tagKind;
};
}  // namespace

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
        sqlite3_stmt* statement = nullptr;
        if (!prepare(sql, statement)) {
            return 0;
        }

        while (sqlite3_step(statement) == SQLITE_ROW) {
            cachedRows.push_back({
                .path = columnText(statement, 0),
                .fileSize = sqlite3_column_int64(statement, 1),
                .modifiedTimeMs = sqlite3_column_int64(statement, 2),
                .tagKind = sqlite3_column_type(statement, 3) == SQLITE_NULL
                    ? std::nullopt
                    : std::optional {sqlite3_column_int64(statement, 3)},
            });
        }

        sqlite3_finalize(statement);
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
) const
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr) {
        return false;
    }

    const auto* sql = tagKind.has_value()
        ? "DELETE FROM serato_tag_cache WHERE file_path = ? AND file_size = ? AND modified_time_ms = ? AND tag_kind = "
          "?;"
        : "DELETE FROM file_analysis WHERE file_path = ? AND file_size = ? AND modified_time_ms = ?;";

    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
        return false;
    }

    bindText(statement, 1, path);
    sqlite3_bind_int64(statement, 2, fileSize);
    sqlite3_bind_int64(statement, 3, modifiedTimeMs);
    if (tagKind) {
        sqlite3_bind_int64(statement, 4, *tagKind);
    }

    const auto completed = stepToCompletion(statement);
    sqlite3_finalize(statement);
    return completed && sqlite3_changes(database) > 0;
}

/// Loads cached thumbnail waveform data for the given file when available.
bool AnalysisCache::getWaveformData(const juce::File& file, juce::MemoryBlock& waveformData)
{
    // The file system is queried before taking the lock, so a slow network share does not block other cache calls.
    const auto fileSize = file.getSize();
    const auto modifiedTimeMs = file.getLastModificationTime().toMilliseconds();
    const juce::ScopedLock lock(mutex);

    waveformData.reset();

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        SELECT file_size, modified_time_ms, waveform_data, waveform_version, analysis_version
        FROM file_analysis
        WHERE file_path = ?;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
        return false;
    }

    bindText(statement, 1, normalizedPath(file));

    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return false;
    }

    const auto cachedFileSize = sqlite3_column_int64(statement, 0);
    const auto cachedModifiedTime = sqlite3_column_int64(statement, 1);
    const auto cachedWaveformVersion = sqlite3_column_int(statement, 3);
    const auto cachedAnalysisVersion = sqlite3_column_int(statement, 4);

    if (cachedFileSize != fileSize || cachedModifiedTime != modifiedTimeMs || cachedWaveformVersion != waveformVersion
        || cachedAnalysisVersion != analysisVersion)
    {
        sqlite3_finalize(statement);
        return false;
    }

    waveformData = columnBlob(statement, 2);
    sqlite3_finalize(statement);
    return waveformData.getSize() > 0;
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

    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
        return false;
    }

    bindBlob(statement, waveform_parameter::waveformData, waveformData);
    sqlite3_bind_int(statement, waveform_parameter::waveformVersion, waveformVersion);
    sqlite3_bind_int64(statement, waveform_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds());
    bindText(statement, waveform_parameter::filePath, normalizedPath(file));
    sqlite3_bind_int64(statement, waveform_parameter::fileSize, fileSize);
    sqlite3_bind_int64(statement, waveform_parameter::modifiedTimeMs, modifiedTimeMs);
    sqlite3_bind_int(statement, waveform_parameter::analysisVersion, analysisVersion);

    const auto completed = stepToCompletion(statement);
    const auto changedRows = sqlite3_changes(database);
    sqlite3_finalize(statement);
    return completed && changedRows > 0;
}
}  // namespace audiobatch
