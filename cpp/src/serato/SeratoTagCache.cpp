/// Implementation of the Serato tag cache.
/// Reads and writes the serato_tag_cache rows through the shared AnalysisCache connection,
/// validating every stored column against the file identity, the parser version and the result state,
/// so stale or corrupt rows are reported as misses instead of as tag data.

#include "serato/SeratoTagCache.h"

#include "analysis/AnalysisCache.h"
#include "analysis/Sqlite.h"
#include "analysis/SqliteTransaction.h"
#include "serato/SeratoTags.h"
#include "serato/SeratoTagTypes.h"
#include "utils/FileUtils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <sqlite3.h>
#include <utility>
#include <vector>

namespace audiobatch::serato
{
namespace
{
/// Largest serialized tag of any kind that the cache stores: a header of four 32-bit fields and the largest payload.
/// The overview serializer writes the same header with a payload two bytes smaller than the largest one.
constexpr std::size_t MAXIMUM_CACHED_TAG_BYTES {4 * sizeof(std::uint32_t) + MAX_TAG_PAYLOAD_BYTES};

/// Zero-based columns for independent tag lookup, including the key's stored SQLite types.
namespace tag_column
{
constexpr int FILE_SIZE {0};
constexpr int MODIFIED_TIME_MS {1};
constexpr int PARSER_VERSION {2};
constexpr int RESULT_STATE {3};
constexpr int DECODED_DATA {4};
constexpr int UPDATED_AT_MS {5};
constexpr int FILE_PATH {6};
constexpr int TAG_KIND {7};
}  // namespace tag_column

/// One-based parameters for independent per-kind tag UPSERT.
namespace tag_parameter
{
constexpr int FILE_PATH {1};
constexpr int TAG_KIND {2};
constexpr int FILE_SIZE {3};
constexpr int MODIFIED_TIME_MS {4};
constexpr int PARSER_VERSION {5};
constexpr int RESULT_STATE {6};
constexpr int DECODED_DATA {7};
constexpr int UPDATED_AT_MS {8};
constexpr int OBSERVED_SOURCE_PRESENT {9};
constexpr int OBSERVED_FILE_SIZE {10};
constexpr int OBSERVED_MODIFIED_TIME_MS {11};
}  // namespace tag_parameter

/// The validated contents of one cached row.
struct CachedTagRow {
    /// Whether the tag was present, or the stable reason it was not.
    ReadStatus status {ReadStatus::absent};
    /// The serialized tag of a present result, empty otherwise.
    juce::MemoryBlock data;
};

/// Reads the row of the tag kind and validates every column against the identity and the parser version.
/// Returns an empty optional for a missing, stale or corrupt row.
std::optional<CachedTagRow> readTagRow(
    sqlite3& database,
    sqlite::StatementCache& statements,
    const utils::FileIdentity& identity,
    const TagKind kind
)
{
    constexpr auto sql = R"SQL(
             SELECT file_size, modified_time_ms, parser_version, result_state, decoded_data, updated_at_ms,
                 file_path, tag_kind
        FROM serato_tag_cache WHERE file_path = ? AND tag_kind = ?;
    )SQL";
    const auto statement = statements.borrow(database, sql);
    if (!statement.isValid() || !sqlite::bindText(*statement, 1, identity.path)
        || !sqlite::bindInt64(*statement, 2, std::to_underlying(kind)))
    {
        return std::nullopt;
    }
    auto* const row = statement.get();
    // Reading a column without a current row is undefined, so a miss returns before any column is read.
    if (sqlite3_step(row) != SQLITE_ROW) {
        return std::nullopt;
    }
    bool valid = true;
    for (const auto column :
         std::array {
             tag_column::FILE_SIZE,
             tag_column::MODIFIED_TIME_MS,
             tag_column::PARSER_VERSION,
             tag_column::RESULT_STATE,
             tag_column::UPDATED_AT_MS,
             tag_column::TAG_KIND,
         })
    {
        valid = valid && sqlite3_column_type(row, column) == SQLITE_INTEGER;
    }
    valid = valid && sqlite3_column_int64(row, tag_column::FILE_SIZE) == identity.fileSize
        && sqlite3_column_int64(row, tag_column::MODIFIED_TIME_MS) == identity.modifiedTimeMs
        && sqlite3_column_int64(row, tag_column::PARSER_VERSION) == parserVersion(kind)
        && sqlite3_column_int64(row, tag_column::UPDATED_AT_MS) > 0
        && sqlite3_column_type(row, tag_column::FILE_PATH) == SQLITE_TEXT
        && sqlite::columnText(*row, tag_column::FILE_PATH) == identity.path
        && std::cmp_equal(sqlite3_column_int64(row, tag_column::TAG_KIND), std::to_underlying(kind));
    const auto state = sqlite3_column_int64(row, tag_column::RESULT_STATE);
    const auto stateIs
        = [state](const ReadStatus candidate) { return std::cmp_equal(state, std::to_underlying(candidate)); };
    CachedTagRow cached;
    if (valid && stateIs(ReadStatus::present)) {
        const auto bytes = sqlite3_column_bytes(row, tag_column::DECODED_DATA);
        valid = sqlite3_column_type(row, tag_column::DECODED_DATA) == SQLITE_BLOB && bytes > 0
            && std::cmp_less_equal(bytes, MAXIMUM_CACHED_TAG_BYTES);
        if (valid) {
            cached.data = sqlite::columnBlob(*row, tag_column::DECODED_DATA);
        }
    } else {
        valid = valid
            && (stateIs(ReadStatus::absent) || stateIs(ReadStatus::unsupported) || stateIs(ReadStatus::invalid))
            && sqlite3_column_type(row, tag_column::DECODED_DATA) == SQLITE_NULL;
    }
    if (!valid) {
        return std::nullopt;
    }
    cached.status = static_cast<ReadStatus>(state);
    return cached;
}

/// Publishes one result only while its source still matches the captured row or the incoming identity.
/// A matching source keeps newer parser results, and a different analysis identity rejects delayed tag work.
bool writeTagRow(
    sqlite3& database,
    sqlite::StatementCache& statements,
    const utils::FileIdentity& identity,
    const TagKind kind,
    const ReadStatus status,
    const juce::MemoryBlock& data,
    const std::optional<std::pair<std::int64_t, std::int64_t>>& observedSource
)
{
    constexpr auto sql = R"SQL(
        INSERT INTO serato_tag_cache (
            file_path, tag_kind, file_size, modified_time_ms, parser_version, result_state, decoded_data, updated_at_ms
        ) SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8
        WHERE NOT EXISTS (SELECT 1 FROM file_analysis WHERE file_path = ?1
            AND (file_size != ?3 OR modified_time_ms != ?4))
        ON CONFLICT(file_path, tag_kind) DO UPDATE SET
            file_size = excluded.file_size, modified_time_ms = excluded.modified_time_ms,
            parser_version = excluded.parser_version, result_state = excluded.result_state,
            decoded_data = excluded.decoded_data, updated_at_ms = excluded.updated_at_ms
        WHERE ((serato_tag_cache.file_size = excluded.file_size
                AND serato_tag_cache.modified_time_ms = excluded.modified_time_ms)
            OR (?9 AND serato_tag_cache.file_size = ?10 AND serato_tag_cache.modified_time_ms = ?11))
            AND ((serato_tag_cache.file_size != excluded.file_size
                OR serato_tag_cache.modified_time_ms != excluded.modified_time_ms)
                OR serato_tag_cache.parser_version <= excluded.parser_version);
    )SQL";
    const auto statement = statements.borrow(database, sql);
    if (!statement.isValid()) {
        return false;
    }
    const bool dataBound = status == ReadStatus::present
        ? sqlite::bindBlob(*statement, tag_parameter::DECODED_DATA, data)
        : sqlite::bindNull(*statement, tag_parameter::DECODED_DATA);
    const bool bound = dataBound && sqlite::bindText(*statement, tag_parameter::FILE_PATH, identity.path)
        && sqlite::bindInt64(*statement, tag_parameter::TAG_KIND, std::to_underlying(kind))
        && sqlite::bindInt64(*statement, tag_parameter::FILE_SIZE, identity.fileSize)
        && sqlite::bindInt64(*statement, tag_parameter::MODIFIED_TIME_MS, identity.modifiedTimeMs)
        && sqlite::bindInt64(*statement, tag_parameter::PARSER_VERSION, parserVersion(kind))
        && sqlite::bindInt64(*statement, tag_parameter::RESULT_STATE, std::to_underlying(status))
        && sqlite::bindInt64(*statement, tag_parameter::UPDATED_AT_MS, juce::Time::getCurrentTime().toMilliseconds());
    const bool sourceBound
        = sqlite::bindInt64(*statement, tag_parameter::OBSERVED_SOURCE_PRESENT, observedSource.has_value() ? 1 : 0)
        && sqlite::bindInt64(*statement, tag_parameter::OBSERVED_FILE_SIZE, observedSource ? observedSource->first : 0)
        && sqlite::bindInt64(
              *statement, tag_parameter::OBSERVED_MODIFIED_TIME_MS, observedSource ? observedSource->second : 0
        );
    return bound && sourceBound && sqlite::stepToCompletion(*statement);
}
}  // namespace

