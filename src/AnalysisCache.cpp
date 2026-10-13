/// Implementation of AnalysisCache.
/// Manages the SQLite database handle, schema creation, and lightweight column migrations,
/// and implements the load and store paths for analysis records,
/// waveform previews, and custom gain values using prepared statements.

#include "AnalysisCache.h"

#include "Log.h"
#include "SeratoTags.h"
#include "StringFormat.h"
#include "version.h"

#include <algorithm>
#include <array>
#include <sqlite3.h>
#include <utility>
#include <vector>

/// Helpers for marshalling SQLite values into JUCE types.
namespace audiobatch::cache
{
/// Divisor that converts the millisecond counter durations into seconds for log output.
constexpr double millisecondsPerSecond = 1000.0;

/// How long SQLite retries a locked database before failing a statement, in milliseconds.
constexpr int busyTimeoutMilliseconds = 2000;

/// Maximum numeric tag cache size, including the explicit serializer header fields.
constexpr std::size_t maximumOverviewBytes
    = 4 * sizeof(std::uint32_t) + audiobatch::serato::Overview::maxSlices * audiobatch::serato::Overview::bandCount;

/// Zero-based columns for independent tag lookup, including the key's stored SQLite types.
namespace serato_column
{
constexpr int fileSize = 0;
constexpr int modifiedTimeMs = 1;
constexpr int parserVersion = 2;
constexpr int resultState = 3;
constexpr int decodedData = 4;
constexpr int updatedAtMs = 5;
constexpr int filePath = 6;
constexpr int tagKind = 7;
}  // namespace serato_column

/// One-based parameters for independent per-kind tag UPSERT.
namespace serato_parameter
{
constexpr int filePath = 1;
constexpr int tagKind = 2;
constexpr int fileSize = 3;
constexpr int modifiedTimeMs = 4;
constexpr int parserVersion = 5;
constexpr int resultState = 6;
constexpr int decodedData = 7;
constexpr int updatedAtMs = 8;
}  // namespace serato_parameter

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

AnalysisCache::FileIdentity::FileIdentity(const juce::File& file) :
    path(file.getFullPathName()),
    fileSize(file.getSize()),
    modifiedTimeMs(file.getLastModificationTime().toMilliseconds())
{}

bool AnalysisCache::FileIdentity::matches(const juce::File& file) const
{
    return juce::File::isAbsolutePath(path) && path == file.getFullPathName() && fileSize >= 0 && modifiedTimeMs > 0
        && file.existsAsFile() && file.getSize() == fileSize
        && file.getLastModificationTime().toMilliseconds() == modifiedTimeMs;
}

AnalysisCache::AnalysisCache()
{
    const auto appDataDirectory
        = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile(version::APP_NAME);
    databaseFile = appDataDirectory.getChildFile("analysis.db");
}

AnalysisCache::AnalysisCache(juce::File cacheDatabaseFile) : databaseFile(std::move(cacheDatabaseFile)) {}

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

bool AnalysisCache::prepare(const char* sql, sqlite3_stmt*& statement) const
{
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK) {
        return true;
    }

    utils::logError(
        "SQLite could not prepare an analysis cache query: {}", juce::String::fromUTF8(sqlite3_errmsg(database))
    );
    return false;
}

