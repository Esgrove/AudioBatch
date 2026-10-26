/// Implementation of file system helpers and default browse-folder selection.
/// Moving to the trash uses the shell's IFileOperation on Windows,
/// with a progress sink that refuses deletes the shell would perform permanently,
/// and juce::File::moveToTrash elsewhere.

#include "utils/FileUtils.h"

#include "utils/Log.h"
#include "utils/StringFormat.h"

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
#if JUCE_WINDOWS
namespace
{
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

}  // namespace

template<typename Interface>
using ComPointer = std::unique_ptr<Interface, ComReleaser>;

/// Moves the file or folder to the recycle bin through IFileOperation,
/// refusing to delete it when the drive has no recycle bin.
/// IFileOperation requires a single-threaded apartment,
/// so the calling thread is initialized for one when it has no apartment yet,
/// and the move fails on a thread that already joined the multithreaded apartment.
static bool moveToRecycleBin(const juce::File& file)
{
    const auto apartmentResult = CoInitializeEx(
        nullptr, static_cast<DWORD>(COINIT_APARTMENTTHREADED) | static_cast<DWORD>(COINIT_DISABLE_OLE1DDE)
    );

    if (FAILED(apartmentResult)) {
        logError("Could not initialize COM to move {} to the recycle bin", file.getFullPathName().quoted());
        return false;
    }

    const auto recycled = [&file] {
        IFileOperation* rawOperation = nullptr;
        constexpr DWORD classContexts = static_cast<DWORD>(CLSCTX_INPROC_SERVER)
            | static_cast<DWORD>(CLSCTX_INPROC_HANDLER) | static_cast<DWORD>(CLSCTX_LOCAL_SERVER)
            | static_cast<DWORD>(CLSCTX_REMOTE_SERVER);
        if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, classContexts, IID_PPV_ARGS(&rawOperation)))) {
            return false;
        }
        const ComPointer<IFileOperation> operation(rawOperation);

        IShellItem* rawItem = nullptr;
        const auto path = file.getFullPathName().replaceCharacter('/', '\\');
        if (FAILED(SHCreateItemFromParsingName(path.toWideCharPointer(), nullptr, IID_PPV_ARGS(&rawItem)))) {
            return false;
        }
        const ComPointer<IShellItem> item(rawItem);

        RecycleOnlyProgressSink progressSink;
        DWORD adviseCookie = 0;

        if (FAILED(operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMATION))
            || FAILED(operation->Advise(&progressSink, &adviseCookie)))
        {
            return false;
        }

        const auto deleted
            = SUCCEEDED(operation->DeleteItem(item.get(), nullptr)) && SUCCEEDED(operation->PerformOperations());
        BOOL anyAborted = FALSE;
        const auto abortQueried = SUCCEEDED(operation->GetAnyOperationsAborted(&anyAborted));
        operation->Unadvise(adviseCookie);

        if (progressSink.refusedPermanentDelete) {
            logError(
                "Did not move {} to the recycle bin, because its drive would delete it permanently",
                file.getFullPathName().quoted()
            );
        }

        return deleted && abortQueried && anyAborted == FALSE && !progressSink.refusedPermanentDelete;
    }();

    CoUninitialize();
    return recycled && !file.exists();
}
#endif

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

/// Renames the backup of a replaced output back to the output path after a failed commit.
/// Logs where the previous output is when the rename fails, so it can still be recovered by hand.
static void restoreOutputBackup(const juce::File& backupFile, const juce::File& outputFile)
{
    if (backupFile == juce::File() || !backupFile.existsAsFile()) {
        return;
    }

    if (!backupFile.moveFileTo(outputFile)) {
        logError(
            "Could not restore the previous output {} from its backup {}",
            outputFile.getFullPathName().quoted(),
            backupFile.getFullPathName().quoted()
        );
    }
}

