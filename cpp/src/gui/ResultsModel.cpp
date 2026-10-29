/// Implementation of ResultsModel, the rows of the results table.
/// Covers the path index that follows the rows through removals and sorting,
/// and the rule that a custom gain survives a new record only while the file is unchanged on disk.

#include "gui/ResultsModel.h"

#include "analysis/AudioAnalysisTypes.h"
#include "gui/ResultsSorting.h"
#include <unordered_set>

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
/// Copies the custom gain of the record in the table onto a new record for the same path,
/// when the file still has the size and modification time the gain was set for.
/// A custom gain only applies to the version of the file it was set for, so a changed file starts without one.
/// Returns true when the gain was carried over.
bool keepCustomGainOfUnchangedFile(const AudioAnalysisRecord& existing, AudioAnalysisRecord& incoming)
{
    if (!existing.customGain || existing.fileSize != incoming.fileSize
        || existing.modifiedTimeMs != incoming.modifiedTimeMs)
    {
        return false;
    }

    incoming.customGain = existing.customGain;
    return true;
}
}  // namespace

/// True when the table has no rows.
bool ResultsModel::empty() const noexcept
{
    return records.empty();
}

/// Returns the number of rows, as the int row numbers JUCE tables use.
int ResultsModel::size() const noexcept
{
    return static_cast<int>(records.size());
}

/// True when the row number refers to a row, which a stale row number from the table may not.
bool ResultsModel::containsRow(const int row) const noexcept
{
    return juce::isPositiveAndBelow(row, size());
}

/// Returns the row of the record with the given full path, or nothing when the path has no row.
std::optional<int> ResultsModel::findRow(const juce::String& fullPath) const
{
    if (const auto found = rowsByPath.find(fullPath); found != rowsByPath.end()) {
        return static_cast<int>(found->second);
    }

    return std::nullopt;
}

/// Returns the record of a row, which must exist.
const AudioAnalysisRecord& ResultsModel::operator[](const int row) const
{
    jassert(containsRow(row));
    return records[static_cast<std::size_t>(row)];
}

/// Returns an iterator to the first row, so the rows can be read with a range-based for loop.
std::vector<AudioAnalysisRecord>::const_iterator ResultsModel::begin() const noexcept
{
    return records.begin();
}

/// Returns an iterator past the last row.
std::vector<AudioAnalysisRecord>::const_iterator ResultsModel::end() const noexcept
{
    return records.end();
}

/// Appends a pending placeholder row for a file that is about to be analyzed.
/// Returns false and leaves the rows unchanged when the file already has a row.
bool ResultsModel::addPending(const juce::File& file)
{
    if (rowsByPath.contains(file.getFullPathName())) {
        return false;
    }

    auto placeholder = AudioAnalysisRecord::fromFile(file);
    placeholder.status = AudioAnalysisStatus::pending;
    rowsByPath.try_emplace(placeholder.fullPath, records.size());
    records.push_back(std::move(placeholder));
    return true;
}

/// Sets or clears the custom gain of a row, which must exist.
/// Returns true when the gain changed.
bool ResultsModel::setCustomGain(const int row, const std::optional<Decibels> gain)
{
    jassert(containsRow(row));
    auto& record = records[static_cast<std::size_t>(row)];

    if (sameCustomGain(record.customGain, gain)) {
        return false;
    }

    record.customGain = gain;
    return true;
}

/// Replaces the row with the same full path as the record, or appends the record as a new row.
/// A replaced row passes its custom gain on while the file is unchanged on disk.
MergedRecord ResultsModel::merge(AudioAnalysisRecord record)
{
    if (const auto existing = rowsByPath.find(record.fullPath); existing != rowsByPath.end()) {
        auto& row = records[existing->second];
        const auto incomingGain = record.customGain;
        const auto carriedOver
            = keepCustomGainOfUnchangedFile(row, record) && !sameCustomGain(incomingGain, record.customGain);
        row = std::move(record);
        return {.row = static_cast<int>(existing->second), .customGainCarriedOver = carriedOver};
    }

    const auto row = records.size();
    rowsByPath.try_emplace(record.fullPath, row);
    records.push_back(std::move(record));
    return {.row = static_cast<int>(row), .customGainCarriedOver = false};
}

/// Merges the output of a normalize or process run in place of the row of its source.
/// A source converted to a new path loses its row, since the conversion moved it to the trash.
/// An output that kept the source path keeps the custom gain of an unchanged file,
/// such as a file that was already at the normalization target.
MergedRecord ResultsModel::replaceAfterOperation(const juce::String& originalPath, AudioAnalysisRecord output)
{
    if (originalPath != output.fullPath) {
        removePaths(juce::StringArray {originalPath});
    }

    return merge(std::move(output));
}

/// Removes every row.
void ResultsModel::clear() noexcept
{
    records.clear();
    rowsByPath.clear();
}

/// Removes the rows of the given full paths and renumbers the rows after them.
void ResultsModel::removePaths(const juce::StringArray& paths)
{
    const std::unordered_set<juce::String> removedPaths(paths.begin(), paths.end());
    const auto removed = std::erase_if(records, [&removedPaths](const AudioAnalysisRecord& record) {
        return removedPaths.contains(record.fullPath);
    });

    if (removed > 0) {
        rebuildIndex();
    }
}

/// Replaces the record of a row, which must exist, with a record for the same path,
/// without carrying anything over from the old record.
/// A record for another path goes through replaceAfterOperation, which keeps the paths unique.
void ResultsModel::setRecord(const int row, AudioAnalysisRecord record)
{
    jassert(containsRow(row));
    auto& existing = records[static_cast<std::size_t>(row)];
    jassert(existing.fullPath == record.fullPath);
    existing = std::move(record);
}

/// Sorts the rows by the primary and secondary sort columns of the state.
/// Failed files always sort last, and file name plus full path act as the final tie breakers,
/// keeping the order stable across incremental result updates.
void ResultsModel::sort(const results_sorting::SortState& state)
{
    results_sorting::sortRecords(records, state);
    rebuildIndex();
}

/// Maps every full path to its current row again after rows moved.
void ResultsModel::rebuildIndex()
{
    rowsByPath.clear();
    rowsByPath.reserve(records.size());

    for (std::size_t row = 0; row < records.size(); ++row) {
        rowsByPath.try_emplace(records[row].fullPath, row);
    }
}
}  // namespace audiobatch
