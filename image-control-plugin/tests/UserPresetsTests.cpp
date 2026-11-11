/// Exercises the real ImageControl user-preset library in disposable directories.
/// Tests cover portable names, strict sound-only XML, optimistic revisions, independent instances,
/// concurrent writers, explicit import/export, and filesystem failures without touching installed presets.

#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "UserPresets.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <latch>
#include <limits>
#include <memory>
#include <thread>

namespace esgrove::image_control::test
{
namespace
{
/// Owns one isolated test workspace. The library directory remains absent until an explicit successful write.
class UserPresetsTest : public testing::Test
{
public:
    /// Returns a complete valid sound snapshot without audition parameters or editor state.
    ProcessingSnapshot sound()
    {
        return captureProcessingState(processor.getParameters());
    }

    /// Writes a deliberately modified document in the scratch parent for import validation.
    juce::File document(const juce::String& text, const juce::String& filename = "input.xml") const
    {
        const auto file = root.getChildFile(filename);
        EXPECT_TRUE(file.replaceWithText(text));
        return file;
    }

    /// Owns the immutable parameter schema for the preset library's lifetime.
    ImageControlProcessor processor;
    /// Unique disposable workspace, created before each test and removed afterward.
    juce::File root {
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("ImageControl-Presets-Test-" + juce::Uuid().toString()),
    };
    /// Nested storage path left absent until a successful explicit write.
    juce::File directory {root.getChildFile("library")};
    /// Real synchronous library under test, bound only to this fixture's processor and storage.
    UserPresets library {processor.getParameters(), directory};

protected:
    /// Creates the disposable parent, but not the nested library directory.
    void SetUp() override
    {
        ASSERT_TRUE(root.createDirectory().wasOk());
    }

    /// Deletes only this test's uniquely named workspace.
    void TearDown() override
    {
        EXPECT_TRUE(root.deleteRecursively());
    }
};

TEST_F(UserPresetsTest, ConstructionListingAndRejectedSavesDoNotCreateStorage)
{
    EXPECT_EQ(library.getDirectory(), directory);
    EXPECT_FALSE(directory.exists());
    const auto listing = library.list();
    ASSERT_TRUE(listing.has_value());
    EXPECT_TRUE(listing->presets.empty());
    EXPECT_TRUE(listing->rejectedFiles.empty());
    EXPECT_FALSE(library.saveAs("../escape", sound()).has_value());
    EXPECT_FALSE(library.saveAs("Invalid", {}).has_value());
    EXPECT_FALSE(directory.exists());
}

TEST_F(UserPresetsTest, DefaultPathIsPluginOwned)
{
#if JUCE_MAC
    EXPECT_EQ(
        UserPresets::defaultDirectory(),
        juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile("Library/Application Support/esgrove/ImageControl/Presets")
    );
#else
    EXPECT_EQ(
        UserPresets::defaultDirectory(),
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("esgrove/ImageControl/Presets")
    );
#endif
}

TEST_F(UserPresetsTest, PortableNamesRejectTraversalControlCharactersAndReservedWindowsDevices)
{
    const juce::StringArray invalid {
        "",     " ",    " Leading", "Trailing ", ".",       "..",       ".hidden", "trailing.", "../escape",
        "a/b",  "a\\b", "C:drive",  "a<b",       "a>b",     "a\"b",     "a|b",     "a?b",       "a*b",
        "a\nb", "a\tb", "con",      "CON.xml",   "prn",     "aux",      "nul",     "com1",      "COM9.wav",
        "lpt1", "LPT9", "CLOCK$",   "CONIN$",    "CONOUT$", "CON .xml",
    };
    for (const auto& name : invalid) {
        SCOPED_TRACE(name.toStdString());
        EXPECT_TRUE(UserPresets::validateName(name).failed());
    }
    EXPECT_TRUE(UserPresets::validateName(juce::String::charToString(127)).failed());
    EXPECT_TRUE(UserPresets::validateName(juce::String::charToString(159)).failed());
    EXPECT_TRUE(UserPresets::validateName(juce::String::fromUTF8("COM¹")).failed());
    EXPECT_TRUE(UserPresets::validateName(juce::String::repeatedString("a", 121)).failed());
    EXPECT_TRUE(UserPresets::validateName("Warm Bass 01").wasOk());
    EXPECT_TRUE(UserPresets::validateName("Wide.v2").wasOk());
    EXPECT_TRUE(UserPresets::validateName("COM10").wasOk());
    EXPECT_TRUE(UserPresets::validateName(juce::String::fromUTF8("Ääni & Stereo")).wasOk());
}

TEST_F(UserPresetsTest, SaveListLoadRoundTripsOnlyDetachedSound)
{
    auto snapshot = sound();
    snapshot.at(OUTPUT_GAIN_ID) = -6.0F;
    const auto saved = library.saveAs("Warm & Wide", snapshot);
    ASSERT_TRUE(saved.has_value());
    EXPECT_TRUE(directory.isDirectory());
    EXPECT_EQ(saved->name, "Warm & Wide");
    EXPECT_FALSE(saved->id.isEmpty());
    EXPECT_FALSE(saved->fingerprint.isEmpty());
    EXPECT_EQ(saved->file.getFileName(), saved->id + ".xml");
    EXPECT_TRUE(sameProcessingState(saved->snapshot, snapshot));
    const auto xml = juce::parseXML(saved->file);
    ASSERT_NE(xml, nullptr);
    EXPECT_TRUE(xml->hasTagName("ImageControlPreset"));
    EXPECT_EQ(xml->getStringAttribute("plugin"), "com.esgrove.imagecontrol");
    EXPECT_EQ(xml->getStringAttribute("version"), "1");
    ASSERT_EQ(xml->getNumChildElements(), 1);
    EXPECT_TRUE(xml->getChildElement(0)->hasTagName("SoundState"));
    EXPECT_EQ(xml->getChildByName("EditorState"), nullptr);
    EXPECT_EQ(xml->getChildByName("History"), nullptr);
    EXPECT_FALSE(saved->snapshot.contains(BYPASS_ID));
    EXPECT_FALSE(saved->snapshot.contains(DELTA_ID));
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        EXPECT_FALSE(saved->snapshot.contains(soloParameterId(band)));
    }
    EXPECT_TRUE(saved->snapshot.contains(MONO_ID));
    const auto listing = library.list();
    ASSERT_TRUE(listing.has_value());
    ASSERT_EQ(listing->presets.size(), 1U);
    EXPECT_TRUE(listing->rejectedFiles.empty());
    const auto loaded = library.load(listing->presets.front());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->fingerprint, saved->fingerprint);
    EXPECT_TRUE(sameProcessingState(loaded->snapshot, snapshot));
    EXPECT_NE(processor.getParameters().getRawParameterValue(OUTPUT_GAIN_ID)->load(), -6.0F);
}

