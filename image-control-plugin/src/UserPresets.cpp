/// Implements strict portable sound-preset XML, optimistic selection checks, and serialized filesystem operations.
/// Writes publish a fully flushed sibling temporary file without deleting an existing preset first.
/// UUID filenames keep renames atomic, while a fresh revision prevents stale selections after identical updates.

#include "UserPresets.h"

#include "ProcessingState.h"
#include <system_error>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <utility>
#include <vector>

#if JUCE_WINDOWS
#include <windows.h>
#else
#include <sys/file.h>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace esgrove::image_control
{
namespace
{
constexpr auto PRESET_TAG {"ImageControlPreset"};
constexpr auto PLUGIN_ID {"com.esgrove.imagecontrol"};
constexpr auto FORMAT_VERSION {"1"};
constexpr auto FILE_EXTENSION {".xml"};
constexpr int PRESET_ATTRIBUTE_COUNT {5};
constexpr int UUID_HEX_CHARACTER_COUNT {32};
constexpr juce::juce_wchar C0_CONTROL_CHARACTER_END {31};
constexpr juce::juce_wchar DELETE_CHARACTER {127};
constexpr juce::juce_wchar C1_CONTROL_CHARACTER_END {159};
constexpr int WINDOWS_DEVICE_NAME_LENGTH {4};
constexpr int WINDOWS_DEVICE_NUMBER_INDEX {WINDOWS_DEVICE_NAME_LENGTH - 1};
constexpr std::size_t MAX_NAME_BYTES {120};
constexpr int MAX_PRESET_BYTES {64 * 1024};
constexpr int LOCK_TIMEOUT_MILLISECONDS {5000};
#if JUCE_MAC
constexpr int LOCK_RETRY_MILLISECONDS {10};
constexpr unsigned int LOCK_FILE_PERMISSIONS {0600};
#endif

/// Provides one in-process gate because native process locks need not exclude threads in the same process.
juce::CriticalSection& processMutex()
{
    static juce::CriticalSection mutex;
    return mutex;
}

/// Serializes all ImageControl preset operations, including libraries opened through different path aliases.
/// The coarse global name deliberately avoids path-hash collisions and cross-platform path canonicalization ambiguity.
class PresetLock final
{
public:
    /// Takes the thread gate before the bounded cross-process lock.
    PresetLock() : threadLock(processMutex())
    {
#if JUCE_WINDOWS
        locked = processLock.enter(LOCK_TIMEOUT_MILLISECONDS);
#else
        // flock is tied to the open description, so separate AU/VST3 images in one host exclude each other too.
        const auto lockFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile("esgrove.ImageControl.UserPresets-v1.lock");
        descriptor = ::open(
            lockFile.getFullPathName().toRawUTF8(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, LOCK_FILE_PERMISSIONS
        );
        if (descriptor < 0) {
            return;
        }
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + LOCK_TIMEOUT_MILLISECONDS;
        do {
            if (::flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
                locked = true;
                return;
            }
            if (errno != EWOULDBLOCK && errno != EINTR) {
                return;
            }
            juce::Thread::sleep(LOCK_RETRY_MILLISECONDS);
        } while (juce::Time::getMillisecondCounterHiRes() < deadline);
#endif
    }

    /// Releases the process lock before allowing another local caller to enter.
    ~PresetLock()
    {
#if JUCE_WINDOWS
        if (locked) {
            processLock.exit();
        }
#else
        if (descriptor >= 0) {
            ::close(descriptor);
        }
#endif
    }

    PresetLock(const PresetLock&) = delete;
    PresetLock& operator=(const PresetLock&) = delete;
    PresetLock(PresetLock&&) = delete;
    PresetLock& operator=(PresetLock&&) = delete;

    /// Reports timeout or native lock creation failure without continuing unprotected.
    [[nodiscard]] bool isLocked() const
    {
        return locked;
    }

private:
    /// Serializes callers within this loaded plugin image before acquiring the operating-system lock.
    juce::ScopedLock threadLock;
#if JUCE_WINDOWS
    /// Named kernel mutex shared by ImageControl instances and host processes.
    juce::InterProcessLock processLock {"com.esgrove.imagecontrol.UserPresets.v1"};
#else
    /// Owned lock-file descriptor, or -1 when opening failed.
    int descriptor {-1};
#endif
    /// Whether the operating-system lock was acquired successfully.
    bool locked {false};
};

/// Preserves Unicode filesystem paths on both supported platforms.
std::filesystem::path nativePath(const juce::File& file)
{
#if JUCE_WINDOWS
    return {file.getFullPathName().toWideCharPointer()};
#else
    return {file.getFullPathName().toStdString()};
#endif
}

/// Converts an enumerated native path without a locale-dependent Windows narrow-string conversion.
juce::File juceFile(const std::filesystem::path& path)
{
#if JUCE_WINDOWS
    return {juce::String(path.c_str())};
#else
    return {juce::String::fromUTF8(path.c_str())};
#endif
}

/// Validates the configured directory, optionally creating it only for explicit writes.
juce::Result checkDirectory(const juce::File& directory, const bool create)
{
    if (directory == juce::File() || directory.isSymbolicLink()) {
        return juce::Result::fail("The preset directory is invalid or is a symbolic link.");
    }
    if (!directory.exists()) {
        return create ? directory.createDirectory() : juce::Result::ok();
    }
    if (!directory.isDirectory() || !directory.hasReadAccess()) {
        return juce::Result::fail("The preset directory is not a readable directory.");
    }
    return juce::Result::ok();
}

/// Enumerates direct children and retains native errors rather than treating inaccessible storage as an empty library.
std::expected<std::vector<juce::File>, juce::String> children(const juce::File& directory)
{
    if (const auto result = checkDirectory(directory, false); result.failed()) {
        return std::unexpected(result.getErrorMessage());
    }
    std::vector<juce::File> files;
    if (!directory.exists()) {
        return files;
    }
    std::error_code error;
    std::filesystem::directory_iterator iterator(nativePath(directory), error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        files.push_back(juceFile(iterator->path()));
        iterator.increment(error);
    }
    if (error) {
        return std::unexpected(juce::String("Cannot read the preset directory: ") + juce::String(error.message()));
    }
    return files;
}

/// Accepts only canonical nonzero UUID identities, avoiding permissive UUID parsing of arbitrary text.
bool validIdentity(const juce::String& identity)
{
    return identity.length() == UUID_HEX_CHARACTER_COUNT && identity.containsOnly("0123456789abcdef")
        && !juce::Uuid(identity).isNull();
}

/// Reads a bounded regular UTF-8 file into an exact-byte fingerprint and a validated detached sound snapshot.
std::expected<UserPreset, juce::String> readPreset(
    const juce::File& file,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(nativePath(file), error);
    if (error || !std::filesystem::is_regular_file(status)) {
        return std::unexpected("The preset is missing, unreadable, or not a regular file.");
    }
    juce::FileInputStream stream(file);
    if (!stream.openedOk()) {
        return std::unexpected(stream.getStatus().getErrorMessage());
    }
    const auto length = stream.getTotalLength();
    if (length <= 0 || length > MAX_PRESET_BYTES) {
        return std::unexpected("The preset file is empty or exceeds the size limit.");
    }
    juce::MemoryBlock bytes;
    stream.readIntoMemoryBlock(bytes, MAX_PRESET_BYTES + 1);
    if (stream.getStatus().failed() || bytes.getSize() != static_cast<std::size_t>(length)) {
        return std::unexpected("The preset could not be read completely or changed during reading.");
    }
    const auto* data = static_cast<const char*>(bytes.getData());
    if (std::memchr(data, 0, bytes.getSize()) != nullptr
        || !juce::CharPointer_UTF8::isValidString(data, static_cast<int>(bytes.getSize())))
    {
        return std::unexpected("The preset is not valid UTF-8 XML.");
    }
    const auto text = juce::String::fromUTF8(data, static_cast<int>(bytes.getSize())).trim();
    const juce::String closingTag("</ImageControlPreset>");
    // JUCE's parser accepts trailing document content. Require a single complete root and disallow DTD/entity markup.
    // A sole optional UTF-8 XML declaration is permitted, but processing instructions and nested declarations are not.
    const auto body = text.startsWith("<?xml ") ? text.fromFirstOccurrenceOf("?>", false, false).trimStart() : text;
    const juce::String openingTag("<ImageControlPreset");
    if (text.contains("<!") || body.contains("<?") || !body.startsWith(openingTag)
        || !juce::CharacterFunctions::isWhitespace(body[openingTag.length()]) || !text.endsWith(closingTag)
        || text.indexOf(closingTag) != text.lastIndexOf(closingTag))
    {
        return std::unexpected("The preset contains unsupported XML markup or trailing content.");
    }
    const auto xml = juce::parseXML(text);
    if (!xml || !xml->hasTagName(PRESET_TAG) || xml->getNumAttributes() != PRESET_ATTRIBUTE_COUNT
        || xml->getStringAttribute("plugin") != PLUGIN_ID || xml->getStringAttribute("version") != FORMAT_VERSION
        || !validIdentity(xml->getStringAttribute("id")) || !validIdentity(xml->getStringAttribute("revision")))
    {
        return std::unexpected("The file is not a supported ImageControl sound preset.");
    }
    const auto name = xml->getStringAttribute("name");
    if (const auto result = UserPresets::validateName(name); result.failed()) {
        return std::unexpected(result.getErrorMessage());
    }
    if (xml->getNumChildElements() != 1 || !xml->getChildElement(0)->hasTagName("SoundState")) {
        return std::unexpected("The preset must contain only its sound state.");
    }
    const auto snapshot = processingStateFromXml(*xml->getChildElement(0), parameters);
    if (!snapshot) {
        return std::unexpected("The preset has invalid, unknown, or unsupported sound parameters.");
    }
    return UserPreset {
        .file = file,
        .fingerprint = bytes.toBase64Encoding(),
        .id = xml->getStringAttribute("id"),
        .name = name,
        .snapshot = *snapshot,
    };
}

/// Revalidates file containment, identity, and exact bytes before loading or mutating a selected library entry.
std::expected<UserPreset, juce::String> selectedPreset(
    const juce::File& directory,
    const UserPreset& selection,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    if (const auto result = checkDirectory(directory, false); result.failed()) {
        return std::unexpected(result.getErrorMessage());
    }
    if (selection.file.getParentDirectory() != directory || !selection.file.hasFileExtension(FILE_EXTENSION)
        || selection.fingerprint.isEmpty())
    {
        return std::unexpected("The selected preset does not belong to this library.");
    }
    auto current = readPreset(selection.file, parameters);
    if (!current) {
        return std::unexpected(current.error());
    }
    if (current->id != selection.id || current->fingerprint != selection.fingerprint || current->name != selection.name)
    {
        return std::unexpected("The selected preset changed. Refresh the library and select it again.");
    }
    return current;
}

/// Checks names against valid metadata and all sibling basenames, including malformed files and directories.
/// Corrupt entries cannot silently be overwritten by choosing their filename as a new preset name.
juce::Result checkCollision(
    const juce::File& directory,
    const juce::String& name,
    const juce::File& excluded,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    const auto files = children(directory);
    if (!files) {
        return juce::Result::fail(files.error());
    }
    for (const auto& file : *files) {
        if (file == excluded) {
            continue;
        }
        if (file.getFileName().equalsIgnoreCase(name) || file.getFileNameWithoutExtension().equalsIgnoreCase(name)) {
            return juce::Result::fail("A preset or file with this name already exists.");
        }
        if (file.hasFileExtension(FILE_EXTENSION)) {
            if (const auto preset = readPreset(file, parameters); preset && preset->name.equalsIgnoreCase(name)) {
                return juce::Result::fail("A preset with this name already exists.");
            }
        }
    }
    return juce::Result::ok();
}

/// Produces one portable sound-only document with an independent storage revision for optimistic updates.
std::expected<juce::String, juce::String> serializePreset(
    const juce::String& presetId,
    const juce::String& name,
    const ProcessingSnapshot& snapshot,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    auto sound = processingStateToXml(snapshot);
    if (!sound) {
        return std::unexpected("The sound snapshot cannot be serialized.");
    }
    const auto validated = processingStateFromXml(*sound, parameters);
    if (!validated || validated->size() != snapshot.size() || !sameProcessingState(*validated, snapshot)) {
        return std::unexpected("The sound snapshot does not match the plugin's sound parameter schema.");
    }
    juce::XmlElement xml(PRESET_TAG);
    xml.setAttribute("plugin", PLUGIN_ID);
    xml.setAttribute("version", FORMAT_VERSION);
    xml.setAttribute("id", presetId);
    xml.setAttribute("revision", juce::Uuid().toString());
    xml.setAttribute("name", name);
    xml.addChildElement(sound.release());
    auto text = xml.toString();
    if (text.getNumBytesAsUTF8() > MAX_PRESET_BYTES) {
        return std::unexpected("The sound preset exceeds the size limit.");
    }
    return text;
}

/// Flushes a sibling temporary file, then atomically replaces an existing file or publishes a new one.
/// Failure never deliberately deletes the old destination. Existing export destinations are always refused.
juce::Result writePreset(const juce::File& destination, const juce::String& text, const bool replace)
{
    if (destination.isSymbolicLink() || destination.isDirectory()) {
        return juce::Result::fail("The destination is a directory or a symbolic link.");
    }
    if (!replace && destination.exists()) {
        return juce::Result::fail("The destination already exists. Choose a new file.");
    }
    if (replace && !destination.existsAsFile()) {
        return juce::Result::fail("The selected preset is missing. Refresh the library.");
    }
    const auto temporaryFile = destination.getSiblingFile(".imagecontrol-" + juce::Uuid().toString() + ".tmp");
    const juce::TemporaryFile temporary(destination, temporaryFile);
    {
        juce::FileOutputStream stream(temporaryFile);
        if (!stream.openedOk()) {
            return juce::Result::fail(
                "Cannot create the temporary preset file: " + stream.getStatus().getErrorMessage()
            );
        }
        const auto data = text.toUTF8();
        if (!stream.write(data.getAddress(), text.getNumBytesAsUTF8())) {
            return juce::Result::fail(
                "Cannot write the temporary preset file: " + stream.getStatus().getErrorMessage()
            );
        }
        stream.flush();
        if (stream.getStatus().failed()) {
            return juce::Result::fail(
                "Cannot flush the temporary preset file: " + stream.getStatus().getErrorMessage()
            );
        }
    }
    if (!replace && destination.exists()) {
        return juce::Result::fail("The destination appeared while saving. No file was replaced.");
    }
    // JUCE's macOS replacement helper can fall back to a truncating copy after rename fails.
    // Use only the native atomic publication primitive, with no copy fallback and no pre-delete.
#if JUCE_WINDOWS
    const auto published = replace
        ? ReplaceFileW(
              destination.getFullPathName().toWideCharPointer(),
              temporaryFile.getFullPathName().toWideCharPointer(),
              nullptr,
              0,
              nullptr,
              nullptr
          )
        : MoveFileExW(
              temporaryFile.getFullPathName().toWideCharPointer(), destination.getFullPathName().toWideCharPointer(), 0
          );
    const auto publishError = published != 0
        ? std::error_code {}
        : std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
    const auto published = replace
        ? ::rename(temporaryFile.getFullPathName().toRawUTF8(), destination.getFullPathName().toRawUTF8())
        : ::renamex_np(
              temporaryFile.getFullPathName().toRawUTF8(), destination.getFullPathName().toRawUTF8(), RENAME_EXCL
          );
    const auto publishError = published == 0 ? std::error_code {} : std::error_code(errno, std::generic_category());
#endif
    if (publishError) {
        return juce::Result::fail(
            juce::String("Cannot publish the preset file: ") + juce::String(publishError.message())
        );
    }
    return juce::Result::ok();
}

/// Saves a new independent identity after name/schema validation and collision checks under the caller's lock.
std::expected<UserPreset, juce::String> saveNew(
    const juce::File& directory,
    const juce::String& name,
    const ProcessingSnapshot& snapshot,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    if (const auto result = UserPresets::validateName(name); result.failed()) {
        return std::unexpected(result.getErrorMessage());
    }
    const auto presetId = juce::Uuid().toString();
    const auto text = serializePreset(presetId, name, snapshot, parameters);
    if (!text) {
        return std::unexpected(text.error());
    }
    if (const auto result = checkCollision(directory, name, {}, parameters); result.failed()) {
        return std::unexpected(result.getErrorMessage());
    }
    if (const auto result = checkDirectory(directory, true); result.failed()) {
        return std::unexpected(result.getErrorMessage());
    }
    const auto file = directory.getChildFile(presetId + FILE_EXTENSION);
    if (const auto result = writePreset(file, *text, false); result.failed()) {
        return std::unexpected(result.getErrorMessage());
    }
    return readPreset(file, parameters);
}
}  // namespace

/// Chooses native user storage without enumerating or creating it.
juce::File UserPresets::defaultDirectory()
{
#if JUCE_MAC
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Library/Application Support/esgrove/ImageControl/Presets");
#else
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("esgrove/ImageControl/Presets");
#endif
}

/// Retains only the immutable validation schema and the default path, with no constructor filesystem work.
UserPresets::UserPresets(const juce::AudioProcessorValueTreeState& parameterState) :
    UserPresets(parameterState, defaultDirectory())
{}

/// Allows isolated tests and caller-selected storage without changing process environment or plugin state.
UserPresets::UserPresets(const juce::AudioProcessorValueTreeState& parameterState, juce::File presetDirectory) :
    parameters(parameterState),
    directory(std::move(presetDirectory))
{}

/// Releases only the retained path and schema reference, without filesystem work.
UserPresets::~UserPresets() = default;

/// Rejects names that are ambiguous, unsafe, nonportable, or too long on either supported platform.
/// Names are never silently sanitized, so the caller can explain precisely why a save was refused.
juce::Result UserPresets::validateName(const juce::String& name)
{
    if (name.isEmpty() || name.trim() != name || name.startsWithChar('.') || name.endsWithChar('.')
        || name.getNumBytesAsUTF8() > MAX_NAME_BYTES)
    {
        return juce::Result::fail(
            "Use a nonempty preset name without leading/trailing spaces or dots, up to 120 UTF-8 bytes."
        );
    }
    for (const auto character : name) {
        if (character <= C0_CONTROL_CHARACTER_END
            || (character >= DELETE_CHARACTER && character <= C1_CONTROL_CHARACTER_END)
            || juce::String("<>:\"/\\|?*").containsChar(character))
        {
            return juce::Result::fail("Preset names cannot contain control characters or <>:\"/\\|?*.");
        }
    }
    const auto stem = name.upToFirstOccurrenceOf(".", false, false).trimEnd().toUpperCase();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CLOCK$" || stem == "CONIN$"
        || stem == "CONOUT$"
        || ((stem.startsWith("COM") || stem.startsWith("LPT")) && stem.length() == WINDOWS_DEVICE_NAME_LENGTH
            && juce::String::fromUTF8("123456789¹²³").containsChar(stem[WINDOWS_DEVICE_NUMBER_INDEX])))
    {
        return juce::Result::fail("This preset name is reserved on Windows.");
    }
    return juce::Result::ok();
}

/// Returns the configured path without touching the filesystem.
juce::File UserPresets::getDirectory() const
{
    return directory;
}

/// Lists validated XML files and separate rejection diagnostics, without creating a missing preset directory.
std::expected<UserPresetListing, juce::String> UserPresets::list() const
{
    const PresetLock lock;
    if (!lock.isLocked()) {
        return std::unexpected("The preset library is busy or its lock is unavailable.");
    }
    const auto files = children(directory);
    if (!files) {
        return std::unexpected(files.error());
    }
    UserPresetListing listing;
    std::vector<UserPreset> candidates;
    for (const auto& file : *files) {
        if (!file.hasFileExtension(FILE_EXTENSION)) {
            continue;
        }
        if (auto preset = readPreset(file, parameters)) {
            candidates.push_back(std::move(*preset));
        } else {
            listing.rejectedFiles.push_back({.file = file, .message = preset.error()});
        }
    }
    for (const auto& candidate : candidates) {
        const bool duplicate = std::ranges::any_of(candidates, [&](const auto& other) {
            return candidate.file != other.file
                && (candidate.name.equalsIgnoreCase(other.name) || candidate.id == other.id);
        });
        if (duplicate) {
            listing.rejectedFiles.push_back(
                {.file = candidate.file, .message = "The preset duplicates another library name or identity."}
            );
        } else {
            listing.presets.push_back(candidate);
        }
    }
    std::ranges::sort(listing.presets, [](const auto& first, const auto& second) {
        return first.name.compareIgnoreCase(second.name) < 0;
    });
    return listing;
}

/// Reloads precisely the selected revision instead of silently applying an externally changed file.
std::expected<UserPreset, juce::String> UserPresets::load(const UserPreset& selection) const
{
    const PresetLock lock;
    if (!lock.isLocked()) {
        return std::unexpected("The preset library is busy or its lock is unavailable.");
    }
    return selectedPreset(directory, selection, parameters);
}

/// Saves a caller-captured sound snapshot as a new preset, never capturing live parameter values on the worker.
std::expected<UserPreset, juce::String> UserPresets::saveAs(
    const juce::String& name,
    const ProcessingSnapshot& snapshot
) const
{
    const PresetLock lock;
    if (!lock.isLocked()) {
        return std::unexpected("The preset library is busy or its lock is unavailable.");
    }
    return saveNew(directory, name, snapshot, parameters);
}

/// Imports only the portable sound document and assigns a new local identity, refusing name collisions.
std::expected<UserPreset, juce::String> UserPresets::importPreset(
    const juce::File& source,
    const juce::String& name
) const
{
    const PresetLock lock;
    if (!lock.isLocked()) {
        return std::unexpected("The preset library is busy or its lock is unavailable.");
    }
    const auto imported = readPreset(source, parameters);
    if (!imported) {
        return std::unexpected(imported.error());
    }
    return saveNew(directory, name.isEmpty() ? imported->name : name, imported->snapshot, parameters);
}

/// Replaces the selected revision's sound while retaining its identity and name.
/// The caller confirms replacing user work before scheduling this operation.
std::expected<UserPreset, juce::String> UserPresets::update(
    const UserPreset& selection,
    const ProcessingSnapshot& snapshot
) const
{
    const PresetLock lock;
    if (!lock.isLocked()) {
        return std::unexpected("The preset library is busy or its lock is unavailable.");
    }
    const auto current = selectedPreset(directory, selection, parameters);
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto text = serializePreset(current->id, current->name, snapshot, parameters);
    if (!text) {
        return std::unexpected(text.error());
    }
    if (const auto result = writePreset(current->file, *text, true); result.failed()) {
        return std::unexpected(result.getErrorMessage());
    }
    return readPreset(current->file, parameters);
}

/// Changes only display metadata using one atomic file replacement, including case-only renames.
std::expected<UserPreset, juce::String> UserPresets::rename(const UserPreset& selection, const juce::String& name) const
{
    const PresetLock lock;
    if (!lock.isLocked()) {
        return std::unexpected("The preset library is busy or its lock is unavailable.");
    }
    if (const auto result = validateName(name); result.failed()) {
        return std::unexpected(result.getErrorMessage());
    }
    const auto current = selectedPreset(directory, selection, parameters);
    if (!current) {
        return std::unexpected(current.error());
    }
    if (const auto result = checkCollision(directory, name, current->file, parameters); result.failed()) {
        return std::unexpected(result.getErrorMessage());
    }
    const auto text = serializePreset(current->id, name, current->snapshot, parameters);
    if (!text) {
        return std::unexpected(text.error());
    }
    if (const auto result = writePreset(current->file, *text, true); result.failed()) {
        return std::unexpected(result.getErrorMessage());
    }
    return readPreset(current->file, parameters);
}

/// Exports a revalidated selection to a new file only, with no destination overwrite or implicit directory creation.
juce::Result UserPresets::exportPreset(const UserPreset& selection, const juce::File& destination) const
{
    const PresetLock lock;
    if (!lock.isLocked()) {
        return juce::Result::fail("The preset library is busy or its lock is unavailable.");
    }
    const auto current = selectedPreset(directory, selection, parameters);
    if (!current) {
        return juce::Result::fail(current.error());
    }
    if (destination.getParentDirectory() == directory || destination == juce::File()) {
        return juce::Result::fail("Export outside the preset library to a new file.");
    }
    if (const auto result = validateName(destination.getFileName()); result.failed()) {
        return result;
    }
    if (!destination.getParentDirectory().isDirectory()) {
        return juce::Result::fail("The export destination directory does not exist.");
    }
    if (const auto result
        = checkCollision(destination.getParentDirectory(), destination.getFileNameWithoutExtension(), {}, parameters);
        result.failed())
    {
        return result;
    }
    juce::MemoryBlock bytes;
    if (!bytes.fromBase64Encoding(current->fingerprint)) {
        return juce::Result::fail("The selected preset fingerprint is invalid.");
    }
    return writePreset(destination, bytes.toString(), false);
}

/// Deletes only the exact selected revision. The caller must confirm deletion before invoking this operation.
juce::Result UserPresets::remove(const UserPreset& selection) const
{
    const PresetLock lock;
    if (!lock.isLocked()) {
        return juce::Result::fail("The preset library is busy or its lock is unavailable.");
    }
    const auto current = selectedPreset(directory, selection, parameters);
    if (!current) {
        return juce::Result::fail(current.error());
    }
    return current->file.deleteFile() ? juce::Result::ok() : juce::Result::fail("Cannot delete the selected preset.");
}
}  // namespace esgrove::image_control
