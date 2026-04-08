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

private:
    static constexpr int analysisVersion = 6;
    static constexpr int waveformVersion = 1;

    bool columnExists(const juce::String& tableName, const juce::String& columnName) const;
    void closeUnlocked();
    bool execute(const juce::String& sql) const;
    bool openUnlocked();
    static juce::String normalizedPath(const juce::File& file);

    juce::CriticalSection mutex;
    juce::File databaseFile;
    sqlite3* database = nullptr;
};
