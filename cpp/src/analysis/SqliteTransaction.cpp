/// Implementation of sqlite::Transaction on top of the logging execute helper.

#include "analysis/SqliteTransaction.h"

#include "analysis/Sqlite.h"

namespace audiobatch::sqlite
{
/// Begins an immediate transaction, which takes the write lock up front instead of on the first write.
/// Check isActive afterwards, since beginning can fail, for example while another process holds the lock.
Transaction::Transaction(sqlite3& connection) : database(connection), active(execute(database, "BEGIN IMMEDIATE;")) {}

/// Rolls the transaction back unless it was committed.
Transaction::~Transaction()
{
    if (active) {
        static_cast<void>(execute(database, "ROLLBACK;"));
    }
}

/// Commits the transaction. Returns true on success.
/// A failed commit leaves the transaction active, so the destructor still rolls it back.
bool Transaction::commit()
{
    if (!active || !execute(database, "COMMIT;")) {
        return false;
    }

    active = false;
    return true;
}

/// True from a successful begin until the commit or the rollback.
bool Transaction::isActive() const noexcept
{
    return active;
}
}  // namespace audiobatch::sqlite
