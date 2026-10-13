/// SQLite-backed cache for audio analysis results.
/// AnalysisCache stores per-file analysis records and waveform preview data keyed by path, size, and modification time,
/// so unchanged files are not re-read on later runs.

#pragma once

#include "AudioAnalysisTypes.h"
#include "SeratoTagTypes.h"

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <optional>

struct sqlite3;
struct sqlite3_stmt;

/// Persistent SQLite-backed cache for file analysis results and waveform previews.
class AnalysisCache
{
public:
    /// Captures the cache key before tag reading. Reuse this identity for lookup, delivery and persistence.
    /// Size and modification time cannot detect external edits that deliberately preserve both values.
    struct FileIdentity {
        /// Captures the absolute path and current file stats without accessing the database.
        explicit FileIdentity(const juce::File& file);

        /// Requires the same existing file path, size and modification time, with valid captured stats.
        [[nodiscard]] bool matches(const juce::File& file) const;

        juce::String path;
        std::int64_t fileSize;
        std::int64_t modifiedTimeMs;
    };

    /// Resolves the default cache database location.
    AnalysisCache();

    /// Uses the given SQLite database file instead of the default location, for example in tests.
    explicit AnalysisCache(juce::File cacheDatabaseFile);

    /// Closes the database if it is currently open.
    ~AnalysisCache();

    AnalysisCache(const AnalysisCache&) = delete;
    AnalysisCache& operator=(const AnalysisCache&) = delete;
    AnalysisCache(AnalysisCache&&) = delete;
    AnalysisCache& operator=(AnalysisCache&&) = delete;

    /// Loads a cached analysis record for the given file when present and current.
    bool getAnalysis(const juce::File& file, AudioAnalysisRecord& record);

    /// Loads cached thumbnail waveform data for the given file when available.
    bool getWaveformData(const juce::File& file, juce::MemoryBlock& waveformData);

    /// Returns an independently versioned stable tag result, or a miss for stale or corrupt data.
    /// Validates file identity after the read. Unknown kinds and transient outcomes are never returned.
    std::optional<audiobatch::serato::TagReadResult> getSeratoTag(
        const FileIdentity& identity,
        audiobatch::serato::TagKind kind = audiobatch::serato::TagKind::overview
    );

    /// Stores independently serialized tag data or a stable negative without requiring an analysis row.
    /// Serializes outside the database lock and checks the captured identity before writing.
    /// Returns false for stale identities, inconsistent results, transient outcomes or database failures.
    bool storeSeratoTag(const FileIdentity& identity, const audiobatch::serato::TagReadResult& result);

    /// Returns the on-disk SQLite file used by the cache.
    juce::File getDatabaseFile() const;

    /// Opens the cache database and creates or migrates tables as needed.
    bool open();

    /// Stores the completed analysis record for a file.
    /// An existing custom gain is kept while the file is unchanged on disk,
    /// and replaced by the record's gain when the file's size or modification time differs,
    /// since a custom gain only applies to the version of the file it was set for.
    bool storeAnalysis(const AudioAnalysisRecord& record);

    /// Stores serialized waveform preview data for a file.
    bool storeWaveformData(const juce::File& file, const juce::MemoryBlock& waveformData);

    /// Loads the custom gain stored for a file when the stored row has the given size and modification time,
    /// so a fresh analysis of an unchanged file keeps the gain the user set.
    /// Returns false when the file has no row, no custom gain, or the row describes another version of the file.
    bool getCustomGain(const juce::File& file, std::int64_t fileSize, std::int64_t modifiedTimeMs, float& customGainDb);

    /// Updates only the user-controlled gain fields for a file.
    /// Returns true when a cached row was updated.
    /// A file without a cached row, such as one still waiting for its analysis, keeps no gain in the cache.
    bool storeCustomGain(const juce::File& file, float customGainDb, bool hasCustomGain);

    /// Removes the analysis, waveform and all independently cached tag kinds for a file, atomically.
    /// Returns true when the operation completed.
    bool removeAnalysis(const juce::File& file);

    /// Deletes the rows of files that no longer exist, for example after they were trashed or renamed.
    /// Rows whose folder is missing are kept, since that usually means an unmounted drive, not deleted files.
    /// A drive whose empty mount point folder stays behind, such as after an unclean eject,
    /// looks like deleted files in that folder, so the rows of files directly in it are removed.
    /// A row is only deleted while it still describes the version that was checked,
    /// so a file recreated in the meantime keeps its new row.
    /// The file system checks run without holding the database lock, so other cache calls are not blocked by them.
    /// Stops early, keeping the remaining rows, when shouldStop returns true.
    /// Returns the number of deleted rows across analysis and tag tables, including tag-only files.
    int pruneMissingFiles(const std::function<bool()>& shouldStop = {});

private:
    static constexpr int analysisVersion = 8;
    static constexpr int waveformVersion = 1;

    /// Returns true when the given table already has the named column.
    /// Used for lightweight schema migrations when opening the database.
    bool columnExists(const juce::String& tableName, const juce::String& columnName) const;

    /// Adds every column that a database created by an older version lacks, with its default value.
    /// Returns false when a migration statement fails.
    bool addMissingColumns() const;

    /// Closes the database handle if it is open. The caller must hold the mutex.
    void closeUnlocked();

    /// Runs a statement that returns no rows, logging any SQLite error.
    bool execute(const juce::String& sql) const;

    /// Prepares a statement, logging the SQLite error when it fails.
    bool prepare(const char* sql, sqlite3_stmt*& statement) const;

    /// Steps a statement that returns no rows to completion, logging the SQLite error when it fails.
    /// Returns true when the statement completed.
    bool stepToCompletion(sqlite3_stmt* statement) const;

    /// Opens the database and ensures the schema is current. The caller must hold the mutex.
    /// Closes the connection again when the schema cannot be prepared, so the next call retries the open.
    bool openUnlocked();

    /// Deletes an analysis row, or the specified tag kind, only while its checked size and time still match.
    /// Returns true when a row was deleted.
    bool removeRowVersion(
        const juce::String& path,
        std::int64_t fileSize,
        std::int64_t modifiedTimeMs,
        std::optional<std::int64_t> tagKind = std::nullopt
    ) const;

    /// Returns the canonical path string used as the cache key for a file.
    static juce::String normalizedPath(const juce::File& file);

    juce::CriticalSection mutex;
    juce::File databaseFile;
    sqlite3* database = nullptr;
};
