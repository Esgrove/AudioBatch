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
    juce::File getDatabaseFile() const;
    bool open();
    bool storeAnalysis(const AudioAnalysisRecord& record);

private:
    static constexpr int analysisVersion = 1;

    bool execute(const juce::String& sql);
    bool openUnlocked();
    juce::String normalizedPath(const juce::File& file) const;

    juce::CriticalSection mutex;
    juce::File databaseFile;
    sqlite3* database = nullptr;
};