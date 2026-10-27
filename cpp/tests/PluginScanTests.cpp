/// Tests for the plugin scanning support in PluginScanner and the CLI plugin listing in PluginListCli.
/// Covers the scan worker command line, the inter-process scan lock, which a helper process holds,
/// the known-plugins list file with its legacy settings value and migration,
/// leaving the Audio Units built into macOS and missing plugins out of the list, and the VST3 search path.
/// The listing runs on a known-plugins list in a temporary settings folder and never scans for plugins.

#include "cli/PluginListCli.h"
#include "plugins/PluginScanner.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace audiobatch::tests
{
namespace
{
/// Settings key under which older versions stored the known-plugins list.
constexpr auto legacyKnownPluginListKey {"knownPluginList"};
/// Name of the scan lock the tests take, apart from the lock of the app, so they never contend with a running
/// AudioBatch.
constexpr auto testLockName {"AudioBatchTestsPluginScan"};

/// A custom scanner that abandons its scan on the first plugin it gets, as the plugin scan does when the lock is busy.
class AbandoningScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    /// Abandons the scan, counts the call, and reports the plugin as scanned without loading it.
    bool findPluginTypesFor(
        juce::AudioPluginFormat& /*format*/,
        juce::OwnedArray<juce::PluginDescription>& /*result*/,
        const juce::String& /*fileOrIdentifier*/
    ) override
    {
        abandonScan();
        ++calls;
        return true;
    }

    int calls {0};
};

/// A helper process that holds a plugin scan lock until it is released or destroyed.
class LockHolder
{
public:
    /// Starts the helper for the lock of the given name, with its release file in the given folder.
    LockHolder(const juce::String& lockName, const juce::File& folder) :
        releaseFile(folder.getNonexistentChildFile("release", ".flag"))
    {
        started = startHelperProcess(process, {scanLockHelperArgument, lockName, releaseFile.getFullPathName()});
    }

    /// Releases the lock and waits for the helper to exit, ending it when it does not.
    ~LockHolder()
    {
        if (!release()) {
            process.kill();
        }
    }

    LockHolder(const LockHolder&) = delete;
    LockHolder& operator=(const LockHolder&) = delete;
    LockHolder(LockHolder&&) = delete;
    LockHolder& operator=(LockHolder&&) = delete;

    /// Waits for the helper to report that it holds the lock, and returns false when it exits without it.
    [[nodiscard]] bool waitUntilHeld()
    {
        return started && readHelperLine(process) == scanLockHeldLine;
    }

    /// Creates the release file and waits for the helper to exit, returning true once it has.
    bool release()
    {
        if (!started) {
            return true;
        }
        if (!releaseFile.exists()) {
            static_cast<void>(releaseFile.create());
        }
        return process.waitForProcessToFinish(30'000);
    }

    /// Returns the exit code of the helper, which is 0 after holding the lock and 1 when the lock was busy.
    [[nodiscard]] int getExitCode() const
    {
        return static_cast<int>(process.getExitCode());
    }

private:
    juce::File releaseFile;
    juce::ChildProcess process;
    bool started {false};
};

/// Returns a description of a plugin with the given format, identifier, and unique id.
juce::PluginDescription makeDescription(const juce::String& format, const juce::String& identifier, const int uniqueId)
{
    juce::PluginDescription description;
    description.name = juce::File::createFileWithoutCheckingPath(identifier).getFileNameWithoutExtension();
    description.pluginFormatName = format;
    description.fileOrIdentifier = identifier;
    description.uniqueId = uniqueId;
    return description;
}

/// Returns settings options that keep the settings file in the given folder.
juce::PropertiesFile::Options temporarySettingsOptions(const juce::File& folder)
{
    juce::PropertiesFile::Options options;
    options.applicationName = "PluginScanTests";
    options.filenameSuffix = ".settings";
    options.folderName = folder.getFullPathName();
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    return options;
}

/// Returns the identifiers of the types in a known-plugins list, sorted.
juce::StringArray identifiersOf(const juce::KnownPluginList& knownPluginList)
{
    juce::StringArray identifiers;
    for (const auto& type : knownPluginList.getTypes()) {
        identifiers.add(type.fileOrIdentifier);
    }
    identifiers.sort(false);
    return identifiers;
}
}  // namespace

/// Checks the plugin scan support that the GUI and the CLI share, and the CLI plugin listing.
class PluginScanTests final : public juce::UnitTest
{
public:
    PluginScanTests() : juce::UnitTest("Plugin scanning", "AudioBatch") {}

    /// Runs the plugin scanning and listing checks.
    void runTest() override
    {
        testWorkerCommandLine();
        testScanLock();
        testKnownPluginListFile();
        testListFiltering();
        testPluginListCli();
    }

private:
    /// Checks which command lines launch the executable as a scan worker.
    void testWorkerCommandLine()
    {
        beginTest("Only a command line that starts with the worker argument launches a scan worker");
        const auto workerArgument = utils::format("--{}:", pluginScanWorkerProcessUid);
        expect(isPluginScanWorkerCommandLine(workerArgument + "pipe_name"));
        expect(isPluginScanWorkerCommandLine("  " + workerArgument + "pipe_name --other"));
        expect(!isPluginScanWorkerCommandLine({}));
        expect(!isPluginScanWorkerCommandLine(utils::format("--{}", pluginScanWorkerProcessUid)));
        expect(!isPluginScanWorkerCommandLine("track.wav " + workerArgument + "pipe_name"));
        expect(!isPluginScanWorkerCommandLine("--other-worker:pipe_name"));
        expect(!isPluginScanWorkerCommandLine(workerArgument.substring(1) + "pipe_name"));

        PluginScanWorker worker([] {});
        expect(!worker.initialiseFromCommandLine("--headless track.wav"));
    }

    /// Checks that the lock can be taken again by its holder, and that another process cannot take it meanwhile.
    void testScanLock()
    {
        const TemporaryDirectory directory;

        beginTest("A scan lock can be taken again by its holder and taken again after its release");
        {
            PluginScanLock lock(testLockName);
            expect(lock.tryAcquire());
            expect(lock.tryAcquire());
            lock.release();
            lock.release();
            expect(lock.tryAcquire());
        }

        beginTest("A scan lock held by another process cannot be taken until that process releases it");
        {
            LockHolder holder(testLockName, directory.getChildFile("holder"));
            expect(holder.waitUntilHeld(), "the helper process did not take the lock");
            PluginScanLock lock(testLockName);
            expect(!lock.tryAcquire());
            expect(holder.release(), "the helper process did not exit");
            expectEquals(holder.getExitCode(), 0);
            expect(lock.tryAcquire());
        }

        beginTest("Another process cannot take a scan lock that this process holds");
        {
            PluginScanLock lock(testLockName);
            expect(lock.tryAcquire());
            LockHolder holder(testLockName, directory.getChildFile("busy"));
            expect(!holder.waitUntilHeld());
            expect(holder.release());
            expectEquals(holder.getExitCode(), 1);
        }
    }

    /// Checks the KnownPlugins.xml file next to the settings, the legacy settings value, and the migration.
    void testKnownPluginListFile()
    {
        const TemporaryDirectory directory;
        juce::PropertiesFile settings(temporarySettingsOptions(directory.getChildFile("settings")));
        const auto listFile = getKnownPluginListFile(settings);
        expectEquals(listFile.getFileName(), juce::String("KnownPlugins.xml"));
        expect(listFile.getParentDirectory() == settings.getFile().getParentDirectory());

        juce::KnownPluginList saved;
        saved.addType(makeDescription("VST3", "/Library/Audio/Plug-Ins/VST3/Kept.vst3", 1));
        saved.addType(makeDescription("AudioUnit", "AudioUnit:Effects/aufx,567h,Vxng", 2));
        saved.addType(makeDescription("AudioUnit", "AudioUnit:Effects/aufx,lmtr,appl", 3));
        saved.addToBlacklist("/Library/Audio/Plug-Ins/VST3/Crashed.vst3");
        const juce::StringArray expectedIdentifiers {
            "/Library/Audio/Plug-Ins/VST3/Kept.vst3",
            "AudioUnit:Effects/aufx,567h,Vxng",
        };

        beginTest("A saved known-plugins list loads back without the built-in Apple Audio Units");
        {
            expect(saveKnownPluginList(saved, listFile));
            expect(listFile.existsAsFile());
            juce::KnownPluginList loaded;
            loadKnownPluginList(settings, loaded);
            expect(identifiersOf(loaded) == expectedIdentifiers, identifiersOf(loaded).joinIntoString(", "));
            expect(loaded.getBlacklistedFiles() == juce::StringArray("/Library/Audio/Plug-Ins/VST3/Crashed.vst3"));
        }

        beginTest("An unreadable list file leaves the list unchanged and a list that cannot be written fails");
        {
            expect(listFile.replaceWithText("not xml"));
            juce::KnownPluginList unchanged;
            unchanged.addType(makeDescription("VST3", "/Existing.vst3", 9));
            loadKnownPluginList(settings, unchanged);
            expect(identifiersOf(unchanged) == juce::StringArray("/Existing.vst3"));

            const auto blockingFile = directory.getChildFile("not_a_folder");
            expect(blockingFile.replaceWithText("a file where the list folder should be"));
            expect(!saveKnownPluginList(saved, blockingFile.getChildFile("KnownPlugins.xml")));
            expect(listFile.deleteFile());
        }

        beginTest("Without a list file, the list stored in the settings by older versions is loaded");
        {
            const auto legacyXml = saved.createXml();
            expect(legacyXml != nullptr);
            settings.setValue(legacyKnownPluginListKey, legacyXml.get());
            juce::KnownPluginList legacy;
            loadKnownPluginList(settings, legacy);
            expect(identifiersOf(legacy) == expectedIdentifiers, identifiersOf(legacy).joinIntoString(", "));
        }

        beginTest("Migration moves the legacy list to its own file and removes it from the settings");
        {
            migrateKnownPluginList(settings);
            expect(listFile.existsAsFile());
            expect(!settings.containsKey(legacyKnownPluginListKey));
            juce::KnownPluginList migrated;
            loadKnownPluginList(settings, migrated);
            expect(identifiersOf(migrated) == expectedIdentifiers, identifiersOf(migrated).joinIntoString(", "));

            // Migrating again with nothing left to move changes nothing.
            const auto migratedText = listFile.loadFileAsString();
            migrateKnownPluginList(settings);
            expectEquals(listFile.loadFileAsString(), migratedText);
        }

        beginTest("Migration keeps an existing list file and only drops the stale legacy value");
        {
            juce::KnownPluginList stale;
            stale.addType(makeDescription("VST3", "/Stale.vst3", 7));
            const auto staleXml = stale.createXml();
            expect(staleXml != nullptr);
            settings.setValue(legacyKnownPluginListKey, staleXml.get());
            migrateKnownPluginList(settings);
            expect(!settings.containsKey(legacyKnownPluginListKey));
            juce::KnownPluginList kept;
            loadKnownPluginList(settings, kept);
            expect(identifiersOf(kept) == expectedIdentifiers, identifiersOf(kept).joinIntoString(", "));
        }
    }

    /// Checks the Apple Audio Unit filter, the removal of missing plugins, the abandoned scan, and VST3_PATH.
    void testListFiltering()
    {
        const TemporaryDirectory directory;

        beginTest("Built-in Apple Audio Units are recognised by the manufacturer code of their identifier");
        {
            expect(isBuiltInApplePlugin("AudioUnit:Effects/aufx,lmtr,appl"));
            expect(isBuiltInApplePlugin("AudioUnit:Synths/aumu,dls ,appl"));
            expect(!isBuiltInApplePlugin("AudioUnit:Effects/aufx,567h,Vxng"));
            expect(!isBuiltInApplePlugin("AudioUnit:Effects/aufx,appl,Vxng"));
            expect(!isBuiltInApplePlugin("/Library/Audio/Plug-Ins/VST3/MSED.vst3"));
        }

        beginTest("A custom scanner that abandons its scan stops it without reporting failed files");
        {
            juce::KnownPluginList knownPluginList;
            auto scanner = std::make_unique<AbandoningScanner>();
            const auto& scannerCalls = scanner->calls;
            knownPluginList.setCustomScanner(std::move(scanner));
            juce::VST3PluginFormat format;
            {
                juce::PluginDirectoryScanner directoryScanner(knownPluginList, format, {}, false, juce::File(), false);
                directoryScanner.setFilesOrIdentifiersToScan({"/Missing/First.vst3", "/Missing/Second.vst3"});
                juce::String pluginBeingScanned;
                expect(!directoryScanner.scanNextFile(true, pluginBeingScanned));
                expectEquals(directoryScanner.getFailedFiles().size(), 0);
                expect(knownPluginList.isCustomScanAbandoned());
            }
            expectEquals(scannerCalls, 1);
            // The request ends with the scan, so the next scan runs again.
            expect(!knownPluginList.isCustomScanAbandoned());
        }

        beginTest("VST3_PATH is split with the platform separator, so Windows drive letters stay intact");
        {
            // What JUCE makes of VST3_PATH=C:\Plugins;D:\More on Windows, followed by its standard folder.
            const juce::FileSearchPath windowsJucePath(R"(C;\Plugins;D;\More;C:\Program Files\Common Files\VST3)");
            expectEquals(
                buildVst3SearchPath(windowsJucePath, R"(C:\Plugins;D:\More)", ";").toString(),
                juce::String(R"(C:\Plugins;D:\More;C:\Program Files\Common Files\VST3)")
            );

            // On macOS and Linux JUCE already splits the variable correctly, and the path stays the same.
            const juce::FileSearchPath macJucePath("/Users/me/VST3;/opt/vst3;/Library/Audio/Plug-Ins/VST3");
            expectEquals(
                buildVst3SearchPath(macJucePath, "/Users/me/VST3:/opt/vst3", ":").toString(), macJucePath.toString()
            );

            // A folder of the variable that is also a standard folder is listed once.
            expectEquals(
                buildVst3SearchPath(juce::FileSearchPath("/Library/VST3"), "/Library/VST3", ":").toString(),
                juce::String("/Library/VST3")
            );
            expectEquals(
                buildVst3SearchPath(juce::FileSearchPath("/Library/VST3"), {}, ":").toString(),
                juce::String("/Library/VST3")
            );
        }

        beginTest("Built-in Apple Audio Units are removed from a known plugins list, including blacklisted ones");
        {
            juce::KnownPluginList knownPluginList;
            knownPluginList.addType(makeDescription("AudioUnit", "AudioUnit:Effects/aufx,lmtr,appl", 1));
            knownPluginList.addType(makeDescription("AudioUnit", "AudioUnit:Effects/aufx,567h,Vxng", 2));
            knownPluginList.addType(makeDescription("VST3", "/Library/Audio/Plug-Ins/VST3/MSED.vst3", 3));
            knownPluginList.addToBlacklist("AudioUnit:Effects/aufx,dely,appl");
            knownPluginList.addToBlacklist("/Library/Audio/Plug-Ins/VST3/Broken.vst3");

            removeBuiltInApplePlugins(knownPluginList);

            const auto types = knownPluginList.getTypes();
            expectEquals(types.size(), 2);
            expect(std::ranges::none_of(types, [](const juce::PluginDescription& type) {
                return isBuiltInApplePlugin(type.fileOrIdentifier);
            }));
            expect(
                knownPluginList.getBlacklistedFiles() == juce::StringArray("/Library/Audio/Plug-Ins/VST3/Broken.vst3")
            );
        }

        beginTest("Missing plugins are removed only from scanned folders that still exist");
        {
            const auto scanned = directory.getChildFile("ScannedVST3");
            expect(scanned.createDirectory());
            const auto present = scanned.getChildFile("Present.vst3");
            expect(present.createDirectory());
            const auto gone = scanned.getChildFile("Gone.vst3").getFullPathName();
            const auto goneBlacklisted = scanned.getChildFile("Crashed.vst3").getFullPathName();
            const auto unscanned = directory.getChildFile("OtherFolder/Elsewhere.vst3").getFullPathName();
            const auto missingFolder = directory.getChildFile("UnpluggedDrive");
            const auto onMissingFolder = missingFolder.getChildFile("Away.vst3").getFullPathName();

            juce::AudioPluginFormatManager formatManager;
            formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());
            juce::PropertiesFile::Options options;
            options.applicationName = "AudioBatchTests";
            juce::PropertiesFile settings(directory.getChildFile("settings.xml"), options);
            juce::FileSearchPath searchPath;
            searchPath.add(scanned);
            searchPath.add(missingFolder);
            juce::PluginListComponent::setLastSearchPath(settings, *formatManager.getFormat(0), searchPath);

            juce::KnownPluginList knownPluginList;
            knownPluginList.addType(makeDescription("VST3", present.getFullPathName(), 1));
            knownPluginList.addType(makeDescription("VST3", gone, 2));
            knownPluginList.addType(makeDescription("VST3", unscanned, 3));
            knownPluginList.addType(makeDescription("VST3", onMissingFolder, 4));
            knownPluginList.addToBlacklist(goneBlacklisted);
            knownPluginList.addToBlacklist(unscanned);

            expectEquals(removeMissingPlugins(knownPluginList, formatManager, settings), 1);

            juce::StringArray expected {present.getFullPathName(), unscanned, onMissingFolder};
            expected.sort(false);
            expect(identifiersOf(knownPluginList) == expected, identifiersOf(knownPluginList).joinIntoString(", "));
            expect(knownPluginList.getBlacklistedFiles() == juce::StringArray(unscanned));
        }
    }

    /// Lists a known-plugins list from temporary settings in every grouping,
    /// and checks that a rescan is refused while another process holds the scan lock.
    void testPluginListCli()
    {
        const TemporaryDirectory directory;
        const auto options = temporarySettingsOptions(directory.getChildFile("settings"));

        beginTest("The plugin listing succeeds without any known plugins");
        expectEquals(cli::runPluginList(false, PluginListGrouping::name, options, testLockName), 0);

        beginTest("The plugin listing prints a stored list in every grouping without saving it");
        {
            const juce::PropertiesFile settings(options);
            const auto listFile = getKnownPluginListFile(settings);
            juce::KnownPluginList knownPluginList;
            auto effect = makeDescription("VST3", "/Library/Audio/Plug-Ins/VST3/Compressor.vst3", 1);
            effect.manufacturerName = "Test Audio";
            effect.category = "Fx|Dynamics";
            effect.version = "1.2.0";
            auto instrument = makeDescription("AudioUnit", "AudioUnit:Synths/aumu,synt,Test", 2);
            instrument.isInstrument = true;
            knownPluginList.addType(effect);
            knownPluginList.addType(instrument);
            expect(saveKnownPluginList(knownPluginList, listFile));
            const auto savedText = listFile.loadFileAsString();

            for (const auto grouping :
                 {
                     PluginListGrouping::name,
                     PluginListGrouping::manufacturer,
                     PluginListGrouping::type,
                     PluginListGrouping::format,
                 })
            {
                expectEquals(cli::runPluginList(false, grouping, options, testLockName), 0);
            }
            expectEquals(listFile.loadFileAsString(), savedText);
        }

        beginTest("A rescan is refused with exit code 1 while another process holds the scan lock");
        {
            LockHolder holder(testLockName, directory.getChildFile("holder"));
            expect(holder.waitUntilHeld(), "the helper process did not take the lock");
            expectEquals(cli::runPluginList(true, PluginListGrouping::name, options, testLockName), 1);
            expect(holder.release());
        }
    }
};

/// Creates the plugin scanning and listing suite.
std::unique_ptr<juce::UnitTest> createPluginScanTests()
{
    return std::make_unique<PluginScanTests>();
}
}  // namespace audiobatch::tests
