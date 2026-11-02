/// Test effect that records preparation and emulates Audio Unit initialization before state restoration.

#pragma once

#include "TestPlugin.h"

namespace audiobatch::tests
{
/// Records preparation order and emulates an Audio Unit that needs initialization before state restoration.
/// Its null native handle exercises the AU lifecycle without calling a real Audio Unit.
class PreparationTestPlugin final : public TestPlugin, public juce::AudioPluginExtensions::AudioUnitClient
{
public:
    explicit PreparationTestPlugin(bool emulateAudioUnit);

    juce::AudioPluginExtensions::AudioUnitClient* getAudioUnitClient() override;
    const juce::AudioPluginExtensions::AudioUnitClient* getAudioUnitClient() const override;

    AudioUnit getAudioUnitHandle() const noexcept override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    [[nodiscard]] const juce::StringArray& getLifecycle() const;

    [[nodiscard]] double getLastSampleRate() const;

    [[nodiscard]] int getPreparedBlockSize() const;

    void clearLifecycle();

private:
    /// Ordered host lifecycle calls, excluding audio rendering.
    juce::StringArray lifecycle;

    /// Advertises the Audio Unit lifecycle when enabled.
    bool audioUnit;
    /// True between preparation and resource release.
    bool initialized {false};

    /// Most recent preparation sample rate.
    double lastSampleRate {0.0};

    /// Most recent preparation block size, including editor-specific sizes.
    int preparedBlockSize {0};
};
}  // namespace audiobatch::tests
