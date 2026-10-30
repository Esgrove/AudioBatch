/// Implementation of file system helpers and default browse-folder selection.
/// Moving to the trash uses the shell's IFileOperation on Windows,
/// with a progress sink that refuses deletes the shell would perform permanently,
/// and juce::File::moveToTrash elsewhere.

#include "utils/FileUtils.h"

#include "utils/Log.h"
#include "utils/StringFormat.h"
#include <system_error>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <utility>
#if !JUCE_WINDOWS
#include <sys/stat.h>
#endif

#if JUCE_WINDOWS
#include <juce_core/native/juce_BasicNativeHeaders.h>

#include <memory>
#include <shobjidl.h>

#if JUCE_MSVC
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#endif
#endif

namespace audiobatch::utils
{
namespace
{
/// Highest number tried when a hidden backup or recovery name is already occupied.
constexpr int maximumBackupIndex {1000};
/// Converts POSIX seconds to the high resolution timestamp used for identity checks.
constexpr std::int64_t nanosecondsPerSecond {1'000'000'000};

/// An approved output retained under an exclusively claimed backup entry.
struct OutputBackup {
    juce::File file;
    std::optional<FileIdentity> identity;
};

#if JUCE_WINDOWS
/// Releases a COM interface when its owning pointer goes out of scope.
struct ComReleaser {
    /// Releases the interface, ignoring null pointers.
    void operator()(IUnknown* object) const noexcept
    {
        if (object != nullptr) {
            object->Release();
        }
    }
};

/// Progress sink that aborts a delete the shell would perform permanently instead of recycling it.
/// Drives without a recycle bin, such as network shares and some removable drives,
/// delete for good even with FOF_ALLOWUNDO, without a prompt when FOF_NOCONFIRMATION is set.
/// The sink lives on the stack for the duration of one operation, so reference counting is a no-op.
class RecycleOnlyProgressSink final : public IFileOperationProgressSink
{
public:
    /// Returns this object for IUnknown and IFileOperationProgressSink.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }

        if (interfaceId == __uuidof(IUnknown) || interfaceId == __uuidof(IFileOperationProgressSink)) {
            *object = static_cast<IFileOperationProgressSink*>(this);
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    /// Does nothing, because the sink is owned by the stack frame that runs the operation.
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return 1;
    }

    /// Does nothing, because the sink is owned by the stack frame that runs the operation.
    ULONG STDMETHODCALLTYPE Release() override
    {
        return 1;
    }

    /// Aborts the delete when the shell will not recycle the item.
    HRESULT STDMETHODCALLTYPE PreDeleteItem(const DWORD flags, IShellItem* /*item*/) override
    {
        if ((flags & static_cast<DWORD>(TSF_DELETE_RECYCLE_IF_POSSIBLE)) == 0) {
            refusedPermanentDelete = true;
            return E_ABORT;
        }

        return S_OK;
    }

    /// Accepts the start of the operations.
    HRESULT STDMETHODCALLTYPE StartOperations() override
    {
        return S_OK;
    }

    /// Accepts the end of the operations.
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT /*result*/) override
    {
        return S_OK;
    }

    /// Accepts renames, which this operation does not perform.
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD /*flags*/, IShellItem* /*item*/, LPCWSTR /*newName*/) override
    {
        return S_OK;
    }

    /// Accepts renames, which this operation does not perform.
    HRESULT STDMETHODCALLTYPE PostRenameItem(
        DWORD /*flags*/,
        IShellItem* /*item*/,
        LPCWSTR /*newName*/,
        HRESULT /*result*/,
        IShellItem* /*newItem*/
    ) override
    {
        return S_OK;
    }

    /// Accepts moves, which this operation does not perform.
    HRESULT STDMETHODCALLTYPE PreMoveItem(
        DWORD /*flags*/,
        IShellItem* /*item*/,
        IShellItem* /*destinationFolder*/,
        LPCWSTR /*newName*/
    ) override
    {
        return S_OK;
    }