TEST_F(UserPresetsTest, NamesCollideCaseInsensitivelyIncludingCorruptFiles)
{
    ASSERT_TRUE(library.saveAs("Warm", sound()).has_value());
    EXPECT_FALSE(library.saveAs("warm", sound()).has_value());
    EXPECT_FALSE(library.saveAs("WARM", sound()).has_value());
    const auto second = library.saveAs("Second", sound());
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(library.rename(*second, "wArM").has_value());
    const auto caseRename = library.rename(*second, "SECOND");
    ASSERT_TRUE(caseRename.has_value());
    EXPECT_EQ(caseRename->name, "SECOND");
    EXPECT_EQ(caseRename->file, second->file);
    EXPECT_TRUE(directory.getChildFile("Blocked.XML").replaceWithText("not XML"));
    EXPECT_FALSE(library.saveAs("blocked", sound()).has_value());
    const auto listing = library.list();
    ASSERT_TRUE(listing.has_value());
    EXPECT_EQ(listing->presets.size(), 2U);
    ASSERT_EQ(listing->rejectedFiles.size(), 1U);
    EXPECT_FALSE(listing->rejectedFiles.front().message.isEmpty());
}

TEST_F(UserPresetsTest, UpdateRenameAndDeleteRequireTheExactRevision)
{
    const auto original = library.saveAs("Original", sound());
    ASSERT_TRUE(original.has_value());
    UserPresets const other(processor.getParameters(), directory);
    auto changed = original->snapshot;
    changed.at(OUTPUT_GAIN_ID) = -9.0F;
    const auto updated = other.update(*original, changed);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->id, original->id);
    EXPECT_EQ(updated->file, original->file);
    EXPECT_EQ(updated->name, original->name);
    EXPECT_TRUE(sameProcessingState(updated->snapshot, changed));
    EXPECT_NE(updated->fingerprint, original->fingerprint);
    EXPECT_FALSE(library.load(*original).has_value());
    EXPECT_FALSE(library.update(*original, sound()).has_value());
    EXPECT_FALSE(library.rename(*original, "Stale rename").has_value());
    EXPECT_TRUE(library.remove(*original).failed());
    const auto renamed = library.rename(*updated, "Renamed");
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(renamed->file, original->file);
    EXPECT_EQ(renamed->id, original->id);
    EXPECT_TRUE(sameProcessingState(renamed->snapshot, changed));
    EXPECT_TRUE(library.remove(*updated).failed());
    EXPECT_TRUE(other.remove(*renamed).wasOk());
    EXPECT_FALSE(original->file.exists());
    EXPECT_TRUE(library.remove(*renamed).failed());
    EXPECT_FALSE(library.load(*renamed).has_value());
}

