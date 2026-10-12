/// SQLite-backed cache for audio analysis results.
/// AnalysisCache stores per-file analysis records and waveform preview data keyed by path, size, and modification time,
/// so unchanged files are not re-read on later runs.

#pragma once

#include "AudioAnalysisTypes.h"

#include <JuceHeader.h>

#include <cstdint>
#include <functional>

struct sqlite3;

/// Persistent SQLite-backed cache for file analysis results and waveform previews.
class AnalysisCache
{
public:
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
    bool storeCustomGain(const juce::File& file, float customGainDb, bool hasCustomGain);

    /// Removes the cached row for a file (if any).
    /// Returns true when the operation completed.
    bool removeAnalysis(const juce::File& file);

    /// Deletes the rows of files that no longer exist, for example after they were trashed or renamed.
    /// Rows whose folder is missing are kept, since that usually means an unmounted drive, not deleted files.
    /// A row is only deleted while it still describes the version that was checked,
    /// so a file recreated in the meantime keeps its new row.
    /// The file system checks run without holding the database lock, so other cache calls are not blocked by them.
    /// Stops early, keeping the remaining rows, when shouldStop returns true.
    /// Returns the number of deleted rows.
    int pruneMissingFiles(const std::function<bool()>& shouldStop = {});

private:
    static constexpr int analysisVersion = 8;
    static constexpr int waveformVersion = 1;

    /// Returns true when the given table already has the named column.
    /// Used for lightweight schema migrations when opening the database.
    bool columnExists(const juce::String& tableName, const juce::String& columnName) const;

    /// Adds every column that a database created by an older version lacks, with its default value.
    /// Returns false when a migration statement fails.
    bool addMissingColumns();

    /// Closes the database handle if it is open. The caller must hold the mutex.
    void closeUnlocked();

    /// Runs a statement that returns no rows, logging any SQLite error.
    bool execute(const juce::String& sql) const;

    /// Opens the database and ensures the schema is current. The caller must hold the mutex.
    bool openUnlocked();

    /// Deletes the row for the path only when it still has the given size and modification time.
    /// Returns true when a row was deleted.
    bool removeRowVersion(const juce::String& path, std::int64_t fileSize, std::int64_t modifiedTimeMs);

    /// Returns the canonical path string used as the cache key for a file.
    static juce::String normalizedPath(const juce::File& file);

    juce::CriticalSection mutex;
    juce::File databaseFile;
    sqlite3* database = nullptr;
};
