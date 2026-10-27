/// File system helpers shared by the services and both front ends.
/// Declares FileIdentity, which recognizes an unchanged file by its path, size and modification time,
/// moving files to the system trash, the file manager label for revealing a file,
/// deleting files with error logging, choosing the default browse folder,
/// and extension checks for supported audio types.

#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <expected>
#include <functional>

namespace audiobatch::utils
{
/// Outcome of moving a file or folder to the system trash.
enum class TrashResult : std::uint8_t {
    /// The file was moved to the trash.
    moved,
    /// There was nothing to move, because the file did not exist.
    alreadyMissing,
    /// The file is still in place.
    failed,
};

/// Captures the cache key before tag reading. Reuse this identity for lookup, delivery and persistence.
/// Size and modification time cannot detect external edits that deliberately preserve both values.
struct FileIdentity {
    explicit FileIdentity(const juce::File& file);

    [[nodiscard]] bool matches(const juce::File& file) const;

    /// Absolute path of the file.
    juce::String path;
    /// Size of the file in bytes when the identity was captured.
    std::int64_t fileSize;
    /// Modification time of the file when the identity was captured, in milliseconds since the epoch.
    std::int64_t modifiedTimeMs;
};

/// Disposes of a file that a committed output replaces, and returns true when the file is gone.
using FileDisposer = std::function<bool(const juce::File&)>;

juce::File getDefaultBrowseDirectory();

[[nodiscard]] std::expected<void, juce::String> checkOutputPath(
    const juce::File& sourceFile,
    const juce::File& outputFile,
    bool overwriteExisting
);
juce::String existingOutputMessage(const juce::File& outputFile);
juce::String fileExtension(const juce::File& file);
juce::String normalizeExtension(const juce::String& extension);
juce::String revealInFileManagerLabel();

[[nodiscard]] TrashResult moveToTrash(const juce::File& file);

[[nodiscard]] bool deleteFile(const juce::File& file);
[[nodiscard]] bool disposeOfCopy(const juce::File& file, const FileDisposer& disposeFile);
bool isAiffFile(const juce::File& file);
bool isHiddenFile(const juce::File& file);
bool isMp3File(const juce::File& file);
bool isWavFile(const juce::File& file);
bool trashFile(const juce::File& file);

void revealFileInFileManager(const juce::File& file);
void revealParentDirectory(const juce::File& file);

[[nodiscard]] std::expected<void, juce::String> commitTemporaryOutput(
    const juce::TemporaryFile& temporaryFile,
    const juce::File& sourceFile,
    const juce::File& outputFile,
    bool overwriteExisting,
    const FileDisposer& disposeFile = trashFile
);
}  // namespace audiobatch::utils