    /// Accepts moves, which this operation does not perform.
    HRESULT STDMETHODCALLTYPE PostMoveItem(
        DWORD /*flags*/,
        IShellItem* /*item*/,
        IShellItem* /*destinationFolder*/,
        LPCWSTR /*newName*/,
        HRESULT /*result*/,
        IShellItem* /*newItem*/
    ) override
    {
        return S_OK;
    }

    /// Accepts copies, which this operation does not perform.
    HRESULT STDMETHODCALLTYPE PreCopyItem(
        DWORD /*flags*/,
        IShellItem* /*item*/,
        IShellItem* /*destinationFolder*/,
        LPCWSTR /*newName*/
    ) override
    {
        return S_OK;
    }

    /// Accepts copies, which this operation does not perform.
    HRESULT STDMETHODCALLTYPE PostCopyItem(
        DWORD /*flags*/,
        IShellItem* /*item*/,
        IShellItem* /*destinationFolder*/,
        LPCWSTR /*newName*/,
        HRESULT /*result*/,
        IShellItem* /*newItem*/
    ) override
    {
        return S_OK;
    }

    /// Accepts the outcome of a delete.
    HRESULT STDMETHODCALLTYPE
        PostDeleteItem(DWORD /*flags*/, IShellItem* /*item*/, HRESULT /*result*/, IShellItem* /*newItem*/) override
    {
        return S_OK;
    }

    /// Accepts new items, which this operation does not create.
    HRESULT STDMETHODCALLTYPE
        PreNewItem(DWORD /*flags*/, IShellItem* /*destinationFolder*/, LPCWSTR /*newName*/) override
    {
        return S_OK;
    }

    /// Accepts new items, which this operation does not create.
    HRESULT STDMETHODCALLTYPE PostNewItem(
        DWORD /*flags*/,
        IShellItem* /*destinationFolder*/,
        LPCWSTR /*newName*/,
        LPCWSTR /*templateName*/,
        DWORD /*fileAttributes*/,
        HRESULT /*result*/,
        IShellItem* /*newItem*/
    ) override
    {
        return S_OK;
    }

    /// Ignores progress updates.
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT /*workTotal*/, UINT /*workSoFar*/) override
    {
        return S_OK;
    }

    /// Ignores timer changes.
    HRESULT STDMETHODCALLTYPE ResetTimer() override
    {
        return S_OK;
    }

    /// Ignores timer changes.
    HRESULT STDMETHODCALLTYPE PauseTimer() override
    {
        return S_OK;
    }

    /// Ignores timer changes.
    HRESULT STDMETHODCALLTYPE ResumeTimer() override
    {
        return S_OK;
    }

    /// True when a delete was aborted because the shell would not have recycled the item.
    bool refusedPermanentDelete {false};
};

/// Owning pointer to a COM interface, which releases the interface when it goes out of scope.
template<typename Interface>
using ComPointer = std::unique_ptr<Interface, ComReleaser>;

