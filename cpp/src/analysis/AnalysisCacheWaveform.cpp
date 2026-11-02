/// Native waveform persistence for AnalysisCache.
/// Uses shared bounded JUCE thumbnail geometry before allocating preview storage,
/// and publishes previews only against the source version and measurements they describe.

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCacheSql.h"
#include "analysis/NativeWaveformData.h"
#include "analysis/Sqlite.h"
#include "utils/FileUtils.h"

#include <cstddef>
#include <optional>
#include <span>
#include <sqlite3.h>

namespace audiobatch
{
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
        SELECT file_size, modified_time_ms, waveform_data, waveform_version, analysis_version,
               length_in_samples, sample_rate, channels
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

    if (sqlite3_column_type(statement, 2) != SQLITE_BLOB || sqlite3_column_bytes(statement, 2) > maximumWaveformBytes) {
        return std::nullopt;
    }
    const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(statement, 2));
    if (data == nullptr) {
        return std::nullopt;
    }
    const auto geometry
        = waveformGeometry(std::span(data, static_cast<std::size_t>(sqlite3_column_bytes(statement, 2))));
    constexpr int lengthColumn {5};
    constexpr int rateColumn {6};
    constexpr int channelsColumn {7};
    if (!geometry || geometry->lengthInSamples != sqlite3_column_int64(statement, lengthColumn)
        || geometry->sampleRate != sqlite3_column_int64(statement, rateColumn)
        || geometry->channels != sqlite3_column_int64(statement, channelsColumn))
    {
        return std::nullopt;
    }
    return sqlite::columnBlob(*statement, 2);
}

/// Stores serialized waveform preview data for a file.
bool AnalysisCache::storeWaveformData(const utils::FileIdentity& identity, const juce::MemoryBlock& waveformData)
{
    const auto geometry
        = waveformGeometry(std::span(static_cast<const std::byte*>(waveformData.getData()), waveformData.getSize()));
    if (!geometry || !identity.matches(juce::File(identity.path))) {
        return false;
    }
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
          AND analysis_version = ?
          AND length_in_samples = ? AND sample_rate = ? AND channels = ? AND status IN (1, 2);
    )SQL";

    const auto statement = statements.borrow(*database, sql);
    if (!statement.isValid()) {
        return false;
    }

    const bool bound = sqlite::bindBlob(*statement, waveform_parameter::waveformData, waveformData)
        && sqlite::bindInt64(*statement, waveform_parameter::waveformVersion, waveformVersion)
        && sqlite::bindInt64(*statement, waveform_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds())
        && sqlite::bindText(*statement, waveform_parameter::filePath, identity.path)
        && sqlite::bindInt64(*statement, waveform_parameter::fileSize, identity.fileSize)
        && sqlite::bindInt64(*statement, waveform_parameter::modifiedTimeMs, identity.modifiedTimeMs)
        && sqlite::bindInt64(*statement, waveform_parameter::analysisVersion, analysisVersion)
        && sqlite::bindInt64(*statement, waveform_parameter::lengthInSamples, geometry->lengthInSamples)
        && sqlite::bindInt64(*statement, waveform_parameter::sampleRate, geometry->sampleRate)
        && sqlite::bindInt64(*statement, waveform_parameter::channels, geometry->channels);

    return bound && sqlite::stepToCompletion(*statement) && sqlite3_changes(database.get()) > 0;
}
}  // namespace audiobatch