bool AnalysisCache::stepToCompletion(sqlite3_stmt* statement) const
{
    if (sqlite3_step(statement) == SQLITE_DONE) {
        return true;
    }

    utils::logError(
        "SQLite could not complete an analysis cache update: {}", juce::String::fromUTF8(sqlite3_errmsg(database))
    );
    return false;
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
    if (!prepare(sql.toRawUTF8(), statement)) {
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

bool AnalysisCache::addMissingColumns() const
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

    // Any failure from here on closes the connection again,
    // so a later call retries the whole open instead of using a database with an incomplete schema.
    utils::logDebug("Analysis cache startup: applying SQLite pragmas");
    if (!execute("PRAGMA journal_mode=WAL;") || !execute("PRAGMA synchronous=NORMAL;")) {
        closeUnlocked();
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
        closeUnlocked();
        return false;
    }

    if (!addMissingColumns()) {
        closeUnlocked();
        return false;
    }

    if (!execute(R"SQL(
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
        (juce::Time::getMillisecondCounterHiRes() - startedAtMs) / millisecondsPerSecond
    );

    return true;
}

std::optional<audiobatch::serato::TagReadResult> AnalysisCache::getSeratoTag(
    const FileIdentity& identity,
    const audiobatch::serato::TagKind kind
)
{
    using namespace audiobatch::serato;
    if (!supportsTagKind(kind) || !juce::File::isAbsolutePath(identity.path)) {
        return std::nullopt;
    }
    const juce::File file(identity.path);
    if (!identity.matches(file)) {
        return std::nullopt;
    }
    TagReadResult result {.kind = kind, .status = ReadStatus::absent, .overview = std::nullopt};
    juce::MemoryBlock data;
    {
        const juce::ScopedLock lock(mutex);
        if (database == nullptr && !openUnlocked()) {
            return std::nullopt;
        }
        constexpr auto sql = R"SQL(
                 SELECT file_size, modified_time_ms, parser_version, result_state, decoded_data, updated_at_ms,
                     file_path, tag_kind
            FROM serato_tag_cache WHERE file_path = ? AND tag_kind = ?;
        )SQL";
        sqlite3_stmt* statement = nullptr;
        if (!prepare(sql, statement)) {
            return std::nullopt;
        }
        bindText(statement, 1, identity.path);
        sqlite3_bind_int64(statement, 2, static_cast<std::uint32_t>(kind));
        bool valid = sqlite3_step(statement) == SQLITE_ROW;
        for (const auto column :
             std::array {
                 serato_column::fileSize,
                 serato_column::modifiedTimeMs,
                 serato_column::parserVersion,
                 serato_column::resultState,
                 serato_column::updatedAtMs,
                 serato_column::tagKind,
             })
        {
            valid = valid && sqlite3_column_type(statement, column) == SQLITE_INTEGER;
        }
        valid = valid && sqlite3_column_int64(statement, serato_column::fileSize) == identity.fileSize
            && sqlite3_column_int64(statement, serato_column::modifiedTimeMs) == identity.modifiedTimeMs
            && sqlite3_column_int64(statement, serato_column::parserVersion) == parserVersion(kind)
            && sqlite3_column_int64(statement, serato_column::updatedAtMs) > 0
            && sqlite3_column_type(statement, serato_column::filePath) == SQLITE_TEXT
            && columnText(statement, serato_column::filePath) == identity.path
            && sqlite3_column_int64(statement, serato_column::tagKind) == static_cast<std::uint32_t>(kind);
        const auto state = sqlite3_column_int64(statement, serato_column::resultState);
        if (valid && state == static_cast<int>(ReadStatus::present)) {
            const auto bytes = sqlite3_column_bytes(statement, serato_column::decodedData);
            valid = sqlite3_column_type(statement, serato_column::decodedData) == SQLITE_BLOB && bytes > 0
                && std::cmp_less_equal(bytes, maximumOverviewBytes);
            if (valid) {
                data = columnBlob(statement, serato_column::decodedData);
            }
        } else {
            valid = valid
                && (state == static_cast<int>(ReadStatus::absent) || state == static_cast<int>(ReadStatus::unsupported)
                    || state == static_cast<int>(ReadStatus::invalid))
                && sqlite3_column_type(statement, serato_column::decodedData) == SQLITE_NULL;
        }
        sqlite3_finalize(statement);
        if (!valid) {
            return std::nullopt;
        }
        result.status = static_cast<ReadStatus>(state);
    }
    if (result.status == ReadStatus::present) {
        const auto decoded = deserializeTag(kind, data);
        if (!decoded) {
            return std::nullopt;
        }
        result = *decoded;
    }
    return identity.matches(file) ? std::optional {std::move(result)} : std::nullopt;
}

bool AnalysisCache::storeSeratoTag(const FileIdentity& identity, const audiobatch::serato::TagReadResult& result)
{
    using namespace audiobatch::serato;
    if (!supportsTagKind(result.kind) || result.status < ReadStatus::present || result.status > ReadStatus::cancelled
        || !juce::File::isAbsolutePath(identity.path))
    {
        return false;
    }
    juce::MemoryBlock data;
    switch (result.status) {
        case ReadStatus::present:
            data = serializeTag(result);
            if (data.isEmpty() || data.getSize() > maximumOverviewBytes) {
                return false;
            }
            break;
        case ReadStatus::absent:
        case ReadStatus::unsupported:
        case ReadStatus::invalid:
            if (result.overview || result.analysis || result.autotags || result.beatGrid || result.markers
                || result.trackInfo)
            {
                return false;
            }
            break;
        case ReadStatus::ioError:
        case ReadStatus::cancelled:
            return false;
    }
    const juce::File file(identity.path);
    if (!identity.matches(file)) {
        return false;
    }
    const juce::ScopedLock lock(mutex);
    if (database == nullptr && !openUnlocked()) {
        return false;
    }
    constexpr auto sql = R"SQL(
        INSERT INTO serato_tag_cache (
            file_path, tag_kind, file_size, modified_time_ms, parser_version, result_state, decoded_data, updated_at_ms
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(file_path, tag_kind) DO UPDATE SET
            file_size = excluded.file_size, modified_time_ms = excluded.modified_time_ms,
            parser_version = excluded.parser_version, result_state = excluded.result_state,
            decoded_data = excluded.decoded_data, updated_at_ms = excluded.updated_at_ms;
    )SQL";
    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
        return false;
    }
    bindText(statement, serato_parameter::filePath, identity.path);
    sqlite3_bind_int64(statement, serato_parameter::tagKind, static_cast<std::uint32_t>(result.kind));
    sqlite3_bind_int64(statement, serato_parameter::fileSize, identity.fileSize);
    sqlite3_bind_int64(statement, serato_parameter::modifiedTimeMs, identity.modifiedTimeMs);
    sqlite3_bind_int64(statement, serato_parameter::parserVersion, parserVersion(result.kind));
    sqlite3_bind_int(statement, serato_parameter::resultState, static_cast<int>(result.status));
    if (result.status == ReadStatus::present) {
        bindBlob(statement, serato_parameter::decodedData, data);
    } else {
        sqlite3_bind_null(statement, serato_parameter::decodedData);
    }
    sqlite3_bind_int64(statement, serato_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds());
    const auto completed = stepToCompletion(statement);
    sqlite3_finalize(statement);
    return completed;
}