/// Moves the new audio out of the temporary file after an in-place replace lost the original,
/// so the temporary file's destructor cannot delete the only remaining copy of the audio.
/// On Windows, ReplaceFile can delete the original and then fail to move the replacement into place.
/// Returns the user-facing error describing where the new audio was kept.
static juce::String keepNewAudioAfterLostOriginal(
    const juce::TemporaryFile& temporaryFile,
    const juce::File& outputFile
)
{
    const auto keptFile
        = outputFile
              .getSiblingFile(outputFile.getFileNameWithoutExtension() + " (recovered)" + outputFile.getFileExtension())
              .getNonexistentSibling(false);

    if (temporaryFile.getFile().moveFileTo(keptFile) || temporaryFile.getFile().copyFileTo(keptFile)) {
        logError(
            "The original file {} was removed while being replaced, the new audio was kept as {}",
            outputFile.getFullPathName().quoted(),
            keptFile.getFullPathName().quoted()
        );
        return format("Could not replace the original file. The new audio was kept as {}", keptFile.getFileName());
    }

    logError("The original file {} was removed while being replaced", outputFile.getFullPathName().quoted());
    return "Could not replace the original file, and the new audio could not be kept";
}

/// Moves a finished temporary output into place next to its source and disposes of the source.
/// When the output path is the source itself, the source is replaced in place.
/// Otherwise an output path that fails checkOutputPath fails the commit,
/// and a file allowed to be overwritten is renamed to a hidden backup next to it first.
/// After the output is in place, a source with a different path is disposed of,
/// and when that fails the output is removed again and the backup is renamed back,
/// so the folder is left as it was.
/// The backup is disposed of last, once everything else succeeded.
/// When an in-place replace fails after the original was already removed,
/// the new audio is kept next to it under a "(recovered)" name instead of being deleted with the temporary file.
/// `disposeFile` moves files to the system trash unless a caller such as a test passes another function.
/// Returns a user-facing error message on failure.
/// The temporary file is deleted by its own destructor on failure.
std::expected<void, juce::String> commitTemporaryOutput(
    const juce::TemporaryFile& temporaryFile,
    const juce::File& sourceFile,
    const juce::File& outputFile,
    const bool overwriteExisting,
    const FileDisposer& disposeFile
)
{
    const bool replacingSource = outputFile == sourceFile;
    juce::File backupFile;

    if (auto outputCheck = checkOutputPath(sourceFile, outputFile, overwriteExisting); !outputCheck) {
        return outputCheck;
    }

    if (!replacingSource && outputFile.exists()) {
        // A rename within the folder is atomic and can be undone,
        // unlike moving the previous output to the trash before the new one is in place.
        // The dot prefix keeps the backup out of folder scans.
        backupFile = outputFile.getSiblingFile("." + outputFile.getFileName() + ".backup").getNonexistentSibling(false);

        if (!outputFile.moveFileTo(backupFile)) {
            return std::unexpected(juce::String("Could not move the existing output file aside"));
        }
    }

    if (!temporaryFile.overwriteTargetFileWithTemporary()) {
        restoreOutputBackup(backupFile, outputFile);

        if (replacingSource && !sourceFile.exists() && temporaryFile.getFile().existsAsFile()) {
            return std::unexpected(keepNewAudioAfterLostOriginal(temporaryFile, outputFile));
        }
        return std::unexpected(
            juce::String(
                replacingSource ? "Could not replace the original file with the new audio"
                                : "Could not move the new audio file into place"
            )
        );
    }

    if (!replacingSource && sourceFile.existsAsFile() && !disposeFile(sourceFile)) {
        // A failed delete is logged, and the backup below is restored either way.
        static_cast<void>(deleteFile(outputFile));
        restoreOutputBackup(backupFile, outputFile);
        return std::unexpected(juce::String("Could not move the original audio file to the system trash"));
    }

    if (backupFile.existsAsFile() && !disposeFile(backupFile)) {
        // The new output and the source are already settled, so only the leftover backup is reported.
        logWarn(
            "Could not move the replaced output to the system trash, it was kept as {}",
            backupFile.getFullPathName().quoted()
        );
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
