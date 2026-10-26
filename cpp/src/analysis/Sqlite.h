/// Ownership and value marshalling helpers for the SQLite C API, used by the analysis cache.
/// Declares the database and statement handles that close or finalize themselves,
/// and the execute, prepare, step, bind and column helpers that check results and log SQLite errors.
/// Only the implementation includes sqlite3.h, so the cache header stays free of it.

#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <memory>

struct sqlite3;
struct sqlite3_stmt;

namespace audiobatch::sqlite
{
/// Closes a database connection when its owning handle is destroyed.
struct DatabaseCloser {
    void operator()(sqlite3* database) const noexcept;
};

/// Finalizes a prepared statement when its owning handle is destroyed.
struct StatementFinalizer {
    void operator()(sqlite3_stmt* statement) const noexcept;
};

/// An open database connection that closes itself.
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

/// A prepared statement that finalizes itself.
using Statement = std::unique_ptr<sqlite3_stmt, StatementFinalizer>;

[[nodiscard]] bool execute(sqlite3& database, const char* sql);
[[nodiscard]] Statement prepare(sqlite3& database, const char* sql);
[[nodiscard]] bool stepToCompletion(sqlite3_stmt& statement);

[[nodiscard]] bool bindBlob(sqlite3_stmt& statement, int index, const juce::MemoryBlock& data);
[[nodiscard]] bool bindDouble(sqlite3_stmt& statement, int index, double value);
[[nodiscard]] bool bindInt64(sqlite3_stmt& statement, int index, std::int64_t value);
[[nodiscard]] bool bindNull(sqlite3_stmt& statement, int index);
[[nodiscard]] bool bindText(sqlite3_stmt& statement, int index, const juce::String& value);

juce::MemoryBlock columnBlob(sqlite3_stmt& statement, int columnIndex);
juce::String columnText(sqlite3_stmt& statement, int columnIndex);
}  // namespace audiobatch::sqlite
