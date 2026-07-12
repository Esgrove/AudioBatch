#pragma once

#include "AudioAnalysisTypes.h"

#include <JuceHeader.h>

struct sqlite3;

/// Persistent SQLite-backed cache for file analysis results and waveform previews.
class AnalysisCache
{
public:
    /// Resolves the default cache database location.
    AnalysisCache();

    /// Closes the database if it is currently open.
    ~AnalysisCache();

    /// Loads a cached analysis record for the given file when present and current.
    bool getAnalysis(const juce::File& file, AudioAnalysisRecord& record);

    /// Loads cached thumbnail waveform data for the given file when available.
    bool getWaveformData(const juce::File& file, juce::MemoryBlock& waveformData);

    /// Returns the on-disk SQLite file used by the cache.
    juce::File getDatabaseFile() const;

    /// Opens the cache database and creates or migrates tables as needed.
    bool open();

    /// Stores the completed analysis record for a file.
    bool storeAnalysis(const AudioAnalysisRecord& record);

    /// Stores serialized waveform preview data for a file.
    bool storeWaveformData(const juce::File& file, const juce::MemoryBlock& waveformData);

    /// Updates only the user-controlled gain fields for a file.
    bool storeCustomGain(const juce::File& file, float customGainDb, bool hasCustomGain);

    /// Removes the cached row for a file (if any).
    /// Returns true when the operation completed.
    bool removeAnalysis(const juce::File& file);

private:
    static constexpr int analysisVersion = 7;
    static constexpr int waveformVersion = 1;

    /// Returns true when the given table already has the named column.
    /// Used for lightweight schema migrations when opening the database.
    bool columnExists(const juce::String& tableName, const juce::String& columnName) const;

    /// Closes the database handle if it is open. The caller must hold the mutex.
    void closeUnlocked();

    /// Runs a statement that returns no rows, logging any SQLite error.
    bool execute(const juce::String& sql) const;

    /// Opens the database and ensures the schema is current. The caller must hold the mutex.
    bool openUnlocked();

    /// Returns the canonical path string used as the cache key for a file.
    static juce::String normalizedPath(const juce::File& file);

    juce::CriticalSection mutex;
    juce::File databaseFile;
    sqlite3* database = nullptr;
};
