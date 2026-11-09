/// Focused startup-linked RealtimeSanitizer scenarios for preview callbacks and their renderer.
/// Every marked entry executes on a fresh standard thread with buffers, decoding, and plugin preparation outside it.
/// Known JUCE and player lock findings are explicit baselines, not suppressions or realtime-safety claims.

#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/LiveChain.h"
#include "plugins/PluginChainPreparation.h"
#include "plugins/PluginChainPresets.h"
#include "plugins/PluginLibrary.h"
#include "plugins/PluginPreviewSource.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include <fmt/base.h>
#include <mach-o/dyld.h>
#include <string_view>

#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <latch>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if !__has_feature(realtime_sanitizer) || !__has_cpp_attribute(clang::nonblocking)
#error This executable requires RealtimeSanitizer instrumentation and the nonblocking attribute
#endif

namespace audiobatch::tests
{
namespace
{
constexpr double SAMPLE_RATE {48'000.0};
constexpr int BLOCK_SIZE {512};
constexpr int SOURCE_SAMPLES {1027};
constexpr int CALLBACK_LIMIT {16};

/// Keeps the controls observable without allocating in the clean callback.
struct ControlState {
    std::mutex mutex;
    void* allocation {nullptr};
    int result {0};
};

/// Throws only outside the realtime boundary so failed setup never masquerades as a sanitizer result.
void require(const bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

/// Verifies the marked scope itself is harmless.
[[gnu::noinline]] void realtimeClean(ControlState& state) [[clang::nonblocking]]
{
    state.result = 42;
}

/// Intentionally allocates as a positive interceptor control.
[[gnu::noinline]] void realtimeAllocation(ControlState& state) [[clang::nonblocking]]
{
    state.allocation = std::malloc(4096);
}

/// Intentionally frees storage prepared before the marked call.
[[gnu::noinline]] void realtimeFree(ControlState& state) [[clang::nonblocking]]
{
    std::free(state.allocation);
    state.allocation = nullptr;
}

/// Intentionally locks an uncontended mutex, which is forbidden even without contention.
[[gnu::noinline]] void realtimeMutex(ControlState& state) [[clang::nonblocking]]
{
    const std::scoped_lock lock(state.mutex);
    state.result = 42;
}

/// Marks the entire manager dispatch, including JUCE's callback locks and their destruction.
[[gnu::noinline]] void realtimeDeviceCallback(juce::AudioIODeviceCallback& callback, juce::AudioBuffer<float>& output)
    [[clang::nonblocking]]
{
    callback.audioDeviceIOCallbackWithContext(nullptr, 0, output.getArrayOfWritePointers(), 2, BLOCK_SIZE, {});
}

/// Marks the actual application callback through its forwarding seam, including try-lock destruction.
[[gnu::noinline]] void realtimePlayerCallback(AudioPreviewPlayer& player, juce::AudioBuffer<float>& output)
    [[clang::nonblocking]]
{
    player.renderTestCallback(output.getArrayOfWritePointers(), output.getNumChannels(), output.getNumSamples());
}

/// Marks only the real prepared renderer, allowing its contract to be distinguished from device hosting.
[[gnu::noinline]] void realtimeRendererCallback(
    PluginPreviewSource& renderer,
    juce::AudioBuffer<float>& output,
    const int samples
) [[clang::nonblocking]]
{
    renderer.getNextAudioBlock({&output, 0, samples});
}

/// Starts one fresh thread per invocation without marking thread startup, joining, or buffer ownership.
template<std::invocable Callback>
void onFreshThread(Callback callback)
{
    std::thread worker(std::move(callback));
    worker.join();
}

/// Runs one deliberate interceptor control. Cleanup is outside the marked call.
void runControl(const std::string_view name)
{
    ControlState state;
    if (name == "control-free") {
        state.allocation = std::malloc(4096);
        require(state.allocation != nullptr, "control allocation failed");
    }
    onFreshThread([&] {
        if (name == "control-clean") {
            realtimeClean(state);
        } else if (name == "control-allocation") {
            realtimeAllocation(state);
        } else if (name == "control-free") {
            realtimeFree(state);
        } else {
            realtimeMutex(state);
        }
    });
    std::free(state.allocation);
    if (name == "control-clean") {
        require(state.result == 42, "clean control did not execute");
    }
}

/// Rejects missing or duplicate runtimes before trusting the host and its loaded plugin bundle.
/// Runs outside the marked callback, after native plugin preparation as well as at process startup.
void checkSingleRuntime(const std::string_view phase)
{
    std::size_t runtimes = 0;
    for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
        const std::string_view path(_dyld_get_image_name(index));
        if (path.contains("libclang_rt.rtsan")) {
            ++runtimes;
        }
    }
    require(runtimes == 1, "expected exactly one startup-linked RTSan runtime in the host and plugin process");
    fmt::print("Realtime runtime count: {} {}\n", runtimes, phase);
    std::fflush(stdout);
}

/// Finds only the CMake-built test plugin, never scanning installed plugin locations.
juce::PluginDescription nativeDescription()
{
    const auto bundle = testPluginBundle();
    require(bundle.isDirectory(), "CMake-built test VST3 bundle is missing");
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, bundle.getFullPathName());
    require(descriptions.size() == 1, "test VST3 did not expose exactly one effect");
    return *descriptions[0];
}

/// Exercises active dry/wet dispatch or the uncontended idle try-lock destruction on a simulated driver.
/// Contention is owned by another thread so recursive locks cannot produce a false test pass.
void runPlayer(const std::string_view name)
{
    const TemporaryDirectory temporary;
    juce::ApplicationProperties properties;
    juce::PropertiesFile::Options settings;
    settings.applicationName = "RealtimePreviewTests";
    settings.filenameSuffix = ".xml";
    settings.folderName = temporary.getChildFile("settings").getFullPathName();
    settings.osxLibrarySubFolder = "Application Support";
    properties.setStorageParameters(settings);
    const bool wet = name.ends_with("wet");
    if (wet) {
        const std::array slots {SavedChainSlot {.description = nativeDescription(), .state = {}, .enabled = true}};
        properties.getUserSettings()->setValue("pluginChain", plugin_presets::createChainXml(slots)->toString());
    }
    TestAudioDeviceState device;
    AudioPreviewPlayer player;
    installTestAudioDevice(player.getDeviceManager(), device);
    PluginLibrary library(properties);
    LiveChain chain(library, properties, player.getRenderingLock(), player.getDeviceManager());
    player.setLiveChain(chain);
    if (wet) {
        require(player.setPluginPreviewEnabled(true), "live test VST3 preparation failed");
    }
    const bool idle = name == "player-idle";
    if (!idle) {
        require(player.load(juce::URL(fixtureFile("sine_stereo_44100_16.wav"))), "preview fixture failed to load");
        player.start();
        require(player.isPlaying() && device.callback != nullptr, "fake device did not start active playback");
        require(player.waitForReadAhead(4096, 2000), "initial read-ahead failed");
    }
    if (wet) {
        checkSingleRuntime("after plugin preparation");
    }
    juce::AudioBuffer<float> output(2, BLOCK_SIZE);
    output.clear();
    if (name == "player-contention") {
        for (int channel = 0; channel < output.getNumChannels(); ++channel) {
            juce::FloatVectorOperations::fill(output.getWritePointer(channel), 1.0f, BLOCK_SIZE);
        }
        const auto position = player.getCurrentPosition();
        std::latch locked(1);
        std::latch release(1);
        std::thread owner([&] {
            const juce::ScopedLock lock(player.getRenderingLock());
            locked.count_down();
            release.wait();
        });
        locked.wait();
        onFreshThread([&] { realtimePlayerCallback(player, output); });
        release.count_down();
        owner.join();
        require(output.getMagnitude(0, BLOCK_SIZE) < 1.0e-7f, "contended player did not clear every output");
        require(juce::approximatelyEqual(player.getCurrentPosition(), position), "contention advanced playback");
    } else if (name.starts_with("device-")) {
        onFreshThread([&] { realtimeDeviceCallback(*device.callback, output); });
    } else {
        onFreshThread([&] { realtimePlayerCallback(player, output); });
    }
    if (!idle && name != "player-contention") {
        require(output.getMagnitude(0, BLOCK_SIZE) > 0.001f, "active callback did not produce audio");
        require(player.getCurrentPosition() > 0.0, "active callback did not advance playback");
    }
    player.suspend();
    player.detachLiveChain();
    player.release();
}

/// Checks deterministic levels and block edges after leaving the realtime scope.
bool checkRendererBlock(
    const std::string_view name,
    const juce::AudioBuffer<float>& output,
    const int callbacks,
    const int count
)
{
    if (name == "renderer-latency-tail" && callbacks == 0) {
        require(output.getMagnitude(0, 37) < 1.0e-7f, "latency pre-roll was not silent");
    }
    if (name == "renderer-gain-ramp" && callbacks == 1) {
        require(output.getSample(0, 0) > output.getSample(0, count - 1), "wet gain did not ramp down");
        require(output.getSample(0, count - 1) > 0.03125f, "wet gain skipped its smoothing ramp");
    }
    if (name == "renderer-stop" && callbacks == 1) {
        require(output.getMagnitude(64, count - 64) < 1.0e-7f, "stop fade did not clear the block remainder");
    }
    if (name == "renderer-eof" && callbacks == 2) {
        require(output.getMagnitude(388, count - 388) < 1.0e-7f, "partial EOF block was not padded with silence");
    }
    if (callbacks != 0 || name == "renderer-failure" || name == "renderer-latency-tail" || name == "renderer-native") {
        return false;
    }
    auto expected = name == "renderer-dry" ? 0.25f : 0.125f;
    if (name == "renderer-mono-gain") {
        expected = 0.046875f;
    }
    require(std::abs(output.getSample(0, 100) - expected) < 1.0e-6f, "renderer gain/fold mismatch");
    require(std::abs(output.getSample(1, 100) - expected) < 1.0e-6f, "renderer stereo output mismatch");
    return true;
}

/// Verifies terminal state and fail-silent behavior without calling reporting APIs from the audio boundary.
void checkRendererFinished(
    const std::string_view name,
    PluginPreviewSource& renderer,
    const juce::AudioBuffer<float>& output,
    const bool heardAudio
)
{
    if (name == "renderer-failure") {
        require(renderer.hasFailed() && !renderer.takeEnded(), "invalid output did not fail playback");
        require(!heardAudio, "failed block exposed invalid audio");
        require(output.getMagnitude(0, BLOCK_SIZE) < 1.0e-7f, "failed output was not silent");
        require(renderer.describeFailure().isNotEmpty(), "failure description missing");
        return;
    }
    require(!renderer.hasFailed() && heardAudio, "renderer failed or never produced audio");
    if (name == "renderer-stop") {
        require(!renderer.isStopping() && !renderer.takeEnded(), "stop incorrectly reported EOF");
        return;
    }
    require(renderer.takeEnded() && !renderer.takeEnded(), "EOF was not delivered exactly once");
    require(std::abs(renderer.getCurrentPosition() - SOURCE_SAMPLES / SAMPLE_RATE) < 1.0e-9, "EOF position mismatch");
}

/// Creates a hosted effect without retaining a local plugin format beyond setup.
std::unique_ptr<juce::AudioPluginInstance> createNativeProcessor()
{
    juce::VST3PluginFormat format;
    auto instance = format.createInstanceFromDescription(nativeDescription(), SAMPLE_RATE, BLOCK_SIZE);
    require(instance != nullptr, "native preview VST3 instantiation failed");
    return instance;
}

/// Applies message-thread-equivalent transport and gain edits only between rendered blocks.
void updateRendererControls(const std::string_view name, PluginPreviewSource& renderer, const int callbacks)
{
    if (callbacks != 1) {
        return;
    }
    if (name == "renderer-gain-ramp") {
        renderer.setInputGain(LinearGain {.value = 0.25f});
    }
    if (name == "renderer-stop") {
        renderer.requestStop();
    }
}

/// Renders memory-backed audio through real dry/wet preparation, including latency, EOF, tail, and partial blocks.
/// Lifecycle transitions and gain changes happen between marked calls, never through a broad suppression.
void runRenderer(const std::string_view name)
{
    const bool dry = name == "renderer-dry";
    const bool failure = name == "renderer-failure";
    const bool mono = name == "renderer-mono-gain";
    const bool latency = name == "renderer-latency-tail";
    const bool native = name == "renderer-native";
    juce::AudioBuffer<float> samples(mono ? 1 : 2, SOURCE_SAMPLES);
    for (int channel = 0; channel < samples.getNumChannels(); ++channel) {
        juce::FloatVectorOperations::fill(samples.getWritePointer(channel), 0.25f, SOURCE_SAMPLES);
    }
    juce::MemoryAudioSource input(samples, false, false);
    TestPlugin effect({
        .gain = 0.5f,
        .rightGain = mono ? 0.5f : 1.0f,
        .producesInfinity = failure,
        .latencySamples = latency ? 37 : 0,
        .echoSamples = latency ? 83 : 0,
    });
    const auto hosted = native ? createNativeProcessor() : nullptr;
    auto& processor = native ? *hosted : static_cast<juce::AudioPluginInstance&>(effect);
    require(plugin_processing::configurePreviewChannels(processor), "preview buses could not be configured");
    processor.setNonRealtime(false);
    processor.setRateAndBufferSizeDetails(SAMPLE_RATE, BLOCK_SIZE);
    processor.prepareToPlay(SAMPLE_RATE, BLOCK_SIZE);
    checkSingleRuntime("after plugin preparation");
    plugin_processing::ChainPreparation preparation;
    plugin_processing::addToPreparation(preparation, processor, SAMPLE_RATE);
    PluginPreviewSource renderer;
    renderer.setInput(&input, SOURCE_SAMPLES / SAMPLE_RATE, mono ? 1 : 2);
    renderer.prepareToPlay(BLOCK_SIZE, SAMPLE_RATE);
    renderer.setChain(
        dry ? std::vector<juce::AudioPluginInstance*>() : std::vector {&processor}, preparation, !dry, true
    );
    renderer.setInputGain(LinearGain {.value = mono ? 0.5f : 1.0f}, true);
    renderer.start();
    juce::AudioBuffer<float> output(2, BLOCK_SIZE);
    bool heardAudio = false;
    bool heardTail = false;
    bool checkedLevel = false;
    int callbacks = 0;
    while ((renderer.isPlaying() || renderer.isStopping()) && callbacks < CALLBACK_LIMIT) {
        const int count = callbacks % 2 == 0 ? BLOCK_SIZE : 127;
        const auto positionBefore = input.getNextReadPosition();
        onFreshThread([&] { realtimeRendererCallback(renderer, output, count); });
        const auto magnitude = output.getMagnitude(0, count);
        heardAudio = heardAudio || magnitude > 0.001f;
        const auto tailOffset = static_cast<int>(juce::jmax<juce::int64>(0, SOURCE_SAMPLES - positionBefore));
        if (latency && tailOffset < count) {
            heardTail = heardTail || output.getMagnitude(tailOffset, count - tailOffset) > 0.001f;
        }
        checkedLevel = checkRendererBlock(name, output, callbacks, count) || checkedLevel;
        ++callbacks;
        updateRendererControls(name, renderer, callbacks);
    }
    require(callbacks < CALLBACK_LIMIT, "renderer exceeded bounded callback count");
    checkRendererFinished(name, renderer, output, heardAudio);
    require(failure || latency || native || checkedLevel, "renderer level was not checked");
    require(!latency || heardTail, "latency/tail scenario did not emit a tail block");
    output.clear();
    onFreshThread([&] { realtimeRendererCallback(renderer, output, 17); });
    require(output.getMagnitude(0, 17) < 1.0e-7f, "stopped renderer was not silent");
    renderer.releaseResources();
    const plugin_processing::ChainPreparation emptyPreparation;
    renderer.setChain({}, emptyPreparation, false, true);
    processor.releaseResources();
}

/// Dispatches one scenario without running the ordinary unit-test registry or touching audio hardware.
int runRealtimeScenario(const std::string_view name)
{
    if (name == "control-clean" || name == "control-allocation" || name == "control-free" || name == "control-mutex") {
        runControl(name);
    } else if (
        name == "device-dry" || name == "device-wet" || name == "player-dry" || name == "player-wet"
        || name == "player-idle" || name == "player-contention"
    )
    {
        runPlayer(name);
    } else if (
        name == "renderer-dry" || name == "renderer-wet" || name == "renderer-latency-tail"
        || name == "renderer-mono-gain" || name == "renderer-gain-ramp" || name == "renderer-failure"
        || name == "renderer-eof" || name == "renderer-stop" || name == "renderer-native"
    )
    {
        runRenderer(name);
    } else {
        throw std::runtime_error("unknown realtime scenario");
    }
    return 0;
}
}  // namespace
}  // namespace audiobatch::tests

/// Initializes the JUCE message thread outside the boundary and validates the dedicated case protocol.
int main(const int argc, char** argv)
{
    const std::span arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() != 3 || std::strcmp(arguments[1], "--case") != 0) {
        fmt::print(stderr, "Usage: AudioBatchRealtimeTests --case NAME\n");
        return 2;
    }
    try {
        const juce::ScopedJuceInitialiser_GUI initialiser;
        audiobatch::tests::checkSingleRuntime("at startup");
        const auto result = audiobatch::tests::runRealtimeScenario(arguments[2]);
        fmt::print("Realtime scenario {} completed\n", arguments[2]);
        return result;
    } catch (const std::exception& error) {
        fmt::print(stderr, "Realtime scenario setup/assertion failure: {}\n", error.what());
        return 2;
    }
}
