/// Implementation of AnalysisCache.
/// Manages the SQLite database handle, schema creation, and lightweight column migrations,
/// and implements the load and store paths for analysis records,
/// waveform previews, and custom gain values using prepared statements.

#include "AnalysisCache.h"

#include "utils.h"
#include "version.h"

#include <algorithm>
#include <array>
#include <sqlite3.h>

/// Helpers for marshalling SQLite values into JUCE types.
namespace audiobatch::cache
{
/// Divisor that converts the millisecond counter durations into seconds for log output.
constexpr double millisecondsPerSecond = 1000.0;

/// How long SQLite retries a locked database before failing a statement, in milliseconds.
constexpr int busyTimeoutMilliseconds = 2000;

/// Zero-based result column indices for the analysis record SELECT in AnalysisCache::getAnalysis.
/// The order must match the column list of that query exactly.
namespace analysis_column
{
constexpr int fileName = 0;
constexpr int formatName = 1;
constexpr int fileSize = 2;
constexpr int modifiedTimeMs = 3;
constexpr int lengthInSamples = 4;
constexpr int durationSeconds = 5;
constexpr int peakLeft = 6;
constexpr int peakRight = 7;
constexpr int overallPeak = 8;
constexpr int truePeakLeft = 9;
constexpr int truePeakRight = 10;
constexpr int overallTruePeak = 11;
constexpr int maxShortTermLufs = 12;
constexpr int integratedLufs = 13;
constexpr int sampleRate = 14;
constexpr int channels = 15;
constexpr int bitsPerSample = 16;
constexpr int status = 17;
constexpr int errorMessage = 18;
constexpr int analysisVersion = 19;
constexpr int customGainDb = 20;
constexpr int hasCustomGain = 21;
}  // namespace analysis_column

/// One-based bind parameter indices for the analysis record upsert in AnalysisCache::storeAnalysis.
/// The order must match the column list of that INSERT statement exactly.
namespace analysis_parameter
{
constexpr int filePath = 1;
constexpr int fileName = 2;
constexpr int formatName = 3;
constexpr int fileSize = 4;
constexpr int modifiedTimeMs = 5;
constexpr int lengthInSamples = 6;
constexpr int durationSeconds = 7;
constexpr int peakLeft = 8;
constexpr int peakRight = 9;
constexpr int overallPeak = 10;
constexpr int truePeakLeft = 11;
constexpr int truePeakRight = 12;
constexpr int overallTruePeak = 13;
constexpr int maxShortTermLufs = 14;
constexpr int integratedLufs = 15;
constexpr int sampleRate = 16;
constexpr int channels = 17;
constexpr int bitsPerSample = 18;
constexpr int status = 19;
constexpr int errorMessage = 20;
constexpr int waveformData = 21;
constexpr int waveformVersion = 22;
constexpr int analysisVersion = 23;
constexpr int updatedAtMs = 24;
constexpr int customGainDb = 25;
constexpr int hasCustomGain = 26;
}  // namespace analysis_parameter

/// One-based bind parameter indices for the waveform UPDATE in AnalysisCache::storeWaveformData.
/// The order must match the placeholders of that statement exactly.
namespace waveform_parameter
{
constexpr int waveformData = 1;
constexpr int waveformVersion = 2;
constexpr int updatedAtMs = 3;
constexpr int filePath = 4;
constexpr int fileSize = 5;
constexpr int modifiedTimeMs = 6;
constexpr int analysisVersion = 7;
}  // namespace waveform_parameter

/// Reads a text column as a juce::String, mapping SQL NULL to an empty string.
static juce::String columnText(sqlite3_stmt* statement, const int columnIndex)
{
    const auto* text = sqlite3_column_blob(statement, columnIndex);
    return text != nullptr ? juce::String::createStringFromData(text, sqlite3_column_bytes(statement, columnIndex))
                           : juce::String();
}

/// Copies a blob column into a juce::MemoryBlock, returning an empty block for NULL or empty data.
static juce::MemoryBlock columnBlob(sqlite3_stmt* statement, const int columnIndex)
{
    juce::MemoryBlock data;

    if (const auto* blob = sqlite3_column_blob(statement, columnIndex)) {
        const auto size = sqlite3_column_bytes(statement, columnIndex);

        if (size > 0) {
            data.append(blob, static_cast<size_t>(size));
        }
    }

    return data;
}

/// Binds a juce::String as UTF-8 text.
/// SQLITE_TRANSIENT makes SQLite copy the buffer, so the string does not need to outlive the statement.
static void bindText(sqlite3_stmt* statement, const int index, const juce::String& value)
{
    const auto* utf8 = value.toRawUTF8();
    sqlite3_bind_text(statement, index, utf8, -1, SQLITE_TRANSIENT);
}

/// Binds a memory block as a blob.
/// SQLITE_TRANSIENT makes SQLite copy the data, so the block does not need to outlive the statement.
static void bindBlob(sqlite3_stmt* statement, const int index, const juce::MemoryBlock& data)
{
    sqlite3_bind_blob(statement, index, data.getData(), static_cast<int>(data.getSize()), SQLITE_TRANSIENT);
}
}  // namespace audiobatch::cache