bool AnalysisCache::getAnalysis(const juce::File& file, AudioAnalysisRecord& record)
{
    // The file system is queried before taking the lock, so a slow network share does not block other cache calls.
    auto fileRecord = AudioAnalysisRecord::fromFile(file);
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
    if (!prepare(sql, statement)) {
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

    if (cachedFileSize != fileRecord.fileSize || cachedModifiedTime != fileRecord.modifiedTimeMs
        || cachedVersion != analysisVersion)
    {
        sqlite3_finalize(statement);
        return false;
    }

    record = std::move(fileRecord);
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
            updated_at_ms = excluded.updated_at_ms,
            custom_gain_db = CASE
                WHEN file_analysis.file_size = excluded.file_size
                    AND file_analysis.modified_time_ms = excluded.modified_time_ms
                THEN file_analysis.custom_gain_db
                ELSE excluded.custom_gain_db
            END,
            has_custom_gain = CASE
                WHEN file_analysis.file_size = excluded.file_size
                    AND file_analysis.modified_time_ms = excluded.modified_time_ms
                THEN file_analysis.has_custom_gain
                ELSE excluded.has_custom_gain
            END;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
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

    const auto completed = stepToCompletion(statement);
    sqlite3_finalize(statement);
    return completed;
}

bool AnalysisCache::getCustomGain(
    const juce::File& file,
    const std::int64_t fileSize,
    const std::int64_t modifiedTimeMs,
    float& customGainDb
)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    constexpr auto sql = R"SQL(
        SELECT custom_gain_db FROM file_analysis
        WHERE file_path = ? AND file_size = ? AND modified_time_ms = ? AND has_custom_gain = 1;
    )SQL";

    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
        return false;
    }

    bindText(statement, 1, normalizedPath(file));
    sqlite3_bind_int64(statement, 2, fileSize);
    sqlite3_bind_int64(statement, 3, modifiedTimeMs);

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
    if (!prepare(sql, statement)) {
        return false;
    }

    sqlite3_bind_double(statement, 1, customGainDb);
    sqlite3_bind_int(statement, 2, hasCustomGain ? 1 : 0);
    sqlite3_bind_int64(statement, 3, juce::Time::getCurrentTime().toMilliseconds());
    bindText(statement, 4, normalizedPath(file));

    const auto completed = stepToCompletion(statement);
    const auto updatedRows = sqlite3_changes(database);
    sqlite3_finalize(statement);
    return completed && updatedRows > 0;
}

bool AnalysisCache::removeAnalysis(const juce::File& file)
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr && !openUnlocked()) {
        return false;
    }

    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    for (const auto* sql :
         {"DELETE FROM file_analysis WHERE file_path = ?;", "DELETE FROM serato_tag_cache WHERE file_path = ?;"})
    {
        sqlite3_stmt* statement = nullptr;
        if (!prepare(sql, statement)) {
            execute("ROLLBACK;");
            return false;
        }
        bindText(statement, 1, normalizedPath(file));
        const auto completed = stepToCompletion(statement);
        sqlite3_finalize(statement);
        if (!completed) {
            execute("ROLLBACK;");
            return false;
        }
    }
    if (execute("COMMIT;")) {
        return true;
    }
    execute("ROLLBACK;");
    return false;
}

