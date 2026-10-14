/// Implementation of the shared test helpers:
/// the self-deleting temporary directory and the lookup of the committed audio fixtures.

#include "TestHelpers.h"

#include <cmath>

namespace audiobatch::tests
{
/// Installs the capture after saving the runner's logger.
ScopedLogCapture::ScopedLogCapture() : previousLogger(juce::Logger::getCurrentLogger())
{
    juce::Logger::setCurrentLogger(this);
}

/// Restores the runner's logger after the tested workers have finished.
ScopedLogCapture::~ScopedLogCapture()
{
    juce::Logger::setCurrentLogger(previousLogger);
}

/// Returns a snapshot without racing worker-thread log messages.
juce::StringArray ScopedLogCapture::getMessages() const
{
    const juce::ScopedLock scopedLock(lock);
    return messages;
}

/// Collects one complete message under the capture lock.
void ScopedLogCapture::logMessage(const juce::String& message)
{
    const juce::ScopedLock scopedLock(lock);
    messages.add(message);
}

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