/// Moves the file or folder to the recycle bin through IFileOperation,
/// refusing to delete it when the drive has no recycle bin.
/// IFileOperation requires a single-threaded apartment,
/// so the calling thread is initialized for one when it has no apartment yet,
/// and the move fails on a thread that already joined the multithreaded apartment.
bool moveToRecycleBin(const juce::File& file)
{
    const auto apartmentResult = CoInitializeEx(
        nullptr, static_cast<DWORD>(COINIT_APARTMENTTHREADED) | static_cast<DWORD>(COINIT_DISABLE_OLE1DDE)
    );

    if (FAILED(apartmentResult)) {
        logError("Could not initialize COM to move {} to the recycle bin", file.getFullPathName().quoted());
        return false;
    }
    // Declared before the interfaces, so they are released before the apartment is torn down.
    const juce::ScopeGuard uninitializeApartment {[] { CoUninitialize(); }};

    ComPointer<IFileOperation> operation;
    constexpr DWORD classContexts = static_cast<DWORD>(CLSCTX_INPROC_SERVER) | static_cast<DWORD>(CLSCTX_INPROC_HANDLER)
        | static_cast<DWORD>(CLSCTX_LOCAL_SERVER) | static_cast<DWORD>(CLSCTX_REMOTE_SERVER);
    if (FAILED(CoCreateInstance(
            CLSID_FileOperation, nullptr, classContexts, __uuidof(IFileOperation), std::out_ptr<void*>(operation)
        )))
    {
        return false;
    }

    ComPointer<IShellItem> item;
    const auto path = file.getFullPathName().replaceCharacter('/', '\\');
    if (FAILED(SHCreateItemFromParsingName(
            path.toWideCharPointer(), nullptr, __uuidof(IShellItem), std::out_ptr<void*>(item)
        )))
    {
        return false;
    }

    RecycleOnlyProgressSink progressSink;
    DWORD adviseCookie = 0;
    if (FAILED(operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMATION))
        || FAILED(operation->Advise(&progressSink, &adviseCookie)))
    {
        return false;
    }
    // Declared after the sink, so the operation stops calling it before the sink goes out of scope.
    const juce::ScopeGuard unadvise {[&operation, adviseCookie] { operation->Unadvise(adviseCookie); }};

    const auto deleted
        = SUCCEEDED(operation->DeleteItem(item.get(), nullptr)) && SUCCEEDED(operation->PerformOperations());
    BOOL anyAborted = FALSE;
    const auto abortQueried = SUCCEEDED(operation->GetAnyOperationsAborted(&anyAborted));

    if (progressSink.refusedPermanentDelete) {
        logError(
            "Did not move {} to the recycle bin, because its drive would delete it permanently",
            file.getFullPathName().quoted()
        );
    }

    return deleted && abortQueried && anyAborted == FALSE && !progressSink.refusedPermanentDelete && !file.exists();
}
#endif

/// Converts a JUCE path without losing native Unicode characters.
std::filesystem::path nativePath(const juce::File& file)
{
#if JUCE_WINDOWS
    return {file.getFullPathName().toWideCharPointer()};
#else
    return {file.getFullPathName().toRawUTF8()};
#endif
}

/// Claims a new directory entry atomically, never replacing an occupant or dangling symlink.
bool linkWithoutReplacing(const juce::File& source, const juce::File& destination)
{
    std::error_code error;
    std::filesystem::create_hard_link(nativePath(source), nativePath(destination), error);
    return !error;
}

/// Restores only an owned backup into an empty output entry, retaining it on failure.
void restoreOutputBackup(
    const juce::File& backupFile,
    const std::optional<FileIdentity>& identity,
    const juce::File& outputFile
)
{
    if (!identity || !identity->owns(backupFile)) {
        return;
    }
    if (linkWithoutReplacing(backupFile, outputFile)) {
        if (identity->owns(backupFile)) {
            static_cast<void>(deleteFile(backupFile));
        }
    } else {
        logError(
            "Could not restore {}. The previous output was kept at {}",
            outputFile.getFullPathName().quoted(),
            backupFile.getFullPathName().quoted()
        );
    }
}

/// Moves the new audio out of the temporary file after an in-place replace lost the original,
/// so the temporary file's destructor cannot delete the only remaining copy of the audio.
/// On Windows, ReplaceFile can delete the original and then fail to move the replacement into place.
/// Returns the user-facing error describing where the new audio was kept.
juce::String keepNewAudioAfterLostOriginal(const OutputTemporaryFile& temporaryFile, const juce::File& outputFile)
{
    for (int index = 1; index <= maximumBackupIndex; ++index) {
        const auto suffix = index == 1 ? juce::String() : juce::String(index);
        const auto keptFile = outputFile.getSiblingFile(
            outputFile.getFileNameWithoutExtension() + " (recovered)" + suffix + outputFile.getFileExtension()
        );
        if (linkWithoutReplacing(temporaryFile.getFile(), keptFile)) {
            return format("Could not publish the new audio. The new audio was kept as {}", keptFile.getFullPathName());
        }
    }
    temporaryFile.preserve();
    logError("The rendered audio was retained at {}", temporaryFile.getFile().getFullPathName().quoted());
    return format(
        "Could not publish the new audio. The new audio was kept as {}", temporaryFile.getFile().getFullPathName()
    );
}