using namespace audiobatch::cache;

AnalysisCache::AnalysisCache()
{
    const auto appDataDirectory
        = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile(version::APP_NAME);
    databaseFile = appDataDirectory.getChildFile("analysis.db");
}

AnalysisCache::AnalysisCache(juce::File cacheDatabaseFile) : databaseFile(std::move(cacheDatabaseFile)) { }

AnalysisCache::~AnalysisCache()
{
    const juce::ScopedLock lock(mutex);
    closeUnlocked();
}

juce::File AnalysisCache::getDatabaseFile() const
{
    return databaseFile;
}

juce::String AnalysisCache::normalizedPath(const juce::File& file)
{
    return file.getFullPathName();
}

bool AnalysisCache::execute(const juce::String& sql) const
{
    char* errorMessage = nullptr;

    if (const auto result = sqlite3_exec(database, sql.toRawUTF8(), nullptr, nullptr, &errorMessage);
        result != SQLITE_OK)
    {
        const auto error
            = errorMessage != nullptr ? juce::String::fromUTF8(errorMessage) : juce::String("Unknown SQLite error");
        utils::logError("SQLite error: {}", error);
        sqlite3_free(errorMessage);
        return false;
    }

    return true;
}

bool AnalysisCache::open()
{
    const juce::ScopedLock lock(mutex);

    return openUnlocked();
}

void AnalysisCache::closeUnlocked()
{
    if (database == nullptr) {
        utils::logDebug("Analysis cache close skipped: database was not open");
        return;
    }

    const auto startedAtMs = juce::Time::getMillisecondCounterHiRes();
    const auto databasePath = databaseFile.getFullPathName();

    sqlite3_close(database);
    database = nullptr;

    utils::logDebug(
        "Closed analysis cache at {} in {:.3f} s",
        databasePath.quoted(),
        (juce::Time::getMillisecondCounterHiRes() - startedAtMs) / millisecondsPerSecond
    );
}

bool AnalysisCache::columnExists(const juce::String& tableName, const juce::String& columnName) const
{
    const auto sql = utils::format("PRAGMA table_info({});", tableName);

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.toRawUTF8(), -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bool found = false;

    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (columnText(statement, 1) == columnName) {
            found = true;
            break;
        }
    }

    sqlite3_finalize(statement);
    return found;
}

