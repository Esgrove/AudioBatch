/// Implementation of the Serato tag cache.
/// Reads and writes the serato_tag_cache rows through the shared AnalysisCache connection,
/// validating every stored column against the file identity, the parser version and the result state,
/// so stale or corrupt rows are reported as misses instead of as tag data.

#include "serato/SeratoTagCache.h"

#include "analysis/Sqlite.h"
#include "serato/SeratoTags.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <sqlite3.h>
#include <utility>

namespace audiobatch::serato
{
namespace
{
/// Largest serialized tag of any kind that the cache stores: a header of four 32-bit fields and the largest payload.
/// The overview serializer writes the same header with a payload two bytes smaller than the largest one.
constexpr std::size_t maximumCachedTagBytes {4 * sizeof(std::uint32_t) + maxTagPayloadBytes};

/// Zero-based columns for independent tag lookup, including the key's stored SQLite types.
namespace tag_column
{
constexpr int fileSize {0};
constexpr int modifiedTimeMs {1};
constexpr int parserVersion {2};
constexpr int resultState {3};
constexpr int decodedData {4};
constexpr int updatedAtMs {5};
constexpr int filePath {6};
constexpr int tagKind {7};
}  // namespace tag_column

/// One-based parameters for independent per-kind tag UPSERT.
namespace tag_parameter
{
constexpr int filePath {1};
constexpr int tagKind {2};
constexpr int fileSize {3};
constexpr int modifiedTimeMs {4};
constexpr int parserVersion {5};
constexpr int resultState {6};
constexpr int decodedData {7};
constexpr int updatedAtMs {8};
}  // namespace tag_parameter

/// The validated contents of one cached row.
struct CachedTagRow {
    ReadStatus status {ReadStatus::absent};
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
             tag_column::fileSize,
             tag_column::modifiedTimeMs,
             tag_column::parserVersion,
             tag_column::resultState,
             tag_column::updatedAtMs,
             tag_column::tagKind,
         })
    {
        valid = valid && sqlite3_column_type(row, column) == SQLITE_INTEGER;
    }
    valid = valid && sqlite3_column_int64(row, tag_column::fileSize) == identity.fileSize
        && sqlite3_column_int64(row, tag_column::modifiedTimeMs) == identity.modifiedTimeMs
        && sqlite3_column_int64(row, tag_column::parserVersion) == parserVersion(kind)
        && sqlite3_column_int64(row, tag_column::updatedAtMs) > 0
        && sqlite3_column_type(row, tag_column::filePath) == SQLITE_TEXT
        && sqlite::columnText(*row, tag_column::filePath) == identity.path
        && std::cmp_equal(sqlite3_column_int64(row, tag_column::tagKind), std::to_underlying(kind));
    const auto state = sqlite3_column_int64(row, tag_column::resultState);
    const auto stateIs
        = [state](const ReadStatus candidate) { return std::cmp_equal(state, std::to_underlying(candidate)); };
    CachedTagRow cached;
    if (valid && stateIs(ReadStatus::present)) {
        const auto bytes = sqlite3_column_bytes(row, tag_column::decodedData);
        valid = sqlite3_column_type(row, tag_column::decodedData) == SQLITE_BLOB && bytes > 0
            && std::cmp_less_equal(bytes, maximumCachedTagBytes);
        if (valid) {
            cached.data = sqlite::columnBlob(*row, tag_column::decodedData);
        }
    } else {
        valid = valid
            && (stateIs(ReadStatus::absent) || stateIs(ReadStatus::unsupported) || stateIs(ReadStatus::invalid))
            && sqlite3_column_type(row, tag_column::decodedData) == SQLITE_NULL;
    }
    if (!valid) {
        return std::nullopt;
    }
    cached.status = static_cast<ReadStatus>(state);
    return cached;
}

/// Inserts or replaces the row of the tag kind with the serialized data, or with no data for a stable negative.
bool writeTagRow(
    sqlite3& database,
    sqlite::StatementCache& statements,
    const utils::FileIdentity& identity,
    const TagKind kind,
    const ReadStatus status,
    const juce::MemoryBlock& data
)
{
    constexpr auto sql = R"SQL(
        INSERT INTO serato_tag_cache (
            file_path, tag_kind, file_size, modified_time_ms, parser_version, result_state, decoded_data, updated_at_ms
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(file_path, tag_kind) DO UPDATE SET
            file_size = excluded.file_size, modified_time_ms = excluded.modified_time_ms,
            parser_version = excluded.parser_version, result_state = excluded.result_state,
            decoded_data = excluded.decoded_data, updated_at_ms = excluded.updated_at_ms;
    )SQL";
    const auto statement = statements.borrow(database, sql);
    if (!statement.isValid()) {
        return false;
    }
    const bool dataBound = status == ReadStatus::present
        ? sqlite::bindBlob(*statement, tag_parameter::decodedData, data)
        : sqlite::bindNull(*statement, tag_parameter::decodedData);
    const bool bound = dataBound && sqlite::bindText(*statement, tag_parameter::filePath, identity.path)
        && sqlite::bindInt64(*statement, tag_parameter::tagKind, std::to_underlying(kind))
        && sqlite::bindInt64(*statement, tag_parameter::fileSize, identity.fileSize)
        && sqlite::bindInt64(*statement, tag_parameter::modifiedTimeMs, identity.modifiedTimeMs)
        && sqlite::bindInt64(*statement, tag_parameter::parserVersion, parserVersion(kind))
        && sqlite::bindInt64(*statement, tag_parameter::resultState, std::to_underlying(status))
        && sqlite::bindInt64(*statement, tag_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds());
    return bound && sqlite::stepToCompletion(*statement);
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
/// Serializes outside the database lock and checks the captured identity before writing.
/// Returns false for stale identities, inconsistent results, transient outcomes or database failures.
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
            if (data.isEmpty() || data.getSize() > maximumCachedTagBytes) {
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
    if (!identity.matches(juce::File(identity.path))) {
        return false;
    }
    bool stored = false;
    cache.withDatabase([&stored, &identity, &result, status, &data](
                           sqlite3& database, sqlite::StatementCache& statements
                       ) { stored = writeTagRow(database, statements, identity, result.kind, status, data); });
    return stored;
}
}  // namespace audiobatch::serato
