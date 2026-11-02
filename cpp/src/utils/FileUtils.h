/// File system helpers shared by the services and both front ends.
/// Declares FileIdentity, which checks path, size, modification time, native file identifiers and precise timestamps,
/// moving files to the system trash, the file manager label for revealing a file,
/// revealing files and opening their folders grouped by parent folder,
/// deleting files with error logging, choosing the default browse folder,
/// and extension checks for supported audio types.

#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

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

/// Captures file identity before tag reading or output rendering.
/// Reuse it for cache lookup, delivery, persistence and checks at output transaction boundaries.
/// Native file identifiers and high resolution timestamps reject ordinary same-path replacements.
/// This is a stale-file guard, not an atomic conditional filesystem operation.
struct FileIdentity {
    explicit FileIdentity(const juce::File& file);

    [[nodiscard]] bool matches(const juce::File& file) const;
    [[nodiscard]] bool owns(const juce::File& file) const;

    /// Absolute path of the file.
    juce::String path;

    /// Platform device or volume and file identifier, absent when identity capture failed.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> platformId;
    /// Nanosecond modification and change times on POSIX, high resolution write time on Windows.
    std::pair<std::int64_t, std::int64_t> preciseTimes;

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

/// Most folders that revealing the selected files or opening their parent folders opens at once,
/// so a large selection does not open hundreds of windows. The Rust application uses the same limit.
inline constexpr int maximumRevealFolders {10};

/// Selected files grouped by their parent folder, in the order the folders first appear.
struct FolderGroups {
    /// Number of further folders left out because of the limit.
    int skippedFolders {0};
    /// Each folder with its files, in selection order, at most the requested number of folders.
    std::vector<std::pair<juce::File, juce::Array<juce::File>>> groups;
};

[[nodiscard]] FolderGroups groupByParentFolder(const juce::Array<juce::File>& files, int maximumFolders);
[[nodiscard]] TrashResult moveToTrash(const juce::File& file);
juce::File getDefaultBrowseDirectory();

juce::String existingOutputMessage(const juce::File& outputFile);
juce::String fileExtension(const juce::File& file);
juce::String normalizeExtension(const juce::String& extension);
juce::String resolvedOutputKey(const juce::File& file);
juce::String revealInFileManagerLabel();

int openParentDirectories(const juce::Array<juce::File>& files);
int revealFilesInFileManager(const juce::Array<juce::File>& files);

[[nodiscard]] bool deleteFile(const juce::File& file);
[[nodiscard]] bool disposeOfCopy(juce::File file, const FileDisposer& disposeFile);
bool isAiffFile(const juce::File& file);
bool isHiddenFile(const juce::File& file);
bool isMp3File(const juce::File& file);
bool isWavFile(const juce::File& file);
bool trashFile(const juce::File& file);

[[nodiscard]] std::expected<void, juce::String> checkOutputPath(
    const juce::File& sourceFile,
    const juce::File& outputFile,
    bool overwriteExisting
);
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