bool AnalysisCache::addMissingColumns()
{
    struct ColumnMigration {
        const char* name;
        const char* definition;
    };

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
    };

    return std::ranges::all_of(migrations, [this](const ColumnMigration& migration) {
        return columnExists("file_analysis", migration.name)
            || execute(
                   utils::format("ALTER TABLE file_analysis ADD COLUMN {} {};", migration.name, migration.definition)
            );
    });
}

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
    const auto result = sqlite3_open_v2(
        databaseFile.getFullPathName().toRawUTF8(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );

    if (result != SQLITE_OK || database == nullptr) {
        utils::logError(
            "Failed to open analysis cache at {} in {:.3f} s",
            databasePath.quoted(),
            (juce::Time::getMillisecondCounterHiRes() - startedAtMs) / millisecondsPerSecond
        );
        if (database != nullptr) {
            sqlite3_close(database);
            database = nullptr;
        }
        return false;
    }

    utils::logDebug("Analysis cache startup: sqlite3_open_v2 returned successfully");
    sqlite3_busy_timeout(database, busyTimeoutMilliseconds);

    utils::logDebug("Analysis cache startup: applying SQLite pragmas");
    if (!execute("PRAGMA journal_mode=WAL;") || !execute("PRAGMA synchronous=NORMAL;")) {
        return false;
    }

    utils::logDebug("Analysis cache startup: ensuring schema");
    if (!execute(R"SQL(
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
        return false;
    }

    if (!addMissingColumns()) {
        return false;
    }

    utils::logDebug("Analysis cache startup: schema ready");
    utils::logDebug(
        "Opened analysis cache at {} ({}) in {:.3f} s",
        databasePath.quoted(),
        databaseFound ? "existing" : "new",
        (juce::Time::getMillisecondCounterHiRes() - startedAtMs) / millisecondsPerSecond
    );

    return true;
}

bool AnalysisCache::getAnalysis(const juce::File& file, AudioAnalysisRecord& record)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        SELECT file_name, format_name, file_size, modified_time_ms, length_in_samples,
               duration_seconds, peak_left, peak_right, overall_peak, true_peak_left,
               true_peak_right, overall_true_peak, max_short_term_lufs, integrated_lufs,
               sample_rate, channels, bits_per_sample, status, error_message, analysis_version,
               custom_gain_db, has_custom_gain
        FROM file_analysis
        WHERE file_path = ?;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bindText(statement, 1, normalizedPath(file));

    if (const auto stepResult = sqlite3_step(statement); stepResult != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return false;
    }

    const auto cachedFileSize = sqlite3_column_int64(statement, analysis_column::fileSize);
    const auto cachedModifiedTime = sqlite3_column_int64(statement, analysis_column::modifiedTimeMs);
    const auto cachedVersion = sqlite3_column_int(statement, analysis_column::analysisVersion);

    if (cachedFileSize != file.getSize() || cachedModifiedTime != file.getLastModificationTime().toMilliseconds()
        || cachedVersion != analysisVersion)
    {
        sqlite3_finalize(statement);
        return false;
    }

    record = AudioAnalysisRecord::fromFile(file);
    record.fileName = columnText(statement, analysis_column::fileName);
    record.formatName = columnText(statement, analysis_column::formatName);
    record.lengthInSamples = sqlite3_column_int64(statement, analysis_column::lengthInSamples);
    record.durationSeconds = sqlite3_column_double(statement, analysis_column::durationSeconds);
    record.peakLeft = static_cast<float>(sqlite3_column_double(statement, analysis_column::peakLeft));
    record.peakRight = static_cast<float>(sqlite3_column_double(statement, analysis_column::peakRight));
    record.overallPeak = static_cast<float>(sqlite3_column_double(statement, analysis_column::overallPeak));
    record.truePeakLeft = sqlite3_column_double(statement, analysis_column::truePeakLeft);
    record.truePeakRight = sqlite3_column_double(statement, analysis_column::truePeakRight);
    record.overallTruePeak = sqlite3_column_double(statement, analysis_column::overallTruePeak);
    record.maxShortTermLufs = sqlite3_column_double(statement, analysis_column::maxShortTermLufs);
    record.integratedLufs = sqlite3_column_double(statement, analysis_column::integratedLufs);
    record.sampleRate = sqlite3_column_int(statement, analysis_column::sampleRate);
    record.channels = sqlite3_column_int(statement, analysis_column::channels);
    record.bitsPerSample = sqlite3_column_int(statement, analysis_column::bitsPerSample);
    record.status = static_cast<AudioAnalysisStatus>(sqlite3_column_int(statement, analysis_column::status));
    record.errorMessage = columnText(statement, analysis_column::errorMessage);
    record.customGainDb = static_cast<float>(sqlite3_column_double(statement, analysis_column::customGainDb));
    record.hasCustomGain = sqlite3_column_int(statement, analysis_column::hasCustomGain) != 0;
    record.fromCache = true;

    if (record.status != AudioAnalysisStatus::failed) {
        record.status = AudioAnalysisStatus::cached;
    }

    sqlite3_finalize(statement);
    return true;
}

