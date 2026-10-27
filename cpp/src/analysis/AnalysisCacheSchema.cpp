/// Database lifecycle and schema of AnalysisCache.
/// Implements construction and destruction, opening the SQLite database with its pragmas,
/// schema creation, and lightweight column migrations.

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCacheSql.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/version.h"

#include <algorithm>
#include <array>
#include <sqlite3.h>
#include <utility>

namespace audiobatch
{
namespace
{
/// A column that older databases lack, with the definition used to add it.
struct ColumnMigration {
    /// Name of the column.
    const char* name {nullptr};
    /// SQL type and constraints of the column, as ALTER TABLE ADD COLUMN takes them.
    const char* definition {nullptr};
};
}  // namespace

/// Resolves the default cache database location.
AnalysisCache::AnalysisCache()
{
    const auto appDataDirectory
        = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile(version::APP_NAME);
    databaseFile = appDataDirectory.getChildFile("analysis.db");
}

/// Uses the given SQLite database file instead of the default location, for example in tests.
AnalysisCache::AnalysisCache(juce::File cacheDatabaseFile) : databaseFile(std::move(cacheDatabaseFile)) {}

/// Closes the database if it is currently open.
AnalysisCache::~AnalysisCache()
{
    const juce::ScopedLock lock(mutex);
    closeUnlocked();
}

/// Returns the on-disk SQLite file used by the cache.
juce::File AnalysisCache::getDatabaseFile() const
{
    return databaseFile;
}

/// Returns the canonical path string used as the cache key for a file.
juce::String AnalysisCache::normalizedPath(const juce::File& file)
{
    return file.getFullPathName();
}

/// Opens the cache database and creates or migrates tables as needed.
bool AnalysisCache::open()
{
    const juce::ScopedLock lock(mutex);

    return openUnlocked();
}

/// Closes the database handle if it is open. The caller must hold the mutex.
void AnalysisCache::closeUnlocked()
{
    if (database == nullptr) {
        utils::logDebug("Analysis cache close skipped: database was not open");
        return;
    }

    const auto startedAtMs = juce::Time::getMillisecondCounterHiRes();
    const auto databasePath = databaseFile.getFullPathName();

    statements.clear();
    database.reset();

    utils::logDebug(
        "Closed analysis cache at {} in {:.3f} s",
        databasePath.quoted(),
        (juce::Time::getMillisecondCounterHiRes() - startedAtMs) / utils::millisecondsPerSecond
    );
}

/// Returns true when the given table already has the named column.
/// Used for lightweight schema migrations when opening the database.
bool AnalysisCache::columnExists(const juce::String& tableName, const juce::String& columnName) const
{
    const auto sql = utils::format("PRAGMA table_info({});", tableName);

    const auto statement = sqlite::prepare(*database, sql.toRawUTF8());
    if (statement == nullptr) {
        return false;
    }

    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        if (sqlite::columnText(*statement, 1) == columnName) {
            return true;
        }
    }

    return false;
}

/// Adds every column that a database created by an older version lacks, with its default value.
/// Returns false when a migration statement fails.
bool AnalysisCache::addMissingColumns() const
{
    constexpr std::array migrations {
        ColumnMigration {.name = "waveform_data", .definition = "BLOB"},
        ColumnMigration {.name = "waveform_version", .definition = "INTEGER NOT NULL DEFAULT 0"},
        ColumnMigration {.name = "true_peak_left", .definition = "REAL NOT NULL DEFAULT 0"},
        ColumnMigration {.name = "true_peak_right", .definition = "REAL NOT NULL DEFAULT 0"},
        ColumnMigration {.name = "overall_true_peak", .definition = "REAL NOT NULL DEFAULT 0"},
        ColumnMigration {.name = "max_short_term_lufs", .definition = "REAL NOT NULL DEFAULT -1000"},
        ColumnMigration {.name = "integrated_lufs", .definition = "REAL NOT NULL DEFAULT -1000"},
        ColumnMigration {.name = "custom_gain_db", .definition = "REAL NOT NULL DEFAULT 0"},
        ColumnMigration {.name = "has_custom_gain", .definition = "INTEGER NOT NULL DEFAULT 0"},
        ColumnMigration {.name = "short_term_loudness", .definition = "BLOB"},
    };

    return std::ranges::all_of(migrations, [this](const ColumnMigration& migration) {
        return columnExists("file_analysis", migration.name)
            || sqlite::execute(
                   *database,
                   utils::format("ALTER TABLE file_analysis ADD COLUMN {} {};", migration.name, migration.definition)
                       .toRawUTF8()
            );
    });
}

