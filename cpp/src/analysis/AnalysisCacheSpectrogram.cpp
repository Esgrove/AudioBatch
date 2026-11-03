/// Independent, bounded spectrogram persistence for AnalysisCache.
/// Optional artifacts are joined to successful measurement geometry without loading measurement histories,
/// and publication checks the captured source before and after the write without changing measurements or gains.

#include "analysis/AnalysisCache.h"
#include "analysis/SpectrogramData.h"
#include "analysis/Sqlite.h"
#include "analysis/SqliteTransaction.h"
#include "utils/FileUtils.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <sqlite3.h>
#include <utility>

namespace audiobatch
{
namespace
{
/// The largest intensity grid plus a bounded allowance for the independently versioned header.
constexpr std::size_t MAXIMUM_SPECTROGRAM_BYTES {2UZ * 1024 * 1024 + 256};

/// Parameter positions shared by artifact identity queries and their lookup or publication tails.
constexpr int PARAM_MODIFIED_TIME {5};
constexpr int PARAM_LOOKUP_MAXIMUM_BYTES {6};
constexpr int PARAM_LOOKUP_ANALYSIS_VERSION {7};
constexpr int PARAM_STORE_SAMPLE_RATE {6};
constexpr int PARAM_STORE_CHANNEL_COUNT {7};
constexpr int PARAM_STORE_DATA {9};
constexpr int PARAM_STORE_ANALYSIS_VERSION {10};
}  // namespace

/// Reads only the current optional artifact and its geometry, never the measurement histories.
/// Stale identities, incompatible measurements and corrupt artifacts are misses, not measurement invalidation.
std::shared_ptr<const SpectrogramData> AnalysisCache::getSpectrogramData(const juce::File& file)
{
    const utils::FileIdentity identity(file);
    if (!identity.matches(file)) {
        return {};
    }
    std::shared_ptr<const SpectrogramData> result;
    withDatabase([&](sqlite3& connection, sqlite::StatementCache& preparedStatements) {
        constexpr auto sql = R"SQL(
            SELECT s.spectrogram_data, s.sample_rate, s.num_channels, s.length_in_samples
            FROM spectrogram_cache AS s JOIN file_analysis AS a ON a.file_path = s.file_path
            WHERE s.file_path = ?1 AND s.algorithm_version = ?2 AND s.serialization_version = ?3
                AND typeof(s.algorithm_version) = 'integer' AND typeof(s.serialization_version) = 'integer'
                AND typeof(s.file_size) = 'integer' AND typeof(s.modified_time_ms) = 'integer'
                AND typeof(s.sample_rate) = 'integer' AND typeof(s.num_channels) = 'integer'
                AND typeof(s.length_in_samples) = 'integer' AND s.producer IN ('cpp', 'rust')
                AND s.file_size = ?4 AND s.modified_time_ms = ?5
                AND typeof(s.spectrogram_data) = 'blob' AND length(s.spectrogram_data) BETWEEN 1 AND ?6
                AND typeof(a.file_size) = 'integer' AND typeof(a.modified_time_ms) = 'integer'
                AND typeof(a.sample_rate) = 'integer' AND typeof(a.channels) = 'integer'
                AND typeof(a.length_in_samples) = 'integer' AND typeof(a.analysis_version) = 'integer'
                AND typeof(a.status) = 'integer' AND a.analysis_version = ?7 AND a.status IN (1, 2)
                AND a.analysis_producer IN ('cpp', 'rust')
                AND a.file_size = s.file_size AND a.modified_time_ms = s.modified_time_ms
                AND a.sample_rate = s.sample_rate AND a.channels = s.num_channels
                AND a.length_in_samples = s.length_in_samples;
        )SQL";
        const auto statement = preparedStatements.borrow(connection, sql);
        if (!statement.isValid() || !sqlite::bindText(*statement, 1, identity.path)
            || !sqlite::bindInt64(*statement, 2, SpectrogramData::algorithmVersion)
            || !sqlite::bindInt64(*statement, 3, SpectrogramData::serializationVersion)
            || !sqlite::bindInt64(*statement, 4, identity.fileSize)
            || !sqlite::bindInt64(*statement, PARAM_MODIFIED_TIME, identity.modifiedTimeMs)
            || !sqlite::bindInt64(
                *statement, PARAM_LOOKUP_MAXIMUM_BYTES, static_cast<std::int64_t>(MAXIMUM_SPECTROGRAM_BYTES)
            )
            || !sqlite::bindInt64(*statement, PARAM_LOOKUP_ANALYSIS_VERSION, analysisVersion)
            || sqlite3_step(statement.get()) != SQLITE_ROW)
        {
            return;
        }
        // SQL excludes oversized payloads before SQLite is asked to materialize the blob.
        const auto bytes = sqlite3_column_bytes(statement.get(), 0);
        if (bytes <= 0 || std::cmp_greater(bytes, MAXIMUM_SPECTROGRAM_BYTES)) {
            return;
        }
        const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(statement.get(), 0));
        if (data == nullptr) {
            return;
        }
        auto decoded = deserializeSpectrogram(std::span(data, static_cast<std::size_t>(bytes)));
        if (decoded && decoded->sampleRate == sqlite3_column_int64(statement.get(), 1)
            && decoded->channelCount == sqlite3_column_int64(statement.get(), 2)
            && decoded->lengthInSamples == sqlite3_column_int64(statement.get(), 3))
        {
            result = std::move(decoded);
        }
    });
    return identity.matches(file) ? result : nullptr;
}

