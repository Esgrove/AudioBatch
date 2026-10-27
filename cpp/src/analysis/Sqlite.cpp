/// Implementation of the SQLite helpers.
/// Every helper that can fail logs the SQLite error message of the connection, so callers only check the result.
/// Text and blobs are bound with SQLITE_TRANSIENT, which makes SQLite copy them,
/// so the values do not need to outlive the statement.

#include "analysis/Sqlite.h"

#include "utils/Log.h"

#include <sqlite3.h>

namespace audiobatch::sqlite
{
namespace
{
/// Logs the latest error of the statement's connection after a failed call, and returns false.
bool logStatementError(sqlite3_stmt& statement, const char* action, const int index)
{
    utils::logError(
        "SQLite could not {} parameter {}: {}",
        action,
        index,
        juce::String::fromUTF8(sqlite3_errmsg(sqlite3_db_handle(&statement)))
    );
    return false;
}
}  // namespace

/// Closes the connection, deferring the close until any statement that was not finalized is finalized.
void DatabaseCloser::operator()(sqlite3* database) const noexcept
{
    sqlite3_close_v2(database);
}

/// Finalizes the statement, releasing its resources.
void StatementFinalizer::operator()(sqlite3_stmt* statement) const noexcept
{
    sqlite3_finalize(statement);
}

/// Runs SQL that returns no rows, such as a pragma, a schema change, or a transaction statement.
/// Returns true on success.
bool execute(sqlite3& database, const char* sql)
{
    char* errorMessage = nullptr;

    if (sqlite3_exec(&database, sql, nullptr, nullptr, &errorMessage) == SQLITE_OK) {
        return true;
    }

    const auto error
        = errorMessage != nullptr ? juce::String::fromUTF8(errorMessage) : juce::String("Unknown SQLite error");
    utils::logError("SQLite error: {}", error);
    sqlite3_free(errorMessage);
    return false;
}

/// Prepares a statement, or returns an empty handle when the SQL cannot be prepared.
Statement prepare(sqlite3& database, const char* sql)
{
    sqlite3_stmt* statement = nullptr;

    if (sqlite3_prepare_v2(&database, sql, -1, &statement, nullptr) == SQLITE_OK) {
        return Statement(statement);
    }

    // A failed prepare leaves the statement null, so there is nothing to finalize.
    utils::logError(
        "SQLite could not prepare an analysis cache query: {}", juce::String::fromUTF8(sqlite3_errmsg(&database))
    );
    return {};
}

/// Steps a statement that returns no rows to completion.
/// Returns true when the statement completed.
bool stepToCompletion(sqlite3_stmt& statement)
{
    if (sqlite3_step(&statement) == SQLITE_DONE) {
        return true;
    }

    utils::logError(
        "SQLite could not complete an analysis cache update: {}",
        juce::String::fromUTF8(sqlite3_errmsg(sqlite3_db_handle(&statement)))
    );
    return false;
}

/// Binds a memory block as a blob. The 64-bit call keeps sizes above 2 GiB from wrapping around.
bool bindBlob(sqlite3_stmt& statement, const int index, const juce::MemoryBlock& data)
{
    return sqlite3_bind_blob64(&statement, index, data.getData(), data.getSize(), SQLITE_TRANSIENT) == SQLITE_OK
        || logStatementError(statement, "bind blob", index);
}

/// Binds a floating-point value.
bool bindDouble(sqlite3_stmt& statement, const int index, const double value)
{
    return sqlite3_bind_double(&statement, index, value) == SQLITE_OK
        || logStatementError(statement, "bind number", index);
}

/// Binds an integer value. SQLite stores every integer as 64 bits, so this serves all integer columns.
bool bindInt64(sqlite3_stmt& statement, const int index, const std::int64_t value)
{
    return sqlite3_bind_int64(&statement, index, value) == SQLITE_OK
        || logStatementError(statement, "bind integer", index);
}

/// Binds SQL NULL.
bool bindNull(sqlite3_stmt& statement, const int index)
{
    return sqlite3_bind_null(&statement, index) == SQLITE_OK || logStatementError(statement, "bind NULL", index);
}

/// Binds a juce::String as UTF-8 text.
bool bindText(sqlite3_stmt& statement, const int index, const juce::String& value)
{
    return sqlite3_bind_text(&statement, index, value.toRawUTF8(), -1, SQLITE_TRANSIENT) == SQLITE_OK
        || logStatementError(statement, "bind text", index);
}

/// Copies a blob column into a juce::MemoryBlock, returning an empty block for NULL or empty data.
juce::MemoryBlock columnBlob(sqlite3_stmt& statement, const int columnIndex)
{
    juce::MemoryBlock data;

    if (const auto* blob = sqlite3_column_blob(&statement, columnIndex)) {
        if (const auto size = sqlite3_column_bytes(&statement, columnIndex); size > 0) {
            data.append(blob, static_cast<size_t>(size));
        }
    }

    return data;
}

/// Reads a text column as UTF-8, which is how the cache stores text, mapping SQL NULL to an empty string.
/// The text is not inspected for byte order marks, so it comes back exactly as it was stored.
juce::String columnText(sqlite3_stmt& statement, const int columnIndex)
{
    // The blob accessor returns the stored bytes of a text value without converting them.
    const auto* text = static_cast<const char*>(sqlite3_column_blob(&statement, columnIndex));
    return text != nullptr ? juce::String::fromUTF8(text, sqlite3_column_bytes(&statement, columnIndex))
                           : juce::String();
}
}  // namespace audiobatch::sqlite
