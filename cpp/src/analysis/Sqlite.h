/// Ownership and value marshalling helpers for the SQLite C API, used by the analysis cache.
/// Declares the database and statement handles that close or finalize themselves,
/// the cache of the prepared statements of a connection with the handle that borrows one of them,
/// and the execute, prepare, step, bind and column helpers that check results and log SQLite errors.
/// Only the implementation includes sqlite3.h, so the cache header stays free of it.

#pragma once

#include <unordered_map>

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

/// A statement borrowed from a StatementCache, reset and cleared of its bindings when the borrow ends,
/// so a reused query never keeps a read transaction open or passes its values on to its next use.
class BorrowedStatement
{
public:
    explicit BorrowedStatement(sqlite3_stmt* borrowed) noexcept;
    ~BorrowedStatement();

    BorrowedStatement(const BorrowedStatement&) = delete;
    BorrowedStatement& operator=(const BorrowedStatement&) = delete;
    BorrowedStatement(BorrowedStatement&&) = delete;
    BorrowedStatement& operator=(BorrowedStatement&&) = delete;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] sqlite3_stmt* get() const noexcept;
    [[nodiscard]] sqlite3_stmt& operator*() const noexcept;

private:
    /// The statement, owned by the cache it came from, or null when it could not be prepared.
    /// It stays valid until the cache is cleared, which must not happen while it is borrowed.
    sqlite3_stmt* statement;
};

/// The prepared statements of one database connection, prepared on first use and reused afterwards,
/// so a query that runs for every file does not parse and plan its SQL again each time.
/// Clear it before its connection closes, and use it on one thread at a time, like the connection.
class StatementCache
{
public:
    [[nodiscard]] BorrowedStatement borrow(sqlite3& database, const char* sql);
    void clear() noexcept;

private:
    /// The statements by the address of their SQL text, which is a string literal at the place that uses it.
    std::unordered_map<const char*, Statement> statements;
};

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