TEST_F(UserPresetsTest, CombinedUpdateReplacesNameAndSoundWhilePreservingIdentityAndFile)
{
    const auto saved = library.saveAs("Original", sound());
    ASSERT_TRUE(saved.has_value());
    auto changed = saved->snapshot;
    changed.at(OUTPUT_GAIN_ID) = -9.0F;
    changed.at(MONO_ID) = 1.0F;
    const auto updated = library.update(*saved, "Updated", changed);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->id, saved->id);
    EXPECT_EQ(updated->file, saved->file);
    EXPECT_EQ(updated->name, "Updated");
    EXPECT_NE(updated->fingerprint, saved->fingerprint);
    EXPECT_TRUE(sameProcessingState(updated->snapshot, changed));
    EXPECT_FALSE(library.load(*saved).has_value());
    const auto loaded = library.load(*updated);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, saved->id);
    EXPECT_EQ(loaded->file, saved->file);
    EXPECT_EQ(loaded->name, "Updated");
    EXPECT_EQ(loaded->fingerprint, updated->fingerprint);
    EXPECT_TRUE(sameProcessingState(loaded->snapshot, changed));
    const auto listing = library.list();
    ASSERT_TRUE(listing.has_value());
    ASSERT_EQ(listing->presets.size(), 1U);
    EXPECT_TRUE(listing->rejectedFiles.empty());
    EXPECT_EQ(listing->presets.front().fingerprint, updated->fingerprint);
    EXPECT_EQ(directory.findChildFiles(juce::File::findFiles, false).size(), 1);
    const auto xml = juce::parseXML(updated->file);
    ASSERT_NE(xml, nullptr);
    EXPECT_EQ(xml->getStringAttribute("version"), "1");
    EXPECT_TRUE(sameProcessingState(sound(), saved->snapshot));
}

TEST_F(UserPresetsTest, CombinedUpdateAllowsCaseOnlyRenameWithChangedSound)
{
    const auto saved = library.saveAs("Warm", sound());
    ASSERT_TRUE(saved.has_value());
    auto changed = saved->snapshot;
    changed.at(OUTPUT_GAIN_ID) = -6.0F;
    const auto updated = library.update(*saved, "WARM", changed);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->name, "WARM");
    EXPECT_EQ(updated->id, saved->id);
    EXPECT_EQ(updated->file, saved->file);
    EXPECT_NE(updated->fingerprint, saved->fingerprint);
    EXPECT_TRUE(sameProcessingState(updated->snapshot, changed));
    EXPECT_TRUE(library.load(*updated).has_value());
    EXPECT_EQ(directory.findChildFiles(juce::File::findFiles, false).size(), 1);
}

TEST_F(UserPresetsTest, CombinedUpdateNameFailuresLeaveNameSoundAndRevisionUntouched)
{
    const auto saved = library.saveAs("Original", sound());
    ASSERT_TRUE(saved.has_value());
    const auto occupied = library.saveAs("Occupied", sound());
    ASSERT_TRUE(occupied.has_value());
    const auto before = saved->file.loadFileAsString();
    const auto occupiedBefore = occupied->file.loadFileAsString();
    ASSERT_TRUE(directory.getChildFile("Blocked.XML").replaceWithText("not XML"));
    auto changed = saved->snapshot;
    changed.at(OUTPUT_GAIN_ID) = -9.0F;
    const juce::StringArray rejectedNames {"oCCUPIED", "blocked", "", " Leading", "../escape", "CON", "trailing."};
    for (const auto& name : rejectedNames) {
        SCOPED_TRACE(name.toStdString());
        EXPECT_FALSE(library.update(*saved, name, changed).has_value());
        EXPECT_EQ(saved->file.loadFileAsString(), before);
        const auto loaded = library.load(*saved);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->name, saved->name);
        EXPECT_EQ(loaded->fingerprint, saved->fingerprint);
        EXPECT_TRUE(sameProcessingState(loaded->snapshot, saved->snapshot));
    }
    EXPECT_EQ(occupied->file.loadFileAsString(), occupiedBefore);
    EXPECT_EQ(directory.getChildFile("Blocked.XML").loadFileAsString(), "not XML");
    EXPECT_EQ(directory.findChildFiles(juce::File::findFiles, false).size(), 3);
}