/// Reports a stale source consistently at every metadata and destruction boundary.
std::unexpected<juce::String> sourceChangedFailure()
{
    return std::unexpected(
        juce::String("The source file changed while audio was being rendered. The original file was left unchanged.")
    );
}

/// Retains the approved destination in an exclusively claimed backup before vacating its path.
std::expected<OutputBackup, juce::String> claimOutputBackup(const juce::File& outputFile, const OutputCommitHook& hook)
{
    const FileIdentity identity(outputFile);
    for (int index = 0; index <= maximumBackupIndex; ++index) {
        const auto suffix = index == 0 ? juce::String() : "." + juce::String(index);
        const auto candidate = outputFile.getSiblingFile("." + outputFile.getFileName() + ".backup" + suffix);
        if (hook) {
            hook("before_backup_claim");
        }
        if (!linkWithoutReplacing(outputFile, candidate)) {
            continue;
        }
        if (!identity.owns(outputFile) || !identity.owns(candidate)) {
            return sourceChangedFailure();
        }
        if (!deleteFile(outputFile)) {
            return std::unexpected(juce::String("Could not move the existing output file aside"));
        }
        return OutputBackup {.file = candidate, .identity = identity};
    }
    return std::unexpected(juce::String("Could not move the existing output file aside"));
}

/// Reverts only the transaction's published entry and preserves audio when another owner blocks restoration.
std::unexpected<juce::String> rollbackPublishedOutput(
    const OutputTemporaryFile& temporaryFile,
    const juce::File& outputFile,
    const FileIdentity& renderedIdentity,
    const OutputBackup& backup,
    const bool sourceMatches,
    const OutputCommitHook& hook
)
{
    if (hook) {
        hook("before_rollback");
    }
    if (renderedIdentity.owns(outputFile)) {
        static_cast<void>(deleteFile(outputFile));
    }
    restoreOutputBackup(backup.file, backup.identity, outputFile);
    const bool backupNotRestored = backup.identity && !backup.identity->owns(outputFile);
    const bool unownedOccupant = !backup.identity && outputFile.exists() && !renderedIdentity.owns(outputFile);
    if (backupNotRestored || unownedOccupant) {
        return std::unexpected(keepNewAudioAfterLostOriginal(temporaryFile, outputFile));
    }
    if (!sourceMatches) {
        return sourceChangedFailure();
    }
    if (renderedIdentity.owns(temporaryFile.getFile())) {
        static_cast<void>(deleteFile(temporaryFile.getFile()));
    }
    return std::unexpected(juce::String("Could not move the original audio file to the system trash"));
}
}  // namespace