bool AnalysisCache::storeAnalysis(const AudioAnalysisRecord& record)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        INSERT INTO file_analysis (
            file_path, file_name, format_name, file_size, modified_time_ms, length_in_samples,
            duration_seconds, peak_left, peak_right, overall_peak, true_peak_left,
            true_peak_right, overall_true_peak, max_short_term_lufs, integrated_lufs,
            sample_rate, channels, bits_per_sample, status, error_message,
            waveform_data, waveform_version, analysis_version, updated_at_ms,
            custom_gain_db, has_custom_gain
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(file_path) DO UPDATE SET
            file_name = excluded.file_name,
            format_name = excluded.format_name,
            file_size = excluded.file_size,
            modified_time_ms = excluded.modified_time_ms,
            length_in_samples = excluded.length_in_samples,
            duration_seconds = excluded.duration_seconds,
            peak_left = excluded.peak_left,
            peak_right = excluded.peak_right,
            overall_peak = excluded.overall_peak,
            true_peak_left = excluded.true_peak_left,
            true_peak_right = excluded.true_peak_right,
            overall_true_peak = excluded.overall_true_peak,
            max_short_term_lufs = excluded.max_short_term_lufs,
            integrated_lufs = excluded.integrated_lufs,
            sample_rate = excluded.sample_rate,
            channels = excluded.channels,
            bits_per_sample = excluded.bits_per_sample,
            status = excluded.status,
            error_message = excluded.error_message,
            waveform_data = excluded.waveform_data,
            waveform_version = excluded.waveform_version,
            analysis_version = excluded.analysis_version,
            updated_at_ms = excluded.updated_at_ms;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bindText(statement, analysis_parameter::filePath, record.fullPath);
    bindText(statement, analysis_parameter::fileName, record.fileName);
    bindText(statement, analysis_parameter::formatName, record.formatName);
    sqlite3_bind_int64(statement, analysis_parameter::fileSize, record.fileSize);
    sqlite3_bind_int64(statement, analysis_parameter::modifiedTimeMs, record.modifiedTimeMs);
    sqlite3_bind_int64(statement, analysis_parameter::lengthInSamples, record.lengthInSamples);
    sqlite3_bind_double(statement, analysis_parameter::durationSeconds, record.durationSeconds);
    sqlite3_bind_double(statement, analysis_parameter::peakLeft, static_cast<double>(record.peakLeft));
    sqlite3_bind_double(statement, analysis_parameter::peakRight, static_cast<double>(record.peakRight));
    sqlite3_bind_double(statement, analysis_parameter::overallPeak, static_cast<double>(record.overallPeak));
    sqlite3_bind_double(statement, analysis_parameter::truePeakLeft, record.truePeakLeft);
    sqlite3_bind_double(statement, analysis_parameter::truePeakRight, record.truePeakRight);
    sqlite3_bind_double(statement, analysis_parameter::overallTruePeak, record.overallTruePeak);
    sqlite3_bind_double(statement, analysis_parameter::maxShortTermLufs, record.maxShortTermLufs);
    sqlite3_bind_double(statement, analysis_parameter::integratedLufs, record.integratedLufs);
    sqlite3_bind_int(statement, analysis_parameter::sampleRate, record.sampleRate);
    sqlite3_bind_int(statement, analysis_parameter::channels, record.channels);
    sqlite3_bind_int(statement, analysis_parameter::bitsPerSample, record.bitsPerSample);
    sqlite3_bind_int(statement, analysis_parameter::status, static_cast<int>(record.status));
    bindText(statement, analysis_parameter::errorMessage, record.errorMessage);
    sqlite3_bind_null(statement, analysis_parameter::waveformData);
    sqlite3_bind_int(statement, analysis_parameter::waveformVersion, 0);
    sqlite3_bind_int(statement, analysis_parameter::analysisVersion, analysisVersion);
    sqlite3_bind_int64(statement, analysis_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds());
    sqlite3_bind_double(statement, analysis_parameter::customGainDb, static_cast<double>(record.customGainDb));
    sqlite3_bind_int(statement, analysis_parameter::hasCustomGain, record.hasCustomGain ? 1 : 0);

    const auto result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return result == SQLITE_DONE;
}

bool AnalysisCache::getCustomGain(const juce::File& file, float& customGainDb)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = "SELECT custom_gain_db FROM file_analysis WHERE file_path = ? AND has_custom_gain = 1;";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bindText(statement, 1, normalizedPath(file));

    const bool found = sqlite3_step(statement) == SQLITE_ROW;
    if (found) {
        customGainDb = static_cast<float>(sqlite3_column_double(statement, 0));
    }

    sqlite3_finalize(statement);
    return found;
}