TEST_F(UserPresetsTest, CombinedUpdateRejectsStaleRevisionEvenWhenNameAndSoundAreIdentical)
{
    const auto saved = library.saveAs("Original", sound());
    ASSERT_TRUE(saved.has_value());
    const UserPresets other(processor.getParameters(), directory);
    const auto refreshed = other.update(*saved, saved->name, saved->snapshot);
    ASSERT_TRUE(refreshed.has_value());
    EXPECT_NE(refreshed->fingerprint, saved->fingerprint);
    const auto before = refreshed->file.loadFileAsString();
    auto changed = saved->snapshot;
    changed.at(OUTPUT_GAIN_ID) = -9.0F;
    EXPECT_FALSE(library.update(*saved, "Stale replacement", changed).has_value());
    EXPECT_EQ(refreshed->file.loadFileAsString(), before);
    const auto loaded = library.load(*refreshed);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->name, saved->name);
    EXPECT_EQ(loaded->fingerprint, refreshed->fingerprint);
    EXPECT_TRUE(sameProcessingState(loaded->snapshot, saved->snapshot));
    EXPECT_EQ(directory.findChildFiles(juce::File::findFiles, false).size(), 1);
}

TEST_F(UserPresetsTest, CombinedUpdateSnapshotValidationFailuresDoNotCommitTheName)
{
    const auto saved = library.saveAs("Original", sound());
    ASSERT_TRUE(saved.has_value());
    const auto before = saved->file.loadFileAsString();
    std::array<ProcessingSnapshot, 6> invalid;
    invalid.fill(saved->snapshot);
    invalid[0].at(OUTPUT_GAIN_ID) = std::numeric_limits<float>::infinity();
    invalid[1].at(OUTPUT_GAIN_ID) = 1000.0F;
    invalid[2]["editorWidth"] = 1200.0F;
    invalid[3][BYPASS_ID] = 1.0F;
    invalid[4].erase(OUTPUT_GAIN_ID);
    invalid[5].clear();
    for (const auto& snapshot : invalid) {
        EXPECT_FALSE(library.update(*saved, "Updated", snapshot).has_value());
        EXPECT_EQ(saved->file.loadFileAsString(), before);
        const auto loaded = library.load(*saved);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->name, saved->name);
        EXPECT_EQ(loaded->fingerprint, saved->fingerprint);
        EXPECT_TRUE(sameProcessingState(loaded->snapshot, saved->snapshot));
    }
    EXPECT_EQ(directory.findChildFiles(juce::File::findFiles, false).size(), 1);
}

TEST_F(UserPresetsTest, IdenticalUpdateStillInvalidatesOldSelection)
{
    const auto saved = library.saveAs("Same sound", sound());
    ASSERT_TRUE(saved.has_value());
    const auto updated = library.update(*saved, saved->snapshot);
    ASSERT_TRUE(updated.has_value());
    EXPECT_NE(saved->fingerprint, updated->fingerprint);
    EXPECT_TRUE(sameProcessingState(saved->snapshot, updated->snapshot));
    EXPECT_FALSE(library.load(*saved).has_value());
}