/// Captures the absolute path and current file stats.
FileIdentity::FileIdentity(const juce::File& file) :
    path(file.getFullPathName()),
    fileSize(file.getSize()),
    modifiedTimeMs(file.getLastModificationTime().toMilliseconds())
{
#if JUCE_WINDOWS
    const auto handle = CreateFileW(
        file.getFullPathName().toWideCharPointer(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (handle != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION information {};
        if (GetFileInformationByHandle(handle, &information)) {
            platformId = std::pair {
                static_cast<std::uint64_t>(information.dwVolumeSerialNumber),
                (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) | information.nFileIndexLow
            };
            preciseTimes.first = static_cast<std::int64_t>(
                (static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32U)
                | information.ftLastWriteTime.dwLowDateTime
            );
        }
        CloseHandle(handle);
    }
#else
    struct stat information {};
    if (::stat(file.getFullPathName().toRawUTF8(), &information) == 0 && S_ISREG(information.st_mode)) {
        platformId = std::pair {
            static_cast<std::uint64_t>(information.st_dev), static_cast<std::uint64_t>(information.st_ino)
        };
#if JUCE_MAC
        preciseTimes
            = {information.st_mtimespec.tv_sec * nanosecondsPerSecond + information.st_mtimespec.tv_nsec,
               information.st_ctimespec.tv_sec * nanosecondsPerSecond + information.st_ctimespec.tv_nsec};
#else
        preciseTimes
            = {information.st_mtim.tv_sec * nanosecondsPerSecond + information.st_mtim.tv_nsec,
               information.st_ctim.tv_sec * nanosecondsPerSecond + information.st_ctim.tv_nsec};
#endif
    }
#endif
}

/// Requires the same existing source and content stats, including native file identity.
/// This is a stale-source guard, not a filesystem compare-and-swap operation.
bool FileIdentity::matches(const juce::File& file) const
{
    const FileIdentity current(file);
    return path == current.path && owns(file) && preciseTimes == current.preciseTimes;
}

/// Recognizes an owned file after a rename or link changed its status time.
bool FileIdentity::owns(const juce::File& file) const
{
    const FileIdentity current(file);
    return platformId && platformId == current.platformId && file.existsAsFile() && fileSize == current.fileSize
        && modifiedTimeMs == current.modifiedTimeMs && preciseTimes.first == current.preciseTimes.first;
}

/// Chooses an unpredictable hidden path whose cleanup can be disabled on a recovery failure.
OutputTemporaryFile::OutputTemporaryFile(const juce::File& output) :
    file(output.getSiblingFile(
        "." + output.getFileNameWithoutExtension() + "_temp" + juce::Uuid().toDashedString() + output.getFileExtension()
    ))
{}

/// Removes only the temporary output when recovery has not retained it.
OutputTemporaryFile::~OutputTemporaryFile()
{
    if (!retained) {
        static_cast<void>(deleteFile(file));
    }
}

/// Returns the rendered file path.
const juce::File& OutputTemporaryFile::getFile() const
{
    return file;
}

/// Keeps the original temporary path as the last recovery fallback.
void OutputTemporaryFile::preserve() const
{
    retained = true;
}

/// Resolves only the existing destination parent, so absent outputs and directory aliases share a key.
juce::String resolvedOutputKey(const juce::File& file)
{
    std::error_code error;
    auto parent = std::filesystem::weakly_canonical(nativePath(file.getParentDirectory()), error);
    if (error) {
        parent = nativePath(file.getParentDirectory());
    }
    const auto resolved = parent / nativePath(file).filename();
#if JUCE_WINDOWS
    const auto text = juce::String(resolved.wstring().c_str());
#else
    const auto text = juce::String::fromUTF8(resolved.string().c_str());
#endif
    return juce::File::areFileNamesCaseSensitive() ? text : text.toLowerCase();
}

/// Moves a file or folder to the OS trash or recycle bin.
/// A missing file is reported separately from a moved one,
/// so a caller that needs the file to end up in the trash can tell the two apart.
/// On Windows, a file on a drive without a recycle bin, such as a network share, is left in place and fails,
/// because the shell would otherwise delete it permanently.
TrashResult moveToTrash(const juce::File& file)
{
    if (!file.exists()) {
        return TrashResult::alreadyMissing;
    }

#if JUCE_WINDOWS
    const bool moved = moveToRecycleBin(file);
#else
    const bool moved = file.moveToTrash();
#endif

    return moved ? TrashResult::moved : TrashResult::failed;
}

/// FileDisposer that moves the file to the system trash.
/// Returns true when the file is gone afterwards, including when it did not exist.
bool trashFile(const juce::File& file)
{
    return moveToTrash(file) != TrashResult::failed;
}

/// Returns the platform-appropriate label for the menu item that reveals a file in the file manager.
juce::String revealInFileManagerLabel()
{
#if JUCE_MAC
    return "Reveal in Finder";
#elif JUCE_WINDOWS
    return "Reveal in Explorer";
#else
    return "Reveal in File Manager";
#endif
}

/// Reveals the file in the platform file manager, or its parent folder when the file no longer exists.
void revealFileInFileManager(const juce::File& file)
{
    if (file.exists()) {
        file.revealToUser();
    } else {
        file.getParentDirectory().revealToUser();
    }
}

/// Opens the file's parent folder in the platform file manager.
void revealParentDirectory(const juce::File& file)
{
    if (const auto parentDirectory = file.getParentDirectory(); parentDirectory.isDirectory()) {
        parentDirectory.revealToUser();
    }
}

/// Deletes a file and logs an error if the deletion fails.
/// Returns true if the file did not exist or was deleted successfully.
bool deleteFile(const juce::File& file)
{
    if (!file.exists()) {
        return true;
    }

    if (!file.deleteFile()) {
        logError("Failed to delete file: {}", file.getFullPathName().quoted());
        return false;
    }

    return true;
}

/// Returns the extension text trimmed, lowercased, and without the leading dot, for example "mp3".
juce::String normalizeExtension(const juce::String& extension)
{
    auto value = extension.trim().toLowerCase();

    if (value.startsWithChar('.')) {
        value = value.substring(1);
    }

    return value;
}

/// Returns the file's extension lowercased and without the leading dot, for example "mp3".
juce::String fileExtension(const juce::File& file)
{
    return normalizeExtension(file.getFileExtension());
}

/// Returns true for a file that analysis skips as hidden: one with the hidden attribute or a name starting with a dot.
/// Dot-prefixed files count on every platform, because juce::File::isHidden only checks the attribute on Windows.
/// This includes AppleDouble "._" files, which hold macOS metadata on non-Apple file systems and are never audio.
bool isHiddenFile(const juce::File& file)
{
    return file.isHidden() || file.getFileName().startsWithChar('.');
}

/// Returns true for an MP3 file.
bool isMp3File(const juce::File& file)
{
    return fileExtension(file) == "mp3";
}

/// Returns true for an AIFF family file: .aif, .aiff, or .aifc.
bool isAiffFile(const juce::File& file)
{
    const auto extension = fileExtension(file);
    return extension == "aif" || extension == "aiff" || extension == "aifc";
}

/// Returns true for a WAV file: .wav or .wave.
bool isWavFile(const juce::File& file)
{
    const auto extension = fileExtension(file);
    return extension == "wav" || extension == "wave";
}

/// Returns the user-facing error for an output path that is already taken by a file other than the source.
juce::String existingOutputMessage(const juce::File& outputFile)
{
    return format("Output file {} already exists. Use force overwrite to replace it.", outputFile.getFileName());
}

/// Disposes of a copy of the file under the same name, leaving the file itself in place.
/// Used before an in-place rewrite that cannot be repeated to get back to the original,
/// so the original stays recoverable from the trash.
/// The copy is made in a new folder in the system temp directory, which is removed afterwards.
/// Each call gets its own uniquely named folder, because workers back up files concurrently,
/// and a shared folder name would let one worker delete the copy of another before it reached the trash.
/// Returns true when the copy was made and disposed of.
bool disposeOfCopy(const juce::File& file, const FileDisposer& disposeFile)
{
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("AudioBatch")
                               .getChildFile(format("original-{}", juce::Uuid().toDashedString()));

    if (directory.exists() || !directory.createDirectory()) {
        return false;
    }

    const auto copy = directory.getChildFile(file.getFileName());
    // The disposer reports a missing file as gone, so the copy is checked first,
    // otherwise a lost copy would count as a recoverable original.
    const bool disposed = file.copyFileTo(copy) && copy.existsAsFile() && disposeFile(copy);

    if (!directory.deleteRecursively()) {
        logWarn("Could not remove the temporary folder {}", directory.getFullPathName().quoted());
    }

    return disposed;
}

/// Checks that the output path can take the output of the given source,
/// and returns the user-facing error when it cannot.
/// A folder at the output path is never replaced, even with overwriteExisting,
/// and a different file there needs overwriteExisting.
/// The services check this before rendering, so a doomed file fails before any work is done.
std::expected<void, juce::String> checkOutputPath(
    const juce::File& sourceFile,
    const juce::File& outputFile,
    const bool overwriteExisting
)
{
    if (outputFile == sourceFile || !outputFile.exists()) {
        return {};
    }

    if (!outputFile.existsAsFile()) {
        return std::unexpected(
            format("Output path {} is not a file and cannot be replaced.", outputFile.getFileName())
        );
    }

    if (!overwriteExisting) {
        return std::unexpected(existingOutputMessage(outputFile));
    }

    return {};
}

/// Publishes audio without replacing a concurrent destination and rolls back only owned directory entries.
/// Native identity checks narrow stale-source races, but cannot make path based disposal an atomic compare-and-swap.
std::expected<void, juce::String> commitTemporaryOutput(
    const OutputTemporaryFile& temporaryFile,
    const juce::File& sourceFile,
    const juce::File& outputFile,
    const bool overwriteExisting,
    const FileIdentity& expectedSource,
    const FileDisposer& disposeFile,
    const OutputCommitHook& hook
)
{
    const auto notify = [&hook](const juce::String& stage) {
        if (hook) {
            hook(stage);
        }
    };
    if (!expectedSource.matches(sourceFile)) {
        return sourceChangedFailure();
    }
    if (auto check = checkOutputPath(sourceFile, outputFile, overwriteExisting); !check) {
        return check;
    }
    const bool replacingSource = outputFile == sourceFile;
    OutputBackup backup;
    if (!replacingSource && outputFile.exists()) {
        auto claimed = claimOutputBackup(outputFile, hook);
        if (!claimed) {
            return std::unexpected(claimed.error());
        }
        backup = std::move(*claimed);
    }
    notify("before_publish");
    if (!expectedSource.matches(sourceFile)) {
        restoreOutputBackup(backup.file, backup.identity, outputFile);
        if (backup.identity && !backup.identity->owns(outputFile)) {
            return std::unexpected(keepNewAudioAfterLostOriginal(temporaryFile, outputFile));
        }
        return sourceChangedFailure();
    }
    const FileIdentity renderedIdentity(temporaryFile.getFile());
    const bool published = replacingSource ? temporaryFile.getFile().replaceFileIn(outputFile)
                                           : linkWithoutReplacing(temporaryFile.getFile(), outputFile);
    if (!published) {
        restoreOutputBackup(backup.file, backup.identity, outputFile);
        if (temporaryFile.getFile().existsAsFile()) {
            return std::unexpected(keepNewAudioAfterLostOriginal(temporaryFile, outputFile));
        }
        return std::unexpected(juce::String("Could not move the new audio file into place"));
    }
    if (!replacingSource) {
        notify("before_dispose");
        const bool sourceMatches = expectedSource.matches(sourceFile);
        if (!sourceMatches || !renderedIdentity.owns(outputFile) || !disposeFile(sourceFile)) {
            return rollbackPublishedOutput(temporaryFile, outputFile, renderedIdentity, backup, sourceMatches, hook);
        }
    }
    if (!replacingSource && !renderedIdentity.owns(outputFile)) {
        return std::unexpected(keepNewAudioAfterLostOriginal(temporaryFile, outputFile));
    }
    if (backup.identity && backup.identity->owns(backup.file) && !disposeFile(backup.file)) {
        logWarn("Could not dispose of the previous output, it was kept as {}", backup.file.getFullPathName().quoted());
    }
    if (renderedIdentity.owns(temporaryFile.getFile())) {
        static_cast<void>(deleteFile(temporaryFile.getFile()));
    }
    return {};
}

/// Chooses a browse location, preferring the existing Dropbox directory, then Music, then the home directory.
juce::File getDefaultBrowseDirectory()
{
    auto homeDirectory = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
#if JUCE_WINDOWS
    juce::File dropboxDirectory("D:\\Dropbox");
#else
    auto dropboxDirectory = homeDirectory.getChildFile("Dropbox");
#endif
    if (dropboxDirectory.isDirectory()) {
        return dropboxDirectory;
    }
    if (auto musicDirectory = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
        musicDirectory.isDirectory())
    {
        return musicDirectory;
    }
    return homeDirectory;
}
}  // namespace audiobatch::utils