/// Returns an independently versioned stable tag result, or a miss for stale or corrupt data.
/// Validates file identity after the read. Unknown kinds and transient outcomes are never returned.
std::optional<TagReadResult> getCachedTag(AnalysisCache& cache, const utils::FileIdentity& identity, const TagKind kind)
{
    const std::array kinds {kind};
    return getCachedTags(cache, identity, kinds).front();
}

/// Returns the cached results of the tag kinds in their order, with a miss for each stale, corrupt, or unknown kind.
/// The file identity is checked once before and once after reading all of them,
/// and the rows are read under one hold of the cache lock,
/// so the preview checks the file a few times per load instead of a few times per kind.
/// A file that no longer matches its identity misses every kind.
std::vector<std::optional<TagReadResult>> getCachedTags(
    AnalysisCache& cache,
    const utils::FileIdentity& identity,
    const std::span<const TagKind> kinds
)
{
    std::vector<std::optional<TagReadResult>> results(kinds.size());
    if (!juce::File::isAbsolutePath(identity.path) || std::ranges::none_of(kinds, supportsTagKind)) {
        return results;
    }
    const juce::File file(identity.path);
    if (!identity.matches(file)) {
        return results;
    }

    std::vector<std::optional<CachedTagRow>> rows(kinds.size());
    cache.withDatabase([&rows, &identity, kinds](sqlite3& database, sqlite::StatementCache& statements) {
        for (std::size_t index = 0; index < kinds.size(); ++index) {
            if (supportsTagKind(kinds[index])) {
                rows[index] = readTagRow(database, statements, identity, kinds[index]);
            }
        }
    });

    for (std::size_t index = 0; index < kinds.size(); ++index) {
        if (const auto& cached = rows[index]; cached.has_value()) {
            results[index] = cached->status == ReadStatus::present
                ? deserializeTag(kinds[index], cached->data)
                : TagReadResult::missing(kinds[index], cached->status);
        }
    }

    if (!identity.matches(file)) {
        std::ranges::fill(results, std::nullopt);
    }
    return results;
}

