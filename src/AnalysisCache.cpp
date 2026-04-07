#include "AnalysisCache.h"

#include "utils.h"
#include "version.h"

#include <sqlite3.h>

namespace
{
juce::String columnText(sqlite3_stmt* statement, int columnIndex)
{
    const auto* text = sqlite3_column_text(statement, columnIndex);
    return text != nullptr ? juce::String::fromUTF8(reinterpret_cast<const char*>(text)) : juce::String();
}

void bindText(sqlite3_stmt* statement, int index, const juce::String& value)
{
    const auto utf8 = value.toRawUTF8();
    sqlite3_bind_text(statement, index, utf8, -1, SQLITE_TRANSIENT);
}
}  // namespace

AnalysisCache::AnalysisCache()
{
    const auto appDataDir
        = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile(version::APP_NAME);
    databaseFile = appDataDir.getChildFile("analysis.db");
}

AnalysisCache::~AnalysisCache()
{
    const juce::ScopedLock lock(mutex);

    if (database != nullptr) {
        sqlite3_close(database);
        database = nullptr;
    }
}

juce::File AnalysisCache::getDatabaseFile() const
{
    return databaseFile;
}

juce::String AnalysisCache::normalizedPath(const juce::File& file) const
{
    return file.getFullPathName();
}

bool AnalysisCache::execute(const juce::String& sql)
{
    char* errorMessage = nullptr;
    const auto result = sqlite3_exec(database, sql.toRawUTF8(), nullptr, nullptr, &errorMessage);

    if (result != SQLITE_OK) {
        const auto error
            = errorMessage != nullptr ? juce::String::fromUTF8(errorMessage) : juce::String("Unknown SQLite error");
        utils::log_error("SQLite error: " + error);
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

bool AnalysisCache::openUnlocked()
{
    if (database != nullptr) {
        return true;
    }

    databaseFile.getParentDirectory().createDirectory();

    const auto result = sqlite3_open_v2(
        databaseFile.getFullPathName().toRawUTF8(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );

    if (result != SQLITE_OK || database == nullptr) {
        utils::log_error("Failed to open analysis cache at " + databaseFile.getFullPathName());
        if (database != nullptr) {
            sqlite3_close(database);
            database = nullptr;
        }
        return false;
    }

    sqlite3_busy_timeout(database, 2000);

    if (!execute("PRAGMA journal_mode=WAL;") || !execute("PRAGMA synchronous=NORMAL;")) {
        return false;
    }

    return execute(R"SQL(
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
            sample_rate INTEGER NOT NULL,
            channels INTEGER NOT NULL,
            bits_per_sample INTEGER NOT NULL,
            status INTEGER NOT NULL,
            error_message TEXT,
            analysis_version INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL
        );
    )SQL");
}

bool AnalysisCache::getAnalysis(const juce::File& file, AudioAnalysisRecord& record)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        SELECT file_name, format_name, file_size, modified_time_ms, length_in_samples,
               duration_seconds, peak_left, peak_right, overall_peak, sample_rate,
               channels, bits_per_sample, status, error_message, analysis_version
        FROM file_analysis
        WHERE file_path = ?;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bindText(statement, 1, normalizedPath(file));

    const auto stepResult = sqlite3_step(statement);

    if (stepResult != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return false;
    }

    const auto cachedFileSize = sqlite3_column_int64(statement, 2);
    const auto cachedModifiedTime = sqlite3_column_int64(statement, 3);
    const auto cachedVersion = sqlite3_column_int(statement, 14);

    if (cachedFileSize != file.getSize() || cachedModifiedTime != file.getLastModificationTime().toMilliseconds()
        || cachedVersion != analysisVersion)
    {
        sqlite3_finalize(statement);
        return false;
    }

    record = AudioAnalysisRecord::fromFile(file);
    record.fileName = columnText(statement, 0);
    record.formatName = columnText(statement, 1);
    record.lengthInSamples = sqlite3_column_int64(statement, 4);
    record.durationSeconds = sqlite3_column_double(statement, 5);
    record.peakLeft = static_cast<float>(sqlite3_column_double(statement, 6));
    record.peakRight = static_cast<float>(sqlite3_column_double(statement, 7));
    record.overallPeak = static_cast<float>(sqlite3_column_double(statement, 8));
    record.sampleRate = sqlite3_column_int(statement, 9);
    record.channels = sqlite3_column_int(statement, 10);
    record.bitsPerSample = sqlite3_column_int(statement, 11);
    record.status = static_cast<AudioAnalysisStatus>(sqlite3_column_int(statement, 12));
    record.errorMessage = columnText(statement, 13);
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
            duration_seconds, peak_left, peak_right, overall_peak, sample_rate, channels,
            bits_per_sample, status, error_message, analysis_version, updated_at_ms
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
            sample_rate = excluded.sample_rate,
            channels = excluded.channels,
            bits_per_sample = excluded.bits_per_sample,
            status = excluded.status,
            error_message = excluded.error_message,
            analysis_version = excluded.analysis_version,
            updated_at_ms = excluded.updated_at_ms;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bindText(statement, 1, record.fullPath);
    bindText(statement, 2, record.fileName);
    bindText(statement, 3, record.formatName);
    sqlite3_bind_int64(statement, 4, record.fileSize);
    sqlite3_bind_int64(statement, 5, record.modifiedTimeMs);
    sqlite3_bind_int64(statement, 6, record.lengthInSamples);
    sqlite3_bind_double(statement, 7, record.durationSeconds);
    sqlite3_bind_double(statement, 8, record.peakLeft);
    sqlite3_bind_double(statement, 9, record.peakRight);
    sqlite3_bind_double(statement, 10, record.overallPeak);
    sqlite3_bind_int(statement, 11, record.sampleRate);
    sqlite3_bind_int(statement, 12, record.channels);
    sqlite3_bind_int(statement, 13, record.bitsPerSample);
    sqlite3_bind_int(statement, 14, static_cast<int>(record.status));
    bindText(statement, 15, record.errorMessage);
    sqlite3_bind_int(statement, 16, analysisVersion);
    sqlite3_bind_int64(statement, 17, juce::Time::getCurrentTime().toMilliseconds());

    const auto result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return result == SQLITE_DONE;
}