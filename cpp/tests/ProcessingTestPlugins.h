/// Small plugin variants and processing-option helpers for the plugin processing and coordinator tests.
/// The reusable configurable effect and preparation fixture live in their own headers.

#pragma once

#include "plugins/PluginProcessing.h"
#include "TestPlugin.h"

#include <JuceHeader.h>

#include <vector>

namespace audiobatch::tests
{
/// Records its resource release and its destruction in a log owned by the test, which outlives the plugin.
class TeardownRecordingPlugin final : public TestPlugin
{
public:
    /// Writes the teardown events to the given log.
    explicit TeardownRecordingPlugin(juce::StringArray& eventLog) : TestPlugin(TestPluginSettings {}), events(eventLog)
    {}

    /// Records the destruction after any release.
    ~TeardownRecordingPlugin() override
    {
        events.add("destroy");
    }

    TeardownRecordingPlugin(const TeardownRecordingPlugin&) = delete;
    TeardownRecordingPlugin(TeardownRecordingPlugin&&) = delete;
    TeardownRecordingPlugin& operator=(const TeardownRecordingPlugin&) = delete;
    TeardownRecordingPlugin& operator=(TeardownRecordingPlugin&&) = delete;

    /// Records the release of the prepared resources.
    void releaseResources() override
    {
        events.add("release");
        TestPlugin::releaseResources();
    }

private:
    /// Teardown log owned by the test, which outlives the plugin.
    juce::StringArray& events;
};

/// Counts the change notifications of the processors it is registered on.
class CountingProcessorListener final : public juce::AudioProcessorListener
{
public:
    /// Counts a change notification.
    void audioProcessorChanged(juce::AudioProcessor* /*processor*/, const ChangeDetails& /*details*/) override
    {
        ++changeCount;
    }

    /// Ignores parameter changes, which the tests do not trigger.
    void audioProcessorParameterChanged(juce::AudioProcessor* /*processor*/, int /*index*/, float /*value*/) override {}

    /// Number of processor change notifications received.
    int changeCount {0};
};

/// The VST3 client of FailingProcessPlugin, which reports a fixed result for the last process call.
class FailingVST3Client final : public juce::AudioPluginExtensions::VST3Client
{
public:
    /// Has no preset.
    [[nodiscard]] juce::MemoryBlock getPreset() const override
    {
        return {};
    }

    /// Has no component, since the plugin is not a real VST3.
    [[nodiscard]] Steinberg::Vst::IComponent* getIComponentPtr() const noexcept override
    {
        return nullptr;
    }

    /// Accepts no preset.
    bool setPreset(const juce::MemoryBlock& /*preset*/) override
    {
        return false;
    }

    /// Reports the result a VST3 plugin returns when its process call fails.
    [[nodiscard]] int getLastProcessResult() const noexcept override
    {
        return FAILED_RESULT;
    }

    /// The result code reported, kResultFalse of the VST3 SDK.
    static constexpr int FAILED_RESULT {1};
};

/// A test plugin that looks like a VST3 plugin whose process calls fail, while its audio passes through.
class FailingProcessPlugin final : public TestPlugin
{
public:
    /// Creates a gain-only plugin.
    FailingProcessPlugin() : TestPlugin({}) {}

    /// Returns the client that reports the failed process call.
    juce::AudioPluginExtensions::VST3Client* getVST3Client() override
    {
        return &client;
    }

    /// Returns the client that reports the failed process call.
    const juce::AudioPluginExtensions::VST3Client* getVST3Client() const override
    {
        return &client;
    }

private:
    /// Exposes the failed process result to the host.
    FailingVST3Client client;
};

PluginProcessingOptions makeTestPluginOptions(
    const std::vector<juce::AudioPluginInstance*>& chain,
    const std::vector<juce::MemoryBlock>& states = {}
);

std::vector<juce::AudioPluginInstance*> chainOf(juce::AudioPluginInstance& plugin);
}  // namespace audiobatch::tests