TEST_F(UserPresetsTest, ExternalSameLengthModificationAndRecreatedIdentityRejectStaleReads)
{
    const auto saved = library.saveAs("Original", sound());
    ASSERT_TRUE(saved.has_value());
    const auto time = saved->file.getLastModificationTime();
    const auto bytes = saved->file.loadFileAsString();
    EXPECT_TRUE(saved->file.replaceWithText(bytes.replace("Original", "Modified")));
    EXPECT_TRUE(saved->file.setLastModificationTime(time));
    EXPECT_FALSE(library.load(*saved).has_value());
    EXPECT_FALSE(library.update(*saved, "Stale replacement", sound()).has_value());
    EXPECT_EQ(saved->file.loadFileAsString(), bytes.replace("Original", "Modified"));
    EXPECT_TRUE(library.remove(*saved).failed());
    EXPECT_TRUE(saved->file.replaceWithText(bytes));
    const auto restored = library.load(*saved);
    ASSERT_TRUE(restored.has_value());
    EXPECT_TRUE(library.remove(*restored).wasOk());
    const auto replacement = library.saveAs("Original", sound());
    ASSERT_TRUE(replacement.has_value());
    EXPECT_NE(replacement->id, saved->id);
    EXPECT_FALSE(library.update(*saved, sound()).has_value());
}

TEST_F(UserPresetsTest, ImportExportAndRenameHaveExplicitCollisionAndOverwritePolicy)
{
    const auto saved = library.saveAs("Exported", sound());
    ASSERT_TRUE(saved.has_value());
    const auto exported = root.getChildFile("Portable.xml");
    EXPECT_TRUE(library.exportPreset(*saved, exported).wasOk());
    EXPECT_EQ(exported.loadFileAsString(), saved->file.loadFileAsString());
    const auto before = exported.loadFileAsString();
    EXPECT_TRUE(library.exportPreset(*saved, exported).failed());
    EXPECT_EQ(exported.loadFileAsString(), before);
    EXPECT_TRUE(library.exportPreset(*saved, root.getChildFile("PORTABLE.xml")).failed());
    EXPECT_TRUE(library.exportPreset(*saved, saved->file).failed());
    EXPECT_FALSE(library.importPreset(exported).has_value());
    const auto imported = library.importPreset(exported, "Imported");
    ASSERT_TRUE(imported.has_value());
    EXPECT_NE(imported->id, saved->id);
    EXPECT_TRUE(sameProcessingState(imported->snapshot, saved->snapshot));
    EXPECT_EQ(imported->name, "Imported");
    EXPECT_TRUE(library.remove(*saved).wasOk());
    const auto originalName = library.importPreset(exported);
    ASSERT_TRUE(originalName.has_value());
    EXPECT_EQ(originalName->name, "Exported");
    EXPECT_TRUE(library.exportPreset(*saved, root.getChildFile("stale.xml")).failed());
}

TEST_F(UserPresetsTest, UnknownCorruptAndFutureDocumentsAreRejectedWithoutChangingLibrary)
{
    const auto saved = library.saveAs("Valid", sound());
    ASSERT_TRUE(saved.has_value());
    const auto original = saved->file.loadFileAsString();
    juce::StringArray invalid;
    invalid.add("not XML");
    invalid.add("<ImageControlPreset>");
    invalid.add(original.replace("com.esgrove.imagecontrol", "com.other.plugin"));
    const auto parsed = juce::parseXML(original);
    ASSERT_NE(parsed, nullptr);
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->setAttribute("version", "99");
        invalid.add(xml->toString());
    }
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->setAttribute("id", "not-a-uuid");
        invalid.add(xml->toString());
    }
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->setAttribute("revision", "");
        invalid.add(xml->toString());
    }
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->setAttribute("unknown", 1);
        invalid.add(xml->toString());
    }
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->createNewChildElement("EditorState");
        invalid.add(xml->toString());
    }
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->getChildElement(0)->createNewChildElement("Unknown");
        invalid.add(xml->toString());
    }
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->getChildElement(0)->setAttribute("version", "2");
        invalid.add(xml->toString());
    }
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->getChildElement(0)->getChildElement(0)->setAttribute("id", "unknown_parameter");
        invalid.add(xml->toString());
    }
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->getChildElement(0)->addChildElement(
            std::make_unique<juce::XmlElement>(*xml->getChildElement(0)->getChildElement(0)).release()
        );
        invalid.add(xml->toString());
    }
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->getChildElement(0)->getChildElement(0)->setAttribute("value", "NaN");
        invalid.add(xml->toString());
    }
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->getChildElement(0)->getChildElement(0)->setAttribute("value", "99999999");
        invalid.add(xml->toString());
    }
    {
        auto xml = std::make_unique<juce::XmlElement>(*parsed);
        xml->getChildElement(0)->removeChildElement(xml->getChildElement(0)->getChildElement(0), true);
        invalid.add(xml->toString());
    }
    invalid.add("<Extra/>" + original);
    invalid.add("garbage" + original);
    invalid.add("<?unsupported instruction?>" + original);
    invalid.add(original + "<Extra/>");
    invalid.add(original + "garbage");
    invalid.add("<!DOCTYPE ImageControlPreset [<!ENTITY source SYSTEM 'file:///not-a-preset'>]>" + original);
    invalid.add(juce::String::repeatedString("x", 65 * 1024));
    for (const auto& text : invalid) {
        SCOPED_TRACE(text.substring(0, 160).toStdString());
        EXPECT_FALSE(library.importPreset(document(text), "Rejected").has_value());
    }
    constexpr std::array invalidUtf8 {'<', static_cast<char>(0xff), '>'};
    const auto invalidFile = root.getChildFile("invalid-utf8.xml");
    ASSERT_TRUE(invalidFile.replaceWithData(invalidUtf8.data(), invalidUtf8.size()));
    EXPECT_FALSE(library.importPreset(invalidFile, "Invalid UTF8").has_value());
    constexpr std::array nulXml {'<', '\0', '>'};
    ASSERT_TRUE(invalidFile.replaceWithData(nulXml.data(), nulXml.size()));
    EXPECT_FALSE(library.importPreset(invalidFile, "Nul XML").has_value());
    EXPECT_EQ(saved->file.loadFileAsString(), original);
    const auto listing = library.list();
    ASSERT_TRUE(listing.has_value());
    EXPECT_EQ(listing->presets.size(), 1U);
}

