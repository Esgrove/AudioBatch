#pragma once

#include "AudioAnalysisTypes.h"

#include <JuceHeader.h>

struct sqlite3;

class AnalysisCache
{
public:
    AnalysisCache();
    ~AnalysisCache();

    bool getAnalysis(const juce::File& file, AudioAnalysisRecord& record);
    bool getWaveformData(const juce::File& file, juce::MemoryBlock& waveformData);
    juce::File getDatabaseFile() const;
    bool open();
    bool storeAnalysis(const AudioAnalysisRecord& record);
    bool storeWaveformData(const juce::File& file, const juce::MemoryBlock& waveformData);

private:
    static constexpr int analysisVersion = 1;
    static constexpr int waveformVersion = 1;

    bool columnExists(const juce::String& tableName, const juce::String& columnName);
    bool execute(const juce::String& sql);
    bool openUnlocked();
    juce::String normalizedPath(const juce::File& file) const;

    juce::CriticalSection mutex;
    juce::File databaseFile;
    sqlite3* database = nullptr;
};