int AnalysisCache::pruneMissingFiles(const std::function<bool()>& shouldStop)
{
    /// One cached row, identified by path and the file version it describes.
    struct CachedRow {
        juce::String path;
        std::int64_t fileSize = 0;
        std::int64_t modifiedTimeMs = 0;
        std::optional<std::int64_t> tagKind;
    };

    std::vector<CachedRow> cachedRows;

    {
        const juce::ScopedLock lock(mutex);

        if (database == nullptr && !openUnlocked()) {
            return 0;
        }

        constexpr auto sql = R"SQL(
            SELECT file_path, file_size, modified_time_ms, NULL FROM file_analysis
            UNION ALL
            SELECT file_path, file_size, modified_time_ms, tag_kind FROM serato_tag_cache;
        )SQL";
        sqlite3_stmt* statement = nullptr;
        if (!prepare(sql, statement)) {
            return 0;
        }

        while (sqlite3_step(statement) == SQLITE_ROW) {
            cachedRows.push_back({
                .path = columnText(statement, 0),
                .fileSize = sqlite3_column_int64(statement, 1),
                .modifiedTimeMs = sqlite3_column_int64(statement, 2),
                .tagKind = sqlite3_column_type(statement, 3) == SQLITE_NULL
                    ? std::nullopt
                    : std::optional {sqlite3_column_int64(statement, 3)},
            });
        }

        sqlite3_finalize(statement);
    }

    int removedRows = 0;

    for (const auto& row : cachedRows) {
        if (shouldStop != nullptr && shouldStop()) {
            break;
        }

        if (!juce::File::isAbsolutePath(row.path)) {
            continue;
        }

        // A missing folder usually means an unmounted drive or network share rather than deleted files,
        // and those rows hold custom gains that would be lost for good.
        const juce::File file(row.path);
        if (file.existsAsFile() || !file.getParentDirectory().isDirectory()) {
            continue;
        }

        if (removeRowVersion(row.path, row.fileSize, row.modifiedTimeMs, row.tagKind)) {
            ++removedRows;
        }
    }

    if (removedRows > 0) {
        utils::logInfo("Removed {} cached analysis and tag rows for files that no longer exist", removedRows);
    }

    return removedRows;
}

bool AnalysisCache::removeRowVersion(
    const juce::String& path,
    const std::int64_t fileSize,
    const std::int64_t modifiedTimeMs,
    const std::optional<std::int64_t> tagKind
) const
{
    const juce::ScopedLock lock(mutex);

    if (database == nullptr) {
        return false;
    }

    const auto* sql = tagKind.has_value()
        ? "DELETE FROM serato_tag_cache WHERE file_path = ? AND file_size = ? AND modified_time_ms = ? AND tag_kind = "
          "?;"
        : "DELETE FROM file_analysis WHERE file_path = ? AND file_size = ? AND modified_time_ms = ?;";

    sqlite3_stmt* statement = nullptr;
    if (!prepare(sql, statement)) {
        return false;
    }

    bindText(statement, 1, path);
    sqlite3_bind_int64(statement, 2, fileSize);
    sqlite3_bind_int64(statement, 3, modifiedTimeMs);
    if (tagKind) {
        sqlite3_bind_int64(statement, 4, *tagKind);
    }

    const auto completed = stepToCompletion(statement);
    sqlite3_finalize(statement);
    return completed && sqlite3_changes(database) > 0;
}

bool AnalysisCache::getWaveformData(const juce::File& file, juce::MemoryBlock& waveformData)
{
    // The file system is queried before taking the lock, so a slow network share does not block other cache calls.
    const auto fileSize = file.getSize();
    const auto modifiedTimeMs = file.getLastModificationTime().toMilliseconds();
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
    if (!prepare(sql, statement)) {
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

    if (cachedFileSize != fileSize || cachedModifiedTime != modifiedTimeMs || cachedWaveformVersion != waveformVersion
        || cachedAnalysisVersion != analysisVersion)
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
    // The file system is queried before taking the lock, so a slow network share does not block other cache calls.
    const auto fileSize = file.getSize();
    const auto modifiedTimeMs = file.getLastModificationTime().toMilliseconds();
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
    if (!prepare(sql, statement)) {
        return false;
    }

    bindBlob(statement, waveform_parameter::waveformData, waveformData);
    sqlite3_bind_int(statement, waveform_parameter::waveformVersion, waveformVersion);
    sqlite3_bind_int64(statement, waveform_parameter::updatedAtMs, juce::Time::getCurrentTime().toMilliseconds());
    bindText(statement, waveform_parameter::filePath, normalizedPath(file));
    sqlite3_bind_int64(statement, waveform_parameter::fileSize, fileSize);
    sqlite3_bind_int64(statement, waveform_parameter::modifiedTimeMs, modifiedTimeMs);
    sqlite3_bind_int(statement, waveform_parameter::analysisVersion, analysisVersion);

    const auto completed = stepToCompletion(statement);
    const auto changedRows = sqlite3_changes(database);
    sqlite3_finalize(statement);
    return completed && changedRows > 0;
}