TEST_F(UserPresetsTest, ListingIsSortedAndNeverAppliesTheSoundSnapshot)
{
    auto snapshot = sound();
    snapshot.at(OUTPUT_GAIN_ID) = -12.0F;
    ASSERT_TRUE(library.saveAs("zebra", snapshot).has_value());
    ASSERT_TRUE(library.saveAs("Alpha", snapshot).has_value());
    ASSERT_TRUE(library.saveAs("middle", snapshot).has_value());
    const auto before = sound();
    const auto listing = library.list();
    ASSERT_TRUE(listing.has_value());
    ASSERT_EQ(listing->presets.size(), 3U);
    EXPECT_EQ(listing->presets[0].name, "Alpha");
    EXPECT_EQ(listing->presets[1].name, "middle");
    EXPECT_EQ(listing->presets[2].name, "zebra");
    EXPECT_TRUE(sameProcessingState(before, sound()));
}

TEST_F(UserPresetsTest, SnapshotValidationFailuresRetainTheOriginalAndDoNotLeakTemporaryFiles)
{
    const auto saved = library.saveAs("Original", sound());
    ASSERT_TRUE(saved.has_value());
    const auto before = saved->file.loadFileAsString();
    auto invalid = sound();
    invalid.at(OUTPUT_GAIN_ID) = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(library.update(*saved, invalid).has_value());
    invalid = sound();
    invalid.at(OUTPUT_GAIN_ID) = 1000.0F;
    EXPECT_FALSE(library.update(*saved, invalid).has_value());
    invalid = sound();
    invalid["editorWidth"] = 1200.0F;
    EXPECT_FALSE(library.saveAs("Unknown parameter", invalid).has_value());
    invalid = sound();
    invalid[BYPASS_ID] = 1.0F;
    EXPECT_FALSE(library.saveAs("Audition state", invalid).has_value());
    invalid = sound();
    invalid.erase(OUTPUT_GAIN_ID);
    EXPECT_FALSE(library.update(*saved, invalid).has_value());
    EXPECT_EQ(saved->file.loadFileAsString(), before);
    EXPECT_EQ(directory.findChildFiles(juce::File::findFiles, false).size(), 1);
}

