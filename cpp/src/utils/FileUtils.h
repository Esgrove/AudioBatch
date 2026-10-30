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
#include <optional>
#include <utility>

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
/// Native file identifiers and high resolution timestamps reject ordinary same-path replacements.
/// This is a stale-file guard, not an atomic conditional filesystem operation.
struct FileIdentity {
    explicit FileIdentity(const juce::File& file);

    [[nodiscard]] bool matches(const juce::File& file) const;
    [[nodiscard]] bool owns(const juce::File& file) const;

    /// Platform device or volume and file identifier, absent when identity capture failed.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> platformId;
    /// Nanosecond modification and change times on POSIX, high resolution write time on Windows.
    std::pair<std::int64_t, std::int64_t> preciseTimes;

    /// Absolute path of the file.
    juce::String path;
    /// Size of the file in bytes when the identity was captured.
    std::int64_t fileSize;
    /// Modification time of the file when the identity was captured, in milliseconds since the epoch.
    std::int64_t modifiedTimeMs;
};

/// Rendered output whose automatic cleanup can be disabled when recovery cannot move the audio.
struct OutputTemporaryFile {
    explicit OutputTemporaryFile(const juce::File& output);
    ~OutputTemporaryFile();
    OutputTemporaryFile(const OutputTemporaryFile&) = delete;
    OutputTemporaryFile& operator=(const OutputTemporaryFile&) = delete;
    OutputTemporaryFile(OutputTemporaryFile&&) = delete;
    OutputTemporaryFile& operator=(OutputTemporaryFile&&) = delete;
    [[nodiscard]] const juce::File& getFile() const;
    void preserve() const;

private:
    juce::File file;
    mutable bool retained {false};
};

/// Deterministic fault injection at output transaction ownership boundaries.
using OutputCommitHook = std::function<void(const juce::String&)>;

/// Disposes of a file that a committed output replaces, and returns true when the file is gone.
using FileDisposer = std::function<bool(const juce::File&)>;

juce::File getDefaultBrowseDirectory();

[[nodiscard]] std::expected<void, juce::String> checkOutputPath(
    const juce::File& sourceFile,
    const juce::File& outputFile,
    bool overwriteExisting
);
juce::String existingOutputMessage(const juce::File& outputFile);
juce::String resolvedOutputKey(const juce::File& file);
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
    const OutputTemporaryFile& temporaryFile,
    const juce::File& sourceFile,
    const juce::File& outputFile,
    bool overwriteExisting,
    const FileIdentity& expectedSource,
    const FileDisposer& disposeFile = trashFile,
    const OutputCommitHook& hook = {}
);

}  // namespace audiobatch::utils