/// Publishes only against matching successful measurements and the still-current captured file.
/// Each algorithm and serialization revision has its own row, so future revisions remain untouched.
/// The final filesystem check runs inside the short transaction so a changed source rolls publication back.
bool AnalysisCache::storeSpectrogramData(const utils::FileIdentity& identity, const SpectrogramData& spectrogramData)
{
    if (!juce::File::isAbsolutePath(identity.path) || !isValidSpectrogramData(spectrogramData)) {
        return false;
    }
    const juce::File file(identity.path);
    if (!identity.matches(file)) {
        return false;
    }
    const auto data = serializeSpectrogram(spectrogramData);
    if (data.isEmpty() || data.getSize() > MAXIMUM_SPECTROGRAM_BYTES) {
        return false;
    }
    bool stored = false;
    withDatabase([&](sqlite3& connection, sqlite::StatementCache& preparedStatements) {
        sqlite::Transaction transaction(connection);
        if (!transaction.isActive()) {
            return;
        }
        constexpr auto sql = R"SQL(
            INSERT INTO spectrogram_cache (
                file_path, algorithm_version, serialization_version, file_size, modified_time_ms,
                sample_rate, num_channels, length_in_samples, producer, spectrogram_data
            ) SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 'cpp', ?9
            WHERE EXISTS (SELECT 1 FROM file_analysis AS a WHERE a.file_path = ?1
                AND typeof(a.file_size) = 'integer' AND typeof(a.modified_time_ms) = 'integer'
                AND typeof(a.sample_rate) = 'integer' AND typeof(a.channels) = 'integer'
                AND typeof(a.length_in_samples) = 'integer' AND typeof(a.analysis_version) = 'integer'
                AND typeof(a.status) = 'integer' AND a.analysis_version = ?10 AND a.status IN (1, 2)
                AND a.analysis_producer IN ('cpp', 'rust')
                AND a.file_size = ?4 AND a.modified_time_ms = ?5
                AND a.sample_rate = ?6 AND a.channels = ?7 AND a.length_in_samples = ?8)
            ON CONFLICT(file_path, algorithm_version, serialization_version) DO UPDATE SET
                file_size = excluded.file_size, modified_time_ms = excluded.modified_time_ms,
                sample_rate = excluded.sample_rate, num_channels = excluded.num_channels,
                length_in_samples = excluded.length_in_samples, producer = excluded.producer,
                spectrogram_data = excluded.spectrogram_data;
        )SQL";
        const auto statement = preparedStatements.borrow(connection, sql);
        if (!statement.isValid() || !sqlite::bindText(*statement, 1, identity.path)
            || !sqlite::bindInt64(*statement, 2, SpectrogramData::algorithmVersion)
            || !sqlite::bindInt64(*statement, 3, SpectrogramData::serializationVersion)
            || !sqlite::bindInt64(*statement, 4, identity.fileSize)
            || !sqlite::bindInt64(*statement, PARAM_MODIFIED_TIME, identity.modifiedTimeMs)
            || !sqlite::bindInt64(*statement, PARAM_STORE_SAMPLE_RATE, spectrogramData.sampleRate)
            || !sqlite::bindInt64(*statement, PARAM_STORE_CHANNEL_COUNT, spectrogramData.channelCount)
            || !sqlite::bindInt64(*statement, 8, spectrogramData.lengthInSamples)
            || !sqlite::bindBlob(*statement, PARAM_STORE_DATA, data)
            || !sqlite::bindInt64(*statement, PARAM_STORE_ANALYSIS_VERSION, analysisVersion)
            || !sqlite::stepToCompletion(*statement) || sqlite3_changes(&connection) == 0 || !identity.matches(file))
        {
            return;
        }
        stored = transaction.commit();
    });
    return stored;
}
}  // namespace audiobatch
