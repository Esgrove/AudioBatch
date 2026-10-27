/// Scoped SQLite transaction for multi-statement cache writes.
/// Declares sqlite::Transaction, which begins an immediate transaction
/// and rolls it back when it goes out of scope without a commit.

#pragma once

struct sqlite3;

namespace audiobatch::sqlite
{
/// An immediate transaction that rolls back when it is destroyed without a successful commit,
/// so every early return of a multi-statement write leaves the database as it was.
/// The connection must outlive the transaction.
class Transaction final
{
public:
    explicit Transaction(sqlite3& connection);

    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    [[nodiscard]] bool commit();
    [[nodiscard]] bool isActive() const noexcept;

private:
    /// The connection the transaction runs on.
    sqlite3& database;
    /// True from a successful begin until the commit or the rollback.
    bool active {false};
};
}  // namespace audiobatch::sqlite