/// Opens the database and ensures the schema is current. The caller must hold the mutex.
/// Closes the connection again when the schema cannot be prepared, so the next call retries the open.
bool AnalysisCache::openUnlocked()
{
    if (database != nullptr) {
        utils::logDebug(
            "Analysis cache open skipped: database already open at {}", databaseFile.getFullPathName().quoted()
        );
        return true;
    }

    const auto startedAtMs = juce::Time::getMillisecondCounterHiRes();
    const auto& databasePath = databaseFile.getFullPathName();
    const auto databaseFound = databaseFile.existsAsFile();

    utils::logDebug("Analysis cache startup: ensuring directory for {}", databasePath.quoted());
    if (const auto createResult = databaseFile.getParentDirectory().createDirectory(); createResult.failed()) {
        utils::logError(
            "Failed to create analysis cache directory {}: {}",
            databaseFile.getParentDirectory().getFullPathName().quoted(),
            createResult.getErrorMessage()
        );
    }

    utils::logDebug("Analysis cache startup: calling sqlite3_open_v2");
    // Every use of the connection holds the cache mutex, so SQLite's own per-connection mutex is not needed.
    sqlite3* openedDatabase = nullptr;
    const auto result = sqlite3_open_v2(
        databaseFile.getFullPathName().toRawUTF8(),
        &openedDatabase,
        static_cast<int>(
            static_cast<unsigned int>(SQLITE_OPEN_READWRITE) | static_cast<unsigned int>(SQLITE_OPEN_CREATE)
            | static_cast<unsigned int>(SQLITE_OPEN_NOMUTEX)
        ),
        nullptr
    );
    // SQLite can hand back a connection even when opening fails, and it still has to be closed.
    database.reset(openedDatabase);

    if (result != SQLITE_OK || database == nullptr) {
        utils::logError(
            "Failed to open analysis cache at {} in {:.3f} s",
            databasePath.quoted(),
            (juce::Time::getMillisecondCounterHiRes() - startedAtMs) / utils::millisecondsPerSecond
        );
        database.reset();
        return false;
    }

    utils::logDebug("Analysis cache startup: sqlite3_open_v2 returned successfully");
    sqlite3_busy_timeout(database.get(), busyTimeoutMilliseconds);

    // Any failure from here on closes the connection again,
    // so a later call retries the whole open instead of using a database with an incomplete schema.
    utils::logDebug("Analysis cache startup: applying SQLite pragmas");
    if (!sqlite::execute(*database, "PRAGMA journal_mode=WAL;")
        || !sqlite::execute(*database, "PRAGMA synchronous=NORMAL;"))
    {
        closeUnlocked();
        return false;
    }

    utils::logDebug("Analysis cache startup: ensuring schema");
    if (!sqlite::execute(*database, R"SQL(
        CREATE TABLE IF NOT EXISTS file_analysis (
            file_path TEXT PRIMARY KEY,
            file_name TEXT NOT NULL,
            format_name TEXT,
            file_size INTEGER NOT NULL,
            modified_time_ms INTEGER NOT NULL,
            length_in_samples INTEGER NOT NULL,
            duration_seconds REAL NOT NULL,
            peak_left REAL NOT NULL,
            peak_right REAL NOT NULL,
            overall_peak REAL NOT NULL,
            true_peak_left REAL NOT NULL DEFAULT 0,
            true_peak_right REAL NOT NULL DEFAULT 0,
            overall_true_peak REAL NOT NULL DEFAULT 0,
            max_short_term_lufs REAL NOT NULL DEFAULT -1000,
            integrated_lufs REAL NOT NULL DEFAULT -1000,
            short_term_loudness BLOB,
            sample_rate INTEGER NOT NULL,
            channels INTEGER NOT NULL,
            bits_per_sample INTEGER NOT NULL,
            custom_gain_db REAL NOT NULL DEFAULT 0,
            has_custom_gain INTEGER NOT NULL DEFAULT 0,
            status INTEGER NOT NULL,
            error_message TEXT,
            waveform_data BLOB,
            waveform_version INTEGER NOT NULL DEFAULT 0,
            analysis_version INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        );
    )SQL"))
    {
        closeUnlocked();
        return false;
    }

    if (!addMissingColumns()) {
        closeUnlocked();
        return false;
    }

    if (!sqlite::execute(*database, R"SQL(
        CREATE TABLE IF NOT EXISTS serato_tag_cache (
            file_path TEXT NOT NULL,
            tag_kind INTEGER NOT NULL,
            file_size INTEGER NOT NULL,
            modified_time_ms INTEGER NOT NULL,
            parser_version INTEGER NOT NULL,
            result_state INTEGER NOT NULL,
            decoded_data BLOB,
            updated_at_ms INTEGER NOT NULL,
            PRIMARY KEY (file_path, tag_kind)
        );
    )SQL"))
    {
        closeUnlocked();
        return false;
    }

    utils::logDebug("Analysis cache startup: schema ready");
    utils::logDebug(
        "Opened analysis cache at {} ({}) in {:.3f} s",
        databasePath.quoted(),
        databaseFound ? "existing" : "new",
        (juce::Time::getMillisecondCounterHiRes() - startedAtMs) / utils::millisecondsPerSecond
    );

    return true;
}
}  // namespace audiobatch
