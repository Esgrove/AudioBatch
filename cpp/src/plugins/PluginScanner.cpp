/// Registers plugin formats, persists the known-plugins list, and serializes scans between processes.
/// Built-in Apple Audio Units are excluded from both scans and previously saved lists.

#include "plugins/PluginScanner.h"

#ifndef AUDIOBATCH_TESTING
#include "utils/AppInfo.h"
#endif
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <memory>

namespace audiobatch
{
namespace
{
#if JUCE_PLUGINHOST_AU && JUCE_MAC
/// Audio Unit format that leaves the Audio Units built into macOS out of plugin scans.
/// Only the list of plugins to scan is filtered, so every other Audio Unit scans and loads as usual.
class AudioUnitFormatWithoutBuiltInPlugins final : public juce::AudioUnitPluginFormat
{
public:
    /// Lists the registered Audio Units except the ones built into macOS.
    juce::StringArray searchPathsForPlugins(
        const juce::FileSearchPath& directoriesToSearch,
        const bool recursive,
        const bool allowPluginsWhichRequireAsynchronousInstantiation
    ) override
    {
        juce::StringArray identifiers;

        for (const auto& identifier : AudioUnitPluginFormat::searchPathsForPlugins(
                 directoriesToSearch, recursive, allowPluginsWhichRequireAsynchronousInstantiation
             ))
        {
            if (!isBuiltInApplePlugin(identifier)) {
                identifiers.add(identifier);
            }
        }

        return identifiers;
    }
};
#endif

/// Environment variable with extra folders to search for VST3 plugins.
constexpr auto vst3PathVariable {"VST3_PATH"};

#if JUCE_PLUGINHOST_VST3
/// VST3 format whose default search path splits VST3_PATH with the separator of the platform's path lists.
/// JUCE replaces every ":" in the variable with ";", which splits a Windows path such as C:\Plugins in two.
class VST3FormatWithPlatformSearchPath final : public juce::VST3PluginFormat
{
public:
    /// Returns the folders of VST3_PATH, then the standard VST3 folders of the platform.
    juce::FileSearchPath getDefaultLocationsToSearch() override
    {
        return buildVst3SearchPath(
            VST3PluginFormat::getDefaultLocationsToSearch(),
            juce::SystemStats::getEnvironmentVariable(vst3PathVariable, {}),
            juce::File::getSeparatorChar() == '\\' ? ";" : ":"
        );
    }
};
#endif

/// Name of the known-plugins list file next to the settings file.
constexpr auto knownPluginListFileName {"KnownPlugins.xml"};
/// Settings key under which older versions kept the known-plugins list.
constexpr auto legacyKnownPluginListPropertyKey {"knownPluginList"};
/// Start of the identifier of every Audio Unit.
constexpr auto audioUnitIdentifierPrefix {"AudioUnit:"};
/// Manufacturer code that ends the identifiers of the Audio Units built into macOS.
constexpr auto appleManufacturerCode {"appl"};
}  // namespace

/// Rebuilds the search path JUCE made from VST3_PATH and the standard folders,
/// with the variable split at `listSeparator`, the separator of the platform's path lists.
/// JUCE splits the variable at both ";" and ":", so the entries it made from the variable are taken out first,
/// leaving the standard folders, which then follow the variable's folders without duplicates.
/// Works on the raw strings, because an entry such as "C" is not a valid juce::File.
juce::FileSearchPath buildVst3SearchPath(
    const juce::FileSearchPath& juceSearchPath,
    const juce::String& variableValue,
    const juce::String& listSeparator
)
{
    auto juceVariableEntries = juce::StringArray::fromTokens(variableValue.replace(":", ";"), ";", "\"");
    juceVariableEntries.trim();
    juceVariableEntries.removeEmptyStrings();

    auto entries = juce::StringArray::fromTokens(variableValue, listSeparator, "\"");
    entries.trim();
    entries.removeEmptyStrings();

    for (int index = 0; index < juceSearchPath.getNumPaths(); ++index) {
        const auto entry = juceSearchPath.getRawString(index);
        if (const auto variableIndex = juceVariableEntries.indexOf(entry); variableIndex >= 0) {
            juceVariableEntries.remove(variableIndex);
            continue;
        }
        entries.add(entry);
    }

    entries.removeDuplicates(!juce::File::areFileNamesCaseSensitive());
    return {entries.joinIntoString(";")};
}

/// Returns true for an Audio Unit built into macOS, such as AUPeakLimiter or AUGraphicEQ.
/// They are recognised by the Apple manufacturer code that ends their identifier,
/// for example "AudioUnit:Effects/aufx,lmtr,appl".
bool isBuiltInApplePlugin(const juce::String& fileOrIdentifier)
{
    return fileOrIdentifier.startsWith(audioUnitIdentifierPrefix)
        && fileOrIdentifier.fromLastOccurrenceOf(",", false, false) == appleManufacturerCode;
}

/// Registers the VST3 and Audio Unit formats that CMakeLists.txt enables, in the order juce::addDefaultFormatsToManager
/// uses, with an Audio Unit format that leaves the Audio Units built into macOS out of plugin scans,
/// and a VST3 format that keeps Windows drive letters in VST3_PATH intact.
void addPluginFormats([[maybe_unused]] juce::AudioPluginFormatManager& formatManager)
{
#if JUCE_PLUGINHOST_AU && JUCE_MAC
    formatManager.addFormat(std::make_unique<AudioUnitFormatWithoutBuiltInPlugins>());
#endif

#if JUCE_PLUGINHOST_VST3
    formatManager.addFormat(std::make_unique<VST3FormatWithPlatformSearchPath>());
#endif
}

/// Removes the Audio Units built into macOS from a known-plugins list, including blacklisted ones,
/// since the scans leave them out and a list saved by an older version may still contain them.
void removeBuiltInApplePlugins(juce::KnownPluginList& knownPluginList)
{
    for (const auto& type : knownPluginList.getTypes()) {
        if (isBuiltInApplePlugin(type.fileOrIdentifier)) {
            knownPluginList.removeType(type);
        }
    }

    // A copy, since removeFromBlacklist changes the array that getBlacklistedFiles refers to.
    const auto blacklistedFiles = knownPluginList.getBlacklistedFiles();
    for (const auto& file : blacklistedFiles) {
        if (isBuiltInApplePlugin(file)) {
            knownPluginList.removeFromBlacklist(file);
        }
    }
}

/// Removes the plugins and blacklist entries whose files are gone from a search folder that still exists,
/// the same rule as in the Rust version.
/// Plugins outside the searched folders, such as Audio Units or plugins on a disconnected drive, are kept.
/// Returns the number of plugins removed.
int removeMissingPlugins(
    juce::KnownPluginList& knownPluginList,
    const juce::AudioPluginFormatManager& formatManager,
    juce::PropertiesFile& settings
)
{
    juce::Array<juce::File> existingFolders;
    for (auto* format : formatManager.getFormats()) {
        const auto searchPath = juce::PluginListComponent::getLastSearchPath(settings, *format);
        for (int index = 0; index < searchPath.getNumPaths(); ++index) {
            if (const auto folder = searchPath[index]; folder.isDirectory()) {
                existingFolders.addIfNotAlreadyThere(folder);
            }
        }
    }

    const auto isMissing = [&existingFolders](const juce::String& fileOrIdentifier) {
        if (!juce::File::isAbsolutePath(fileOrIdentifier)) {
            return false;
        }
        const juce::File file(fileOrIdentifier);
        return !file.exists() && std::ranges::any_of(existingFolders, [&file](const juce::File& folder) {
            return file.isAChildOf(folder);
        });
    };

    int removedCount = 0;
    for (const auto& type : knownPluginList.getTypes()) {
        if (isMissing(type.fileOrIdentifier)) {
            knownPluginList.removeType(type);
            ++removedCount;
        }
    }

    // A copy, since removeFromBlacklist changes the array that getBlacklistedFiles refers to.
    const auto blacklistedFiles = knownPluginList.getBlacklistedFiles();
    for (const auto& file : blacklistedFiles) {
        if (isMissing(file)) {
            knownPluginList.removeFromBlacklist(file);
        }
    }

    if (removedCount > 0) {
        utils::logInfo("Removed {} plugins that no longer exist", removedCount);
    }
    return removedCount;
}

/// Returns true when the command line launches this executable as a plugin scan worker.
/// Uses the same check as ChildProcessWorker: the worker prefix must start the command line.
bool isPluginScanWorkerCommandLine(const juce::String& commandLine)
{
    // ChildProcessCoordinator passes "--<uid>:<pipe name>" as the first argument.
    // Match ChildProcessWorker::initialiseFromCommandLine exactly,
    // so the same text in a later argument of an ordinary launch is not mistaken for a worker.
    return commandLine.trim().startsWith(utils::format("--{}:", pluginScanWorkerProcessUid));
}

/// Returns the KnownPlugins.xml file next to the given settings file.
/// The known-plugins list lives in its own file instead of the settings,
/// so the GUI and the CLI can each update it without overwriting the other one's settings.
juce::File getKnownPluginListFile(const juce::PropertiesFile& settings)
{
    return settings.getFile().getSiblingFile(knownPluginListFileName);
}

/// Restores the known-plugins list from the KnownPlugins.xml file next to the settings file.
/// When that file does not exist yet, reads the list stored in the settings by older versions instead.
/// Leaves the list unchanged when nothing has been stored.
/// Drops the Audio Units built into macOS, which older versions scanned.
void loadKnownPluginList(const juce::PropertiesFile& settings, juce::KnownPluginList& knownPluginList)
{
    if (const auto file = getKnownPluginListFile(settings); file.existsAsFile()) {
        if (const auto element = juce::parseXML(file); element != nullptr) {
            knownPluginList.recreateFromXml(*element);
            removeBuiltInApplePlugins(knownPluginList);
        } else {
            utils::logErrorAlways("Could not parse the known plugins list: {}", file.getFullPathName().quoted());
        }

        return;
    }

    // Older versions stored the list in the settings.
    const auto legacyXmlText = settings.getValue(legacyKnownPluginListPropertyKey);
    if (const auto element = juce::parseXML(legacyXmlText); element != nullptr) {
        knownPluginList.recreateFromXml(*element);
        removeBuiltInApplePlugins(knownPluginList);
    }
}

/// Writes the known-plugins list, including blacklisted plugins, to the given file.
/// The file is replaced atomically, so a concurrent reader never sees a partial list.
/// Returns false and logs an error when the file could not be written.
bool saveKnownPluginList(const juce::KnownPluginList& knownPluginList, const juce::File& file)
{
    const auto xml = knownPluginList.createXml();

    // writeTo writes a temporary file and then replaces the target,
    // so a concurrent reader never sees a partially written list.
    if (xml == nullptr || !file.getParentDirectory().createDirectory() || !xml->writeTo(file)) {
        utils::logError("Could not save the known plugins list: {}", file.getFullPathName().quoted());
        return false;
    }

    return true;
}

/// Moves a known-plugins list stored in the settings by older versions into KnownPlugins.xml,
/// and removes it from the settings.
/// Only the GUI calls this, since it owns the settings file.
void migrateKnownPluginList(juce::PropertiesFile& settings)
{
    if (!settings.containsKey(legacyKnownPluginListPropertyKey)) {
        return;
    }

    if (const auto file = getKnownPluginListFile(settings); !file.existsAsFile()) {
        juce::KnownPluginList legacyList;
        loadKnownPluginList(settings, legacyList);

        if (!saveKnownPluginList(legacyList, file)) {
            // Keep the legacy value so nothing is lost, and try again on the next start.
            return;
        }

        utils::logInfo("Moved the known plugins list to {}", file.getFullPathName().quoted());
    }

    settings.removeValue(legacyKnownPluginListPropertyKey);
    settings.saveIfNeeded();
}

/// Creates the lock without taking it.
/// Every process that uses the same name shares the lock, and tests pass their own name to stay apart from the app.
PluginScanLock::PluginScanLock(const juce::String& lockName) :
#ifdef AUDIOBATCH_TESTING
    lock(lockName == pluginScanLockName ? utils::format("{}-tests", lockName) : lockName)
#else
    lock(utils::scopedPluginScanLockName(lockName))
#endif
{}

/// Releases the lock if it is held.
PluginScanLock::~PluginScanLock()
{
    release();
}

/// Tries to take the lock without waiting.
/// Returns false when another process is scanning.
/// Taking a lock that is already held by this object succeeds.
bool PluginScanLock::tryAcquire()
{
    if (!held) {
        held = lock.enter(0);
    }

    return held;
}

/// Releases the lock if it is held.
void PluginScanLock::release()
{
    if (held) {
        lock.exit();
        held = false;
    }
}
}  // namespace audiobatch