TEST_F(UserPresetsTest, ForeignSelectionsAndUnavailableDirectoriesFailExplicitly)
{
    const auto saved = library.saveAs("Saved", sound());
    ASSERT_TRUE(saved.has_value());
    UserPresets const different(processor.getParameters(), root.getChildFile("other"));
    EXPECT_FALSE(different.load(*saved).has_value());
    EXPECT_TRUE(different.remove(*saved).failed());
    const auto blocked = document("regular file", "blocked");
    UserPresets const invalid(processor.getParameters(), blocked);
    EXPECT_FALSE(invalid.list().has_value());
    EXPECT_FALSE(invalid.saveAs("Cannot save", sound()).has_value());
    UserPresets const blockedParent(processor.getParameters(), blocked.getChildFile("library"));
    EXPECT_FALSE(blockedParent.saveAs("Cannot create", sound()).has_value());
    EXPECT_TRUE(library.exportPreset(*saved, root.getChildFile("missing/output.xml")).failed());
    EXPECT_TRUE(library.exportPreset(*saved, root).failed());
    EXPECT_FALSE(library.importPreset(root).has_value());
    EXPECT_FALSE(library.importPreset(root.getChildFile("missing.xml")).has_value());
    EXPECT_TRUE(library.load(*saved).has_value());
}

TEST_F(UserPresetsTest, SymlinkEntriesAreNeverLoadedOverwrittenOrDeleted)
{
    const auto saved = library.saveAs("Saved", sound());
    ASSERT_TRUE(saved.has_value());
    const auto external = root.getChildFile("external.xml");
    ASSERT_TRUE(saved->file.copyFileTo(external));
    const auto link = directory.getChildFile("linked.xml");
    if (!external.createSymbolicLink(link, false)) {
        GTEST_SKIP() << "This test account cannot create symbolic links.";
    }
    EXPECT_FALSE(library.importPreset(link, "Linked").has_value());
    auto forged = *saved;
    forged.file = link;
    EXPECT_FALSE(library.load(forged).has_value());
    EXPECT_TRUE(library.remove(forged).failed());
    EXPECT_TRUE(external.existsAsFile());
    const auto listing = library.list();
    ASSERT_TRUE(listing.has_value());
    EXPECT_EQ(listing->presets.size(), 1U);
    EXPECT_EQ(listing->rejectedFiles.size(), 1U);
}

TEST_F(UserPresetsTest, DuplicateExternalNamesAndIdentitiesAreAllRejected)
{
    const auto saved = library.saveAs("Unique", sound());
    ASSERT_TRUE(saved.has_value());
    ASSERT_TRUE(saved->file.copyFileTo(directory.getChildFile("duplicate.xml")));
    const auto listing = library.list();
    ASSERT_TRUE(listing.has_value());
    EXPECT_TRUE(listing->presets.empty());
    EXPECT_EQ(listing->rejectedFiles.size(), 2U);
    EXPECT_FALSE(library.saveAs("UNIQUE", sound()).has_value());
}

TEST_F(UserPresetsTest, FilesystemWriteFailuresRetainTheSelectedPreset)
{
    const auto saved = library.saveAs("Retained", sound());
    ASSERT_TRUE(saved.has_value());
    const auto before = saved->file.loadFileAsString();
    auto changed = saved->snapshot;
    changed.at(OUTPUT_GAIN_ID) = -9.0F;
#if JUCE_WINDOWS
    ASSERT_TRUE(saved->file.setReadOnly(true));
    const auto updated = library.update(*saved, "Cannot update", changed);
    const auto renamed = library.rename(*saved, "Cannot rename");
    EXPECT_TRUE(saved->file.setReadOnly(false));
#else
    ASSERT_TRUE(directory.setReadOnly(true, false));
    if (directory.hasWriteAccess()) {
        EXPECT_TRUE(directory.setReadOnly(false, false));
        GTEST_SKIP() << "This test account bypasses directory write permissions.";
    }
    const auto updated = library.update(*saved, "Cannot update", changed);
    const auto renamed = library.rename(*saved, "Cannot rename");
    EXPECT_TRUE(directory.setReadOnly(false, false));
#endif
    EXPECT_FALSE(updated.has_value());
    EXPECT_FALSE(renamed.has_value());
    EXPECT_EQ(saved->file.loadFileAsString(), before);
    EXPECT_EQ(directory.findChildFiles(juce::File::findFiles, false).size(), 1);
    EXPECT_TRUE(library.load(*saved).has_value());
}

