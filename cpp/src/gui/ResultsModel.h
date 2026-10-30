/// The rows of the GUI results table.
/// Declares ResultsModel, which owns the analysis records shown in the table.
/// It keeps an index from full path to row, so lookups by path do not scan the rows.
/// It merges analysis and operation results, adds pending placeholders, removes rows, and sorts them,
/// with no dependency on the table component, so its rules are unit tested directly.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "gui/ResultsSorting.h"
#include <unordered_map>

#include <JuceHeader.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace audiobatch
{
/// Where merging a record put it, and whether a retained gain edit must be persisted.
struct MergedRecord {
    /// The row of the merged record, valid until rows are removed or sorted.
    int row {-1};
    /// True when a retained gain or explicit clear overrode the incoming gain,
    /// which the cache may not have stored for this version of the file yet.
    bool customGainCarriedOver {false};
};

/// An explicit gain edit, including a clear, belonging only to the edited version of a file.
struct CustomGainEdit {
    /// File size when the gain was edited.
    std::int64_t fileSize {0};
    /// Modification time when the gain was edited.
    std::int64_t modifiedTimeMs {0};
    /// The authoritative gain, empty after an explicit clear.
    std::optional<Decibels> gain;
};

/// The records of the results table, with each full path in at most one row.
/// Rows keep their order until the model is sorted, and merging appends new paths at the end.
/// Only used on the message thread.
class ResultsModel
{
public:
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] int size() const noexcept;
    [[nodiscard]] bool containsRow(int row) const noexcept;
    [[nodiscard]] std::optional<int> findRow(const juce::String& fullPath) const;
    [[nodiscard]] const AudioAnalysisRecord& operator[](int row) const;
    [[nodiscard]] std::vector<AudioAnalysisRecord>::const_iterator begin() const noexcept;
    [[nodiscard]] std::vector<AudioAnalysisRecord>::const_iterator end() const noexcept;

    bool addPending(const juce::File& file);
    bool setCustomGain(int row, std::optional<Decibels> gain);
    MergedRecord merge(AudioAnalysisRecord record);
    MergedRecord replaceAfterOperation(const juce::String& originalPath, AudioAnalysisRecord output);
    void clear() noexcept;
    void removePaths(const juce::StringArray& paths);
    void setRecord(int row, AudioAnalysisRecord record);
    void sort(const results_sorting::SortState& state);

private:
    void rebuildIndex();

    /// The rows in table order.
    std::vector<AudioAnalysisRecord> records;
    /// The row of each record by its full path, rebuilt whenever rows move.
    std::unordered_map<juce::String, std::size_t> rowsByPath;
    /// Explicit edits remain authoritative, including an empty gain, until the file identity changes.
    std::unordered_map<juce::String, CustomGainEdit> customGainEdits;
};

juce::StringArray remapSelectedPaths(
    const juce::StringArray& selectedPaths,
    const std::unordered_map<juce::String, juce::String>& movedPaths
);
}  // namespace audiobatch
