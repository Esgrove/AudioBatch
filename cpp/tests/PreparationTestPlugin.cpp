/// Implements the preparation fixture's lifecycle recording and Audio Unit state-restoration constraints.

#include "PreparationTestPlugin.h"

#include "TestPlugin.h"

namespace audiobatch::tests
{
/// Starts with a saved gain different from the unity gain reinstated by every AU preparation.
PreparationTestPlugin::PreparationTestPlugin(const bool emulateAudioUnit) :
    TestPlugin({.gain = 0.25f}),
    audioUnit(emulateAudioUnit)
{}

/// Returns the ordered host lifecycle calls without allowing tests to mutate the record.
const juce::StringArray& PreparationTestPlugin::getLifecycle() const
{
    return lifecycle;
}

/// Returns the sample rate supplied by the most recent preparation.
double PreparationTestPlugin::getLastSampleRate() const
{
    return lastSampleRate;
}

/// Returns the block size supplied by the most recent preparation.
int PreparationTestPlugin::getPreparedBlockSize() const
{
    return preparedBlockSize;
}

/// Starts a fresh lifecycle record without changing the plugin's initialization state.
void PreparationTestPlugin::clearLifecycle()
{
    lifecycle.clear();
}

/// Advertises AU extensions only when testing the Audio Unit lifecycle.
juce::AudioPluginExtensions::AudioUnitClient* PreparationTestPlugin::getAudioUnitClient()
{
    return audioUnit ? this : nullptr;
}

/// Exposes the same format identity to const host queries.
const juce::AudioPluginExtensions::AudioUnitClient* PreparationTestPlugin::getAudioUnitClient() const
{
    return audioUnit ? this : nullptr;
}

/// Prevents native offline-render and latency queries from reaching the operating system.
AudioUnit PreparationTestPlugin::getAudioUnitHandle() const noexcept
{
    return nullptr;
}

/// Emulates AU initialization resetting parameters, so a second preparation destroys restored state.
void PreparationTestPlugin::prepareToPlay(const double sampleRate, const int samplesPerBlock)
{
    lifecycle.add("prepare");
    preparedBlockSize = samplesPerBlock;
    lastSampleRate = sampleRate;
    TestPlugin::prepareToPlay(sampleRate, samplesPerBlock);
    initialized = true;
    if (audioUnit) {
        constexpr float defaultGain = 1.0f;
        TestPlugin::setStateInformation(&defaultGain, sizeof(defaultGain));
    }
}

/// Requires the next file to initialize the plugin again before AU state can be restored.
void PreparationTestPlugin::releaseResources()
{
    lifecycle.add("release");
    initialized = false;
    TestPlugin::releaseResources();
}

/// Records the chain reset without changing the restored gain.
void PreparationTestPlugin::reset()
{
    lifecycle.add("reset");
    TestPlugin::reset();
}

/// Rejects premature AU restoration and records whether other formats were restored before activation.
void PreparationTestPlugin::setStateInformation(const void* data, const int sizeInBytes)
{
    lifecycle.add(initialized ? "restore-initialized" : "restore-inactive");
    if (!audioUnit || initialized) {
        TestPlugin::setStateInformation(data, sizeInBytes);
    }
}
}  // namespace audiobatch::tests
