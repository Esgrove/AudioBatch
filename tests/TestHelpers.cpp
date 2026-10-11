/// Implementation of the shared test helpers:
/// the self-deleting temporary directory and the lookup of the committed audio fixtures.

#include "TestHelpers.h"

#include <cmath>

namespace audiobatch::tests
{
TemporaryDirectory::TemporaryDirectory() :
    directory(
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("AudioBatchTests")
            .getNonexistentChildFile("run", "", false)
    )
{
    directory.createDirectory();
}

TemporaryDirectory::~TemporaryDirectory()
{
    directory.deleteRecursively();
}

juce::File TemporaryDirectory::getChildFile(const juce::String& name) const
{
    return directory.getChildFile(name);
}

juce::File TemporaryDirectory::copyFixture(const juce::String& fixtureName, const juce::String& copyName) const
{
    const auto copy = directory.getChildFile(copyName.isEmpty() ? fixtureName : copyName);
    fixtureFile(fixtureName).copyFileTo(copy);
    return copy;
}

juce::File fixtureFile(const juce::String& name)
{
    // Set by CMake to the absolute path of tests/fixtures.
    return juce::File(AUDIOBATCH_TEST_FIXTURES_DIR).getChildFile(name);
}

double peakDecibels(const AudioAnalysisRecord& record)
{
    return static_cast<double>(juce::Decibels::gainToDecibels(std::abs(record.overallPeak)));
}
}  // namespace audiobatch::tests