bool AnalysisCache::storeCustomGain(const juce::File& file, const float customGainDb, const bool hasCustomGain)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        UPDATE file_analysis
        SET custom_gain_db = ?, has_custom_gain = ?, updated_at_ms = ?
        WHERE file_path = ?;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_double(statement, 1, static_cast<double>(customGainDb));
    sqlite3_bind_int(statement, 2, hasCustomGain ? 1 : 0);
    sqlite3_bind_int64(statement, 3, juce::Time::getCurrentTime().toMilliseconds());
    bindText(statement, 4, normalizedPath(file));

    const auto result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return result == SQLITE_DONE;
}

bool AnalysisCache::removeAnalysis(const juce::File& file)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = "DELETE FROM file_analysis WHERE file_path = ?;";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bindText(statement, 1, normalizedPath(file));

    const auto result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return result == SQLITE_DONE;
}

int AnalysisCache::pruneMissingFiles(const std::function<bool()>& shouldStop)
{
    juce::StringArray cachedPaths;

    {
        const juce::ScopedLock lock(mutex);

        if (database == nullptr && !openUnlocked()) {
            return 0;
        }

        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT file_path FROM file_analysis;", -1, &statement, nullptr) != SQLITE_OK)
        {
            return 0;
        }

        while (sqlite3_step(statement) == SQLITE_ROW) {
            cachedPaths.add(columnText(statement, 0));
        }

        sqlite3_finalize(statement);
    }

    int removedRows = 0;

    for (const auto& path : cachedPaths) {
        if (shouldStop != nullptr && shouldStop()) {
            break;
        }

        if (!juce::File::isAbsolutePath(path) || juce::File(path).existsAsFile()) {
            continue;
        }

        if (removeAnalysis(juce::File(path))) {
            ++removedRows;
        }
    }

    if (removedRows > 0) {
        utils::logInfo("Removed {} cached analysis rows for files that no longer exist", removedRows);
    }

    return removedRows;
}

bool AnalysisCache::getWaveformData(const juce::File& file, juce::MemoryBlock& waveformData)
{
    const juce::ScopedLock lock(mutex);

    waveformData.reset();

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        SELECT file_size, modified_time_ms, waveform_data, waveform_version, analysis_version
        FROM file_analysis
        WHERE file_path = ?;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bindText(statement, 1, normalizedPath(file));

    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return false;
    }

    const auto cachedFileSize = sqlite3_column_int64(statement, 0);
    const auto cachedModifiedTime = sqlite3_column_int64(statement, 1);
    const auto cachedWaveformVersion = sqlite3_column_int(statement, 3);
    const auto cachedAnalysisVersion = sqlite3_column_int(statement, 4);

    if (cachedFileSize != file.getSize() || cachedModifiedTime != file.getLastModificationTime().toMilliseconds()
        || cachedWaveformVersion != waveformVersion || cachedAnalysisVersion != analysisVersion)
    {
        sqlite3_finalize(statement);
        return false;
    }

    waveformData = columnBlob(statement, 2);
    sqlite3_finalize(statement);
    return waveformData.getSize() > 0;
}

bool AnalysisCache::storeWaveformData(const juce::File& file, const juce::MemoryBlock& waveformData)
{
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
          AND analysis_version = ?;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bindBlob(statement, waveform_parameter::waveformData, waveformData);
    sqlite3_bind_int(statement, waveform_parameter::waveformVersion, waveformVersion);
    sqlite3_bind_int64(statement, waveform_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds());
    bindText(statement, waveform_parameter::filePath, normalizedPath(file));
    sqlite3_bind_int64(statement, waveform_parameter::fileSize, file.getSize());
    sqlite3_bind_int64(statement, waveform_parameter::modifiedTimeMs, file.getLastModificationTime().toMilliseconds());
    sqlite3_bind_int(statement, waveform_parameter::analysisVersion, analysisVersion);

    const auto result = sqlite3_step(statement);
    const auto changedRows = sqlite3_changes(database);
    sqlite3_finalize(statement);
    return result == SQLITE_DONE && changedRows > 0;
}
