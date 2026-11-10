/// Exercises ImageControl's real-time callbacks in an isolated, startup-linked RealtimeSanitizer executable.
/// Preparation, control changes, allocation, assertions, and thread lifecycle stay outside marked regions.
/// Each process runs one scenario so a fatal sanitizer diagnostic cannot hide another scenario's first callback.

#include "analysis/StereoSampleFifo.h"
#include "dsp/MultibandImager.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include <fmt/base.h>
#include <mach-o/dyld.h>
#include <string_view>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <pthread.h>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !__has_feature(realtime_sanitizer)
#error "ImageControlRealtimeTests must be compiled and linked with -fsanitize=realtime"
#endif
#if !__has_attribute(nonblocking)
#error "ImageControlRealtimeTests requires Clang's nonblocking attribute"
#endif

namespace esgrove::image_control::realtime
{
namespace
{
/// Prepared callback rate, independent of physical audio hardware.
constexpr double SAMPLE_RATE {48'000.0};
/// Includes empty blocks, subpartitions, nonmultiples, and a block larger than a partition.
constexpr std::array BLOCK_SIZES {0, 1, 37, 255, 256, 511, 1023};
/// Enough audio per phase to settle smoothing and fill the analyzer FIFO without a consumer.
constexpr int PHASE_SAMPLES {40'000};
/// The crossover worker must publish and the callback must consume a new slot within this bound.
constexpr auto HANDOFF_TIMEOUT {std::chrono::seconds(5)};

/// Fails outside the marked callback, including in release configurations where assertions are disabled.
void require(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

/// Runs a scenario on a new thread without preparing FFTs or calling process on that thread first.
void onFreshThread(const std::function<void()>& work)
{
    std::exception_ptr failure;
    std::thread thread([&] {
        try {
            work();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    thread.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

/// Calls malloc through an observable volatile function pointer so optimization cannot remove the control.
[[clang::noinline]] void* allocateControl() noexcept [[clang::nonblocking]]
{
    void* (*volatile allocate)(std::size_t) = &std::malloc;
    return allocate(64);
}

/// Frees storage allocated before the marked region, independently of the allocation control.
[[clang::noinline]] void freeControl(void* storage) noexcept [[clang::nonblocking]]
{
    void (*volatile deallocate)(void*) = &std::free;
    deallocate(storage);
}

/// Acquires and releases a native uncontended mutex to prove the runtime detects locking.
[[clang::noinline]] void mutexControl(pthread_mutex_t& mutex) noexcept [[clang::nonblocking]]
{
    pthread_mutex_lock(&mutex);
    pthread_mutex_unlock(&mutex);
}

/// Proves a marked region with only arithmetic runs cleanly under the same startup runtime.
[[clang::noinline]] int cleanControl(const int input) noexcept [[clang::nonblocking]]
{
    return input * 3 + 1;
}

/// Dispatches independent harness controls, with all setup and teardown outside the marked region.
void runControl(const std::string_view scenario)
{
    if (scenario == "control-clean") {
        int result = 0;
        onFreshThread([&] { result = cleanControl(7); });
        require(result == 22, "Clean control result changed");
    } else if (scenario == "control-allocate") {
        void* storage = nullptr;
        onFreshThread([&] { storage = allocateControl(); });
        std::free(storage);
    } else if (scenario == "control-free") {
        auto* storage = std::malloc(64);
        require(storage != nullptr, "Could not prepare the free control");
        onFreshThread([&] { freeControl(storage); });
    } else if (scenario == "control-mutex") {
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        onFreshThread([&] { mutexControl(mutex); });
        pthread_mutex_destroy(&mutex);
    } else {
        require(false, "Unknown harness control");
    }
}

/// Sets a direct processor parameter through the real host-facing API, never in a marked region.
void setPlain(ImageControlProcessor& processor, const juce::String& identifier, const float value)
{
    auto* parameter = processor.getParameters().getParameter(identifier);
    require(parameter != nullptr, "Missing direct processor parameter");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

/// Activates gain conversion, recovery, panning, Width, solos, and output gain before a cold callback.
void activate(ImageControlProcessor& processor)
{
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        setPlain(processor, midGainParameterId(band), -3.0F + static_cast<float>(band));
        setPlain(processor, sideGainParameterId(band), 2.0F);
        setPlain(processor, panParameterId(band), band % 2 == 0 ? -35.0F : 25.0F);
        setPlain(processor, widthParameterId(band), 0.25F);
        setPlain(processor, recoverParameterId(band), 75.0F);
    }
    setPlain(processor, soloParameterId(1), 1.0F);
    setPlain(processor, OUTPUT_GAIN_ID, -2.0F);
}

/// Fills preallocated storage with deterministic broadband input without warming any processor FFT state.
void fillInput(juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
            const int phase = (sample * (channel + 3) + channel * 17) % 101;
            buffer.setSample(channel, sample, static_cast<float>(phase - 50) * 0.005F);
        }
    }
}

/// Marks only the production processor callback, including the host-bypass callback when selected.
[[clang::noinline]] void processDirect(
    ImageControlProcessor& processor,
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midi,
    const bool bypassed
) noexcept [[clang::nonblocking]]
{
    if (bypassed) {
        processor.processBlockBypassed(buffer, midi);
    } else {
        processor.processBlock(buffer, midi);
    }
}

/// Checks finite output after leaving the marked region, without permitting a silent no-op harness.
void checkOutput(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
            require(std::isfinite(buffer.getSample(channel, sample)), "Callback returned nonfinite audio");
        }
    }
}

/// Processes enough irregular blocks to execute first-use FFT work, steady state, and FIFO-full drops.
void directPhase(ImageControlProcessor& processor, juce::AudioBuffer<float>& storage, const bool bypassed = false)
{
    juce::MidiBuffer midi;
    int samples = 0;
    std::size_t index = 0;
    while (samples < PHASE_SAMPLES) {
        const int count = BLOCK_SIZES[index++ % BLOCK_SIZES.size()];
        fillInput(storage);
        juce::AudioBuffer<float> block(storage.getArrayOfWritePointers(), storage.getNumChannels(), count);
        processDirect(processor, block, midi, bypassed);
        checkOutput(block);
        samples += count;
    }
}

/// Exercises neutral or active mono/stereo callbacks and transitions without marking parameter edits.
void runDirect(const std::string_view scenario)
{
    const bool mono = scenario.ends_with("mono");
    const bool active = scenario.starts_with("direct-active");
    const bool transitions = scenario == "direct-transitions";
    ImageControlProcessor processor;
    const auto layout = mono ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    require(
        processor.setBusesLayout({.inputBuses = {layout}, .outputBuses = {layout}}),
        "Processor rejected a supported layout"
    );
    processor.setNonRealtime(false);
    processor.setCorrelationEnabled(active || transitions);
    processor.prepareToPlay(SAMPLE_RATE, BLOCK_SIZES.back());
    if (active) {
        activate(processor);
    }
    juce::AudioBuffer<float> storage(mono ? 1 : 2, BLOCK_SIZES.back());
    std::vector<float> left(static_cast<std::size_t>(ANALYZER_FIFO_FRAMES));
    std::vector<float> right(left.size());
    onFreshThread([&] {
        directPhase(processor, storage);
        if (transitions) {
            activate(processor);
            directPhase(processor, storage);
            setPlain(processor, DELTA_ID, 1.0F);
            setPlain(processor, MONO_ID, 1.0F);
            processor.getAnalyzerFifo().setTap(false);
            directPhase(processor, storage);
            setPlain(processor, BYPASS_ID, 1.0F);
            directPhase(processor, storage);
            setPlain(processor, BYPASS_ID, 0.0F);
            setPlain(processor, DELTA_ID, 0.0F);
            setPlain(processor, MONO_ID, 0.0F);
            processor.getAnalyzerFifo().setTap(true);
            directPhase(processor, storage, true);
            directPhase(processor, storage);
        }
    });
    require(processor.getLatencySamples() > PARTITION_SIZE, "Processor was not prepared");
    require(processor.getAnalyzerFifo().pop(left, right) == ANALYZER_FIFO_FRAMES - 1, "FIFO-full path not reached");
    if (active || transitions) {
        const auto readings = processor.getBandCorrelations();
        require(
            std::ranges::any_of(readings, [](const float value) { return std::isfinite(value); }),
            "Enabled correlation meters did not publish"
        );
    }
    processor.releaseResources();
}

/// Publishes a real-time crossover request and processes its partition without marking the background designer.
[[clang::noinline]] void processHandoff(
    MultibandImager& imager,
    juce::AudioBuffer<float>& buffer,
    const Crossovers& crossovers
) noexcept [[clang::nonblocking]]
{
    imager.setCrossovers(crossovers, false);
    imager.process(buffer);
}

/// Proves the designer's spare slot became active, including crossfade arithmetic with nonneutral gains.
void runHandoff(const bool active)
{
    MultibandImager imager;
    imager.prepare(SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    if (active) {
        BandGains mid;
        mid.fill(0.7F);
        BandGains side;
        side.fill(0.4F);
        imager.setGains(mid, side, 0.9F);
    }
    constexpr Crossovers movedCrossovers {180.0F, 1200.0F, 6500.0F};
    juce::AudioBuffer<float> buffer(2, 511);
    onFreshThread([&] {
        const auto deadline = std::chrono::steady_clock::now() + HANDOFF_TIMEOUT;
        do {
            fillInput(buffer);
            processHandoff(imager, buffer, movedCrossovers);
            checkOutput(buffer);
            if (sameCrossovers(imager.getActiveCrossovers(), movedCrossovers)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < deadline);
        require(false, "Crossover request never became active");
    });
    require(sameCrossovers(imager.getActiveCrossovers(), movedCrossovers), "Active crossover proof failed");
    imager.release();
}

/// Rejects an absent runtime or two independently loaded runtimes before trusting hosted callback results.
void checkSingleRuntime()
{
    std::size_t runtimes = 0;
    for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
        const std::string_view path(_dyld_get_image_name(index));
        if (path.contains("libclang_rt.rtsan")) {
            ++runtimes;
        }
    }
    require(runtimes == 1, "Expected exactly one startup-linked RTSan runtime in the host and bundle process");
}

/// Marks the actual JUCE VST3 host callback, not an unwrapped ImageControl instance.
[[clang::noinline]] void processHosted(
    juce::AudioPluginInstance& instance,
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midi
) noexcept [[clang::nonblocking]]
{
    instance.processBlock(buffer, midi);
}

/// Loads only the exact build-tree VST3 and changes cached host parameters before its first fresh-thread callback.
void runHosted(const juce::String& pluginPath, const bool changed)
{
    require(pluginPath == juce::String(IMAGE_CONTROL_REALTIME_VST3_PATH), "Hosted test requires the exact built VST3");
    require(juce::File(pluginPath).isDirectory(), "Built VST3 bundle is missing");
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, pluginPath);
    require(descriptions.size() == 1, "Expected one ImageControl VST3 description");
    juce::String error;
    auto instance = format.createInstanceFromDescription(*descriptions[0], SAMPLE_RATE, BLOCK_SIZES.back(), error);
    require(instance != nullptr, "Could not create the exact build-tree VST3");
    instance->setNonRealtime(false);
    instance->prepareToPlay(SAMPLE_RATE, BLOCK_SIZES.back());
    if (changed) {
        bool found = false;
        for (auto* parameter : instance->getParameters()) {
            if (parameter->getName(100) == "Low Mid: Side") {
                parameter->setValueNotifyingHost(gainRange(BAND_GAIN_MIN_DB, BAND_GAIN_MAX_DB).convertTo0to1(-9.0F));
                found = true;
            }
        }
        require(found, "Hosted cached parameter was not changed");
    }
    checkSingleRuntime();
    juce::AudioBuffer<float> buffer(2, 511);
    juce::MidiBuffer midi;
    fillInput(buffer);
    onFreshThread([&] {
        processHosted(*instance, buffer, midi);
        checkOutput(buffer);
    });
    instance->releaseResources();
}
/// Dispatches one named scenario after GUI and runtime initialization, keeping all diagnostics process-isolated.
int run(const std::span<char*> arguments)
{
    std::string_view scenario;
    juce::String pluginPath;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view option(arguments[index]);
        require(index + 1 < arguments.size(), "Missing scenario argument value");
        if (option == "--case") {
            scenario = arguments[++index];
        } else if (option == "--plugin") {
            pluginPath = arguments[++index];
        } else {
            require(false, "Unknown scenario argument");
        }
    }
    require(!scenario.empty(), "Expected --case NAME");
    checkSingleRuntime();
    if (scenario.starts_with("control-")) {
        runControl(scenario);
    } else {
        const juce::ScopedJuceInitialiser_GUI initializeJuce;
        if (scenario == "direct-neutral-mono" || scenario == "direct-neutral-stereo" || scenario == "direct-active-mono"
            || scenario == "direct-active-stereo" || scenario == "direct-transitions")
        {
            runDirect(scenario);
        } else if (scenario == "imager-handoff-neutral" || scenario == "imager-handoff-active") {
            runHandoff(scenario == "imager-handoff-active");
        } else if (
            scenario == "hosted-first-callback" || scenario == "hosted-cached-parameters"
            || scenario == "hosted-first-callback-diagnostic" || scenario == "hosted-cached-parameters-diagnostic"
        )
        {
            runHosted(pluginPath, scenario.starts_with("hosted-cached-parameters"));
        } else {
            require(false, "Unknown real-time scenario");
        }
    }
    fmt::print("ImageControl RTSan scenario completed: {}\n", scenario);
    return 0;
}
}  // namespace
}  // namespace esgrove::image_control::realtime

/// Converts unmarked setup and assertion failures into an ordinary nonzero exit, distinct from sanitizer findings.
int main(const int argc, char* argv[])
{
    try {
        return esgrove::image_control::realtime::run(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception& error) {
        fmt::print(stderr, "ImageControl RTSan setup/assertion failure: {}\n", error.what());
        return 2;
    }
}