/// Stores independently serialized tag data or a stable negative without requiring an analysis row.
/// Captures the stored source before checking the file, with serialization and filesystem work outside database locks.
/// A short write transaction rejects a changed source or newer parser instead of replacing newer work.
/// Returns false for stale identities, inconsistent results, transient outcomes, superseded work or database failures.
bool storeCachedTag(AnalysisCache& cache, const utils::FileIdentity& identity, const TagReadResult& result)
{
    const auto status = result.status();
    if (!supportsTagKind(result.kind) || status < ReadStatus::present || status > ReadStatus::cancelled
        || !juce::File::isAbsolutePath(identity.path))
    {
        return false;
    }
    juce::MemoryBlock data;
    switch (status) {
        case ReadStatus::present:
            data = serializeTag(result);
            if (data.isEmpty() || data.getSize() > MAXIMUM_CACHED_TAG_BYTES) {
                return false;
            }
            break;
        case ReadStatus::absent:
        case ReadStatus::unsupported:
        case ReadStatus::invalid:
            break;
        case ReadStatus::ioError:
        case ReadStatus::cancelled:
            return false;
    }
    std::optional<std::pair<std::int64_t, std::int64_t>> observedSource;
    bool observed = false;
    cache.withDatabase([&](sqlite3& database, sqlite::StatementCache& statements) {
        constexpr auto sql = R"SQL(
            SELECT file_size, modified_time_ms FROM serato_tag_cache WHERE file_path = ? AND tag_kind = ?;
        )SQL";
        const auto statement = statements.borrow(database, sql);
        if (!statement.isValid() || !sqlite::bindText(*statement, 1, identity.path)
            || !sqlite::bindInt64(*statement, 2, std::to_underlying(result.kind)))
        {
            return;
        }
        const auto step = sqlite3_step(statement.get());
        observed = step == SQLITE_ROW || step == SQLITE_DONE;
        if (step == SQLITE_ROW) {
            observedSource
                = std::pair {sqlite3_column_int64(statement.get(), 0), sqlite3_column_int64(statement.get(), 1)};
        }
    });
    if (!observed || !identity.matches(juce::File(identity.path))) {
        return false;
    }
    bool stored = false;
    cache.withDatabase([&](sqlite3& database, sqlite::StatementCache& statements) {
        sqlite::Transaction transaction(database);
        if (!transaction.isActive()
            || !writeTagRow(database, statements, identity, result.kind, status, data, observedSource))
        {
            return;
        }
        const bool changed = sqlite3_changes(&database) > 0;
        stored = transaction.commit() && changed;
    });
    return stored;
}
}  // namespace audiobatch::serato