TEST_F(UserPresetsTest, TwoInstancesSerializeConflictingCreatesAndRevisionUpdates)
{
    const auto snapshot = sound();
    const UserPresets other(processor.getParameters(), directory);
    std::atomic<int> successes {0};
    std::latch start(1);
    std::thread first([&] {
        start.wait();
        if (library.saveAs("Concurrent", snapshot)) {
            ++successes;
        }
    });
    std::thread second([&] {
        start.wait();
        if (other.saveAs("concurrent", snapshot)) {
            ++successes;
        }
    });
    start.count_down();
    first.join();
    second.join();
    EXPECT_EQ(successes.load(), 1);
    const auto listing = library.list();
    ASSERT_TRUE(listing.has_value());
    ASSERT_EQ(listing->presets.size(), 1U);
    const auto selected = listing->presets.front();
    auto firstSound = snapshot;
    firstSound.at(OUTPUT_GAIN_ID) = -6.0F;
    auto secondSound = snapshot;
    secondSound.at(OUTPUT_GAIN_ID) = -9.0F;
    successes.store(0);
    std::latch updateStart(1);
    std::thread firstUpdate([&] {
        updateStart.wait();
        if (library.update(selected, "First update", firstSound)) {
            ++successes;
        }
    });
    std::thread secondUpdate([&] {
        updateStart.wait();
        if (other.update(selected, "Second update", secondSound)) {
            ++successes;
        }
    });
    updateStart.count_down();
    firstUpdate.join();
    secondUpdate.join();
    EXPECT_EQ(successes.load(), 1);
    const auto updatedListing = library.list();
    ASSERT_TRUE(updatedListing.has_value());
    ASSERT_EQ(updatedListing->presets.size(), 1U);
    EXPECT_TRUE(updatedListing->rejectedFiles.empty());
    const auto& updated = updatedListing->presets.front();
    EXPECT_EQ(updated.id, selected.id);
    EXPECT_EQ(updated.file, selected.file);
    EXPECT_NE(updated.fingerprint, selected.fingerprint);
    ASSERT_TRUE(updated.name == "First update" || updated.name == "Second update");
    EXPECT_TRUE(sameProcessingState(updated.snapshot, updated.name == "First update" ? firstSound : secondSound));
    EXPECT_EQ(directory.findChildFiles(juce::File::findFiles, false).size(), 1);
}

TEST_F(UserPresetsTest, TwoInstancesSerializeCombinedUpdateNameCollisions)
{
    const auto firstPreset = library.saveAs("First", sound());
    ASSERT_TRUE(firstPreset.has_value());
    const auto secondPreset = library.saveAs("Second", sound());
    ASSERT_TRUE(secondPreset.has_value());
    const auto firstBefore = firstPreset->file.loadFileAsString();
    const auto secondBefore = secondPreset->file.loadFileAsString();
    auto firstSound = firstPreset->snapshot;
    firstSound.at(OUTPUT_GAIN_ID) = -6.0F;
    auto secondSound = secondPreset->snapshot;
    secondSound.at(OUTPUT_GAIN_ID) = -9.0F;
    const UserPresets other(processor.getParameters(), directory);
    std::atomic<int> successes {0};
    std::latch start(1);
    std::thread first([&] {
        start.wait();
        if (library.update(*firstPreset, "Shared", firstSound)) {
            ++successes;
        }
    });
    std::thread second([&] {
        start.wait();
        if (other.update(*secondPreset, "shared", secondSound)) {
            ++successes;
        }
    });
    start.count_down();
    first.join();
    second.join();
    EXPECT_EQ(successes.load(), 1);
    const auto listing = library.list();
    ASSERT_TRUE(listing.has_value());
    ASSERT_EQ(listing->presets.size(), 2U);
    EXPECT_TRUE(listing->rejectedFiles.empty());
    for (const auto& preset : listing->presets) {
        const bool isFirst = preset.id == firstPreset->id;
        const auto& original = isFirst ? *firstPreset : *secondPreset;
        EXPECT_EQ(preset.file, original.file);
        if (preset.name.equalsIgnoreCase("shared")) {
            EXPECT_EQ(preset.name, isFirst ? "Shared" : "shared");
            EXPECT_NE(preset.fingerprint, original.fingerprint);
            EXPECT_TRUE(sameProcessingState(preset.snapshot, isFirst ? firstSound : secondSound));
        } else {
            EXPECT_EQ(preset.name, original.name);
            EXPECT_EQ(preset.fingerprint, original.fingerprint);
            EXPECT_TRUE(sameProcessingState(preset.snapshot, original.snapshot));
            EXPECT_EQ(preset.file.loadFileAsString(), isFirst ? firstBefore : secondBefore);
        }
    }
    EXPECT_EQ(directory.findChildFiles(juce::File::findFiles, false).size(), 2);
}
}  // namespace
}  // namespace esgrove::image_control::test
