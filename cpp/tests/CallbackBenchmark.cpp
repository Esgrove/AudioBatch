/// Measures the complete production preview callback on one persistent, hardware-free callback thread.
/// Decoding, read-ahead admission, control operations, validation, and JSON serialization are outside callback timing.
/// CPU counters cover the entire measured phase, including paced sleeps and between-burst idle waits.

#include "CallbackBenchmark.h"

#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioPreviewPlayer.h"
#include "plugins/PluginChainPreparation.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "utils/CallbackAccess.h"
#include "utils/StringFormat.h"
#include <fmt/base.h>
#include <system_error>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if JUCE_MAC
#include <mach/kern_return.h>
#include <mach/message.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <sys/resource.h>

#include <pthread.h>
#elif JUCE_WINDOWS
#include <windows.h>
#endif

namespace audiobatch::tests
{
namespace
{
constexpr int DEFAULT_COUNT {256};
constexpr int DEFAULT_WARMUP {16};
constexpr int MAXIMUM_COUNT {100'000};
constexpr int MAXIMUM_BLOCK {2048};
constexpr int MAXIMUM_CAPTURE_FRAMES {16'000'000};
/// Leaves room for resampler history in the player's fixed 32768-source-frame read-ahead buffer.
constexpr int BURST_SOURCE_FRAMES {8192};
/// Frequent control changes remain represented even when the default workload uses very small blocks.
constexpr int CONTROL_BURST_CALLBACKS {16};
constexpr int RESAMPLER_LOOKAHEAD_FRAMES {64};
constexpr juce::uint32 READ_AHEAD_TIMEOUT_MILLISECONDS {2000};
constexpr double SOURCE_RATE {44'100.0};
constexpr float SILENCE_THRESHOLD {1.0e-8f};
constexpr float EFFECT_GAIN {0.5f};
constexpr std::uint64_t HASH_OFFSET {14'695'981'039'346'656'037ULL};
constexpr std::uint64_t HASH_PRIME {1'099'511'628'211ULL};
constexpr std::array CASES {"idle", "idle-ready", "dry", "fake-wet", "native-wet", "control-contention", "transitions"};

/// Explicit workload configuration. Smoke mode invokes the same callback without collecting timing or CPU data.
struct Options {
    juce::String caseName {"dry"};
    int rate {48'000};
    int block {127};
    int count {DEFAULT_COUNT};
    int warmup {DEFAULT_WARMUP};
    bool paced {true};
    bool smoke {false};
};

/// Preallocated observations for one callback, written only by the persistent callback thread.
struct CallbackSample {
    double seconds {};
    double latenessSeconds {};
    double positionBefore {};
    double positionAfter {};
    bool excluded {};
    bool transition {};
};

/// Shared burst command, synchronized by semaphores rather than polled by the idle callback thread.
struct CallbackWork {
    std::binary_semaphore requested {0};
    std::binary_semaphore completed {0};
    std::vector<CallbackSample> samples;
    juce::AudioBuffer<float> output;
    std::exception_ptr failure;
    double threadCpuSeconds {};
    int first {};
    int count {};
    bool beginPhase {};
    bool endPhase {};
    bool excluded {};
    bool transition {};
    bool stop {};
};

/// A control operation's elapsed duration and semantic name, retained outside callback timing.
struct TransitionSample {
    juce::String operation;
    double seconds {};
};

/// Output statistics accumulated only after the callback thread has joined.
struct AudioStatistics {
    std::uint64_t checksum {HASH_OFFSET};
    double energy {};
    int invalidSamples {};
};

/// Throws only outside callback timing, so failed setup cannot become a fast benchmark result.
void require(const bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

/// Reads CPU time consumed by this callback thread, not elapsed wall time or a sum of timed callback scopes.
double threadCpuSeconds()
{
#if JUCE_MAC
    std::array<int, THREAD_BASIC_INFO_COUNT> storage {};
    auto count = static_cast<mach_msg_type_number_t>(storage.size());
    require(
        thread_info(pthread_mach_thread_np(pthread_self()), THREAD_BASIC_INFO, storage.data(), &count) == KERN_SUCCESS,
        "Could not read callback thread CPU time"
    );
    const auto information = std::bit_cast<thread_basic_info_data_t>(storage);
    return static_cast<double>(information.user_time.seconds + information.system_time.seconds)
        + static_cast<double>(information.user_time.microseconds + information.system_time.microseconds) * 1.0e-6;
#elif JUCE_WINDOWS
    FILETIME creation {}, exit {}, kernel {}, user {};
    require(
        GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user) != 0,
        "Could not read callback thread CPU time"
    );
    ULARGE_INTEGER kernelTicks {}, userTicks {};
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    return static_cast<double>(kernelTicks.QuadPart + userTicks.QuadPart) * 1.0e-7;
#endif
}

/// Reads process CPU time across every thread, including the decoder and control thread during idle waits.
double processCpuSeconds()
{
#if JUCE_MAC
    rusage usage {};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "Could not read process CPU time");
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec)
        + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1.0e-6;
#elif JUCE_WINDOWS
    FILETIME creation {}, exit {}, kernel {}, user {};
    require(
        GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != 0, "Could not read process CPU time"
    );
    ULARGE_INTEGER kernelTicks {}, userTicks {};
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    return static_cast<double>(kernelTicks.QuadPart + userTicks.QuadPart) * 1.0e-7;
#endif
}

/// Parses whole decimal integers without accepting partial values or allocating unreasonable capture buffers.
int parseInteger(const juce::String& text, const int minimum, const int maximum)
{
    const auto value = text.toStdString();
    int result = 0;
    const auto* const end = std::to_address(value.end());
    const auto parsed = std::from_chars(value.data(), end, result);
    require(
        parsed.ec == std::errc() && parsed.ptr == end && result >= minimum && result <= maximum,
        "Invalid or out-of-range callback benchmark integer"
    );
    return result;
}

/// Identifies both silent workloads without changing the original not-ready idle case.
bool isIdle(const Options& options)
{
    return options.caseName == "idle" || options.caseName == "idle-ready";
}

/// Bounds each burst by available read-ahead source frames at the requested device rate.
int burstCallbacks(const Options& options)
{
    return std::max(1, static_cast<int>(BURST_SOURCE_FRAMES * options.rate / SOURCE_RATE) / options.block);
}

/// Caps alternating control bursts so the contention workload includes frequent admission changes.
int controlBurstCallbacks(const Options& options)
{
    return std::min(burstCallbacks(options), CONTROL_BURST_CALLBACKS);
}

/// Validates named options before opening a source or creating a worker.
Options parseOptions(const juce::StringArray& arguments)
{
    Options options;
    for (int index = 1; index < arguments.size(); ++index) {
        const auto& name = arguments[index];
        if (name == "--smoke") {
            options.smoke = true;
            continue;
        }
        require(index + 1 < arguments.size(), "Callback benchmark option is missing its value");
        const auto& value = arguments[++index];
        if (name == "--case") {
            options.caseName = value;
        } else if (name == "--rate") {
            options.rate = parseInteger(value, 44'100, 96'000);
            require(
                options.rate == 44'100 || options.rate == 48'000 || options.rate == 96'000,
                "Callback benchmark rates are 44100, 48000, and 96000"
            );
        } else if (name == "--block") {
            options.block = parseInteger(value, 1, MAXIMUM_BLOCK);
        } else if (name == "--count") {
            options.count = parseInteger(value, 1, MAXIMUM_COUNT);
        } else if (name == "--warmup") {
            options.warmup = parseInteger(value, 0, 1024);
        } else if (name == "--mode") {
            require(value == "paced" || value == "unpaced", "Callback benchmark mode must be paced or unpaced");
            options.paced = value == "paced";
        } else {
            throw std::runtime_error(utils::format("Unknown callback benchmark option {}", name).toStdString());
        }
    }
    require(
        std::ranges::any_of(CASES, [&](const auto* name) { return options.caseName == name; }),
        "Unknown callback benchmark case"
    );
    require(
        options.block <= MAXIMUM_CAPTURE_FRAMES / (options.count + options.warmup + 1),
        "Callback benchmark capture exceeds the bounded frame allocation"
    );
    require(
        options.caseName != "control-contention" || options.count > controlBurstCallbacks(options),
        "Control-contention count must exceed one control burst to measure both excluded and active callbacks"
    );
    return options;
}

/// Loads only a repository build-tree test plugin, permitting a frozen bundle for paired measurements.
std::unique_ptr<juce::AudioPluginInstance> createNativePlugin(const Options& options)
{
    const auto overridePath = juce::SystemStats::getEnvironmentVariable("AUDIOBATCH_CALLBACK_BENCHMARK_VST3", {});
    const auto bundle = overridePath.isEmpty() ? testPluginBundle() : juce::File(overridePath);
    const auto repository = juce::File(AUDIOBATCH_TEST_FIXTURES_DIR).getParentDirectory();
    require(
        bundle.isAChildOf(repository.getChildFile("cpp")) || bundle.isAChildOf(repository.getChildFile("test-plugin")),
        "Callback benchmark only accepts repository build-tree plugins"
    );
    require(bundle.isDirectory(), "Build-tree callback benchmark VST3 is missing");
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, bundle.getFullPathName());
    require(
        descriptions.size() == 1 && descriptions[0]->name == "AudioBatch Test MS",
        "Build-tree VST3 is not the expected test effect"
    );
    auto processor = format.createInstanceFromDescription(*descriptions[0], options.rate, options.block);
    require(processor != nullptr, "Could not instantiate build-tree callback benchmark VST3");
    const auto& parameters = processor->getParameters();
    require(parameters.size() >= 3 && parameters[0]->getName(32) == "Gain", "Unexpected native test parameters");
    parameters[0]->setValueNotifyingHost(EFFECT_GAIN * 0.5f);
    return processor;
}

/// Prepares a test-owned fake or native effect before attaching its non-owning pointer to the player.
void preparePlugin(juce::AudioPluginInstance& processor, const Options& options)
{
    require(plugin_processing::configurePreviewChannels(processor), "Could not configure preview test effect");
    processor.setNonRealtime(false);
    processor.setRateAndBufferSizeDetails(options.rate, options.block);
    processor.prepareToPlay(options.rate, options.block);
}

/// Executes all cold, warmup, and measured bursts on the same thread, which sleeps while awaiting work.
/// The timer covers renderTestCallback's entire call, including the production access guard's destructor.
void callbackThread(AudioPreviewPlayer& player, CallbackWork& work, const Options& options)
{
    double phaseCpuStart = 0.0;
    try {
        for (;;) {
            work.requested.acquire();
            if (work.stop) {
                return;
            }
            if (work.beginPhase && !options.smoke) {
                phaseCpuStart = threadCpuSeconds();
            }
            auto deadline = std::chrono::steady_clock::now();
            const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(static_cast<double>(options.block) / options.rate)
            );
            for (int offset = 0; offset < work.count; ++offset) {
                const auto index = work.first + offset;
                auto& sample = work.samples[static_cast<std::size_t>(index)];
                std::array outputs {
                    work.output.getWritePointer(0, index * options.block),
                    work.output.getWritePointer(1, index * options.block),
                };
                sample.excluded = work.excluded;
                sample.transition = work.transition && offset == 0;
                sample.positionBefore = player.getCurrentPosition();
                if (options.paced && !options.smoke) {
                    std::this_thread::sleep_until(deadline);
                }
                if (options.smoke) {
                    player.renderTestCallback(outputs.data(), 2, options.block);
                } else {
                    const auto started = std::chrono::steady_clock::now();
                    player.renderTestCallback(outputs.data(), 2, options.block);
                    const auto finished = std::chrono::steady_clock::now();
                    sample.seconds = std::chrono::duration<double>(finished - started).count();
                    sample.latenessSeconds = std::max(0.0, std::chrono::duration<double>(started - deadline).count());
                }
                sample.positionAfter = player.getCurrentPosition();
                deadline += period;
            }
            // Include the final device period so an idle phase measures idle CPU, even for a one-block burst.
            if (options.paced && !options.smoke) {
                std::this_thread::sleep_until(deadline);
            }
            if (work.endPhase && !options.smoke) {
                work.threadCpuSeconds = threadCpuSeconds() - phaseCpuStart;
            }
            work.completed.release();
        }
    } catch (...) {
        work.failure = std::current_exception();
        work.completed.release();
    }
}

/// Runs one bounded burst and propagates worker failures after it is quiescent.
void runBurst(
    CallbackWork& work,
    const int first,
    const int count,
    const bool excluded,
    const bool transition,
    const bool beginPhase,
    const bool endPhase
)
{
    work.first = first;
    work.count = count;
    work.excluded = excluded;
    work.transition = transition;
    work.beginPhase = beginPhase;
    work.endPhase = endPhase;
    work.requested.release();
    work.completed.acquire();
    if (work.failure != nullptr) {
        std::rethrow_exception(work.failure);
    }
}

/// Computes nearest-rank distributions from independent storage after the callback thread has exited.
double percentile(const std::vector<double>& sorted, const double quantile)
{
    if (sorted.empty()) {
        return 0.0;
    }
    const auto rank = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(sorted.size())));
    return sorted[std::max(std::size_t {1}, rank) - 1];
}

/// Hashes the exact committed input bytes outside measurement without requiring another linked JUCE module.
juce::String sourceIdentity()
{
    juce::MemoryBlock bytes;
    require(fixtureFile("sine_stereo_44100_16.wav").loadFileAsData(bytes), "Could not hash benchmark input");
    std::uint64_t hash = HASH_OFFSET;
    for (const auto byte : std::span(static_cast<const unsigned char*>(bytes.getData()), bytes.getSize())) {
        hash = (hash ^ byte) * HASH_PRIME;
    }
    return juce::String(hash);
}

/// Accumulates every output sample, detecting finite audio and a stable bitwise checksum without callback work.
int inspectAudioBlock(
    const juce::AudioBuffer<float>& output,
    const int start,
    const int frames,
    AudioStatistics& statistics
)
{
    int validFrames = 0;
    for (int frame = 0; frame < frames; ++frame) {
        bool nonSilent = false;
        for (int channel = 0; channel < 2; ++channel) {
            const auto value = output.getSample(channel, start + frame);
            statistics.invalidSamples += std::isfinite(value) ? 0 : 1;
            statistics.energy += static_cast<double>(value) * static_cast<double>(value);
            nonSilent = nonSilent || std::abs(value) > SILENCE_THRESHOLD;
            statistics.checksum = (statistics.checksum ^ std::bit_cast<std::uint32_t>(value)) * HASH_PRIME;
        }
        validFrames += nonSilent ? 1 : 0;
    }
    return validFrames;
}

/// Requires real input consumption for active workloads and no hidden consumption in either idle workload.
bool renderingValid(
    const Options& options,
    const juce::int64 expectedFrames,
    const juce::int64 bufferedFrames,
    const double energy
)
{
    return isIdle(options) ? expectedFrames == 0 && bufferedFrames == 0
                           : expectedFrames > 0 && bufferedFrames > 0 && energy > 0.0;
}

/// Labels readiness explicitly so the frozen idle shortcut cannot be confused with prepared renderer work.
void setReadinessProperties(juce::DynamicObject& object, const Options& options)
{
    object.setProperty("callback_readiness", options.caseName == "idle" ? "not-ready" : "ready");
    if (options.caseName == "idle") {
        object.setProperty(
            "semantic_note", "Frozen v1 idle is the not-ready device shortcut, not a prepared renderer."
        );
    } else if (options.caseName == "idle-ready") {
        object.setProperty("semantic_note", "Prepared device with released, stopped renderer and no buffered input.");
    }
}

/// Serializes raw samples and semantic audio checks after all callback and CPU measurements are complete.
juce::var makeReport(
    const Options& options,
    const CallbackWork& work,
    const std::vector<TransitionSample>& transitions,
    const double wallSeconds,
    const double processSeconds,
    const juce::int64 bufferedFrames,
    const bool sourceFailure
)
{
    const auto measuredFirst = options.warmup + 1;
    const auto period = static_cast<double>(options.block) / options.rate;
    std::vector<double> sorted;
    juce::Array<juce::var> raw;
    juce::Array<juce::var> lateness;
    juce::Array<juce::var> progress;
    juce::Array<juce::var> exclusions;
    juce::Array<juce::var> transitionCallbacks;
    juce::Array<juce::var> controlSamples;
    juce::int64 progressedFrames = 0;
    juce::int64 expectedFrames = 0;
    juce::int64 nonSilentFrames = 0;
    AudioStatistics audio;
    double callbackSeconds = 0.0;
    int silentBlocks = 0;
    int excludedBlocks = 0;
    int missedDeadlines = 0;
    int unexpectedSilentBlocks = 0;
    bool frameAccountingValid = true;
    for (int callback = measuredFirst; callback < measuredFirst + options.count; ++callback) {
        const auto& sample = work.samples[static_cast<std::size_t>(callback)];
        const auto delta
            = static_cast<juce::int64>(std::llround((sample.positionAfter - sample.positionBefore) * options.rate));
        const auto expected = isIdle(options) || sample.excluded ? 0 : options.block;
        frameAccountingValid = frameAccountingValid && delta == expected;
        expectedFrames += expected;
        progressedFrames += delta;
        excludedBlocks += sample.excluded ? 1 : 0;
        missedDeadlines += sample.seconds > period ? 1 : 0;
        callbackSeconds += sample.seconds;
        sorted.push_back(sample.seconds);
        raw.add(sample.seconds);
        lateness.add(sample.latenessSeconds);
        progress.add(delta);
        exclusions.add(sample.excluded);
        transitionCallbacks.add(sample.transition);
        const auto validFrames = inspectAudioBlock(work.output, callback * options.block, options.block, audio);
        nonSilentFrames += validFrames;
        silentBlocks += validFrames == 0 ? 1 : 0;
        unexpectedSilentBlocks += expected > 0 && validFrames == 0 ? 1 : 0;
        frameAccountingValid = frameAccountingValid && (expected > 0 || validFrames == 0);
    }
    for (const auto& transition : transitions) {
        const juce::var item(new juce::DynamicObject());
        item.getDynamicObject()->setProperty("operation", transition.operation);
        item.getDynamicObject()->setProperty("seconds", transition.seconds);
        controlSamples.add(item);
    }
    std::ranges::sort(sorted);
    const bool contentionValid
        = options.caseName != "control-contention" || (excludedBlocks > 0 && excludedBlocks < options.count);
    const auto valid = frameAccountingValid && renderingValid(options, expectedFrames, bufferedFrames, audio.energy)
        && contentionValid && !sourceFailure && audio.invalidSamples == 0 && unexpectedSilentBlocks == 0;
    juce::var report(new juce::DynamicObject());
    auto& object = *report.getDynamicObject();
    object.setProperty("schema_version", 1);
    object.setProperty("implementation", "cpp");
    object.setProperty("case", options.caseName);
    setReadinessProperties(object, options);
    object.setProperty("mode", options.paced ? "paced" : "unpaced");
    object.setProperty("smoke", options.smoke);
    object.setProperty("timing_valid", !options.smoke);
    object.setProperty("sample_rate", options.rate);
    object.setProperty("source_sample_rate", SOURCE_RATE);
    object.setProperty("source_fixture", "sine_stereo_44100_16.wav");
    object.setProperty("source_identity_fnv1a64", sourceIdentity());
    object.setProperty("block_frames", options.block);
    object.setProperty("channels", 2);
    object.setProperty("callback_count", options.count);
    object.setProperty("warmup_count", options.warmup);
    object.setProperty("cold_callback_seconds", options.smoke ? juce::var() : juce::var(work.samples.front().seconds));
    object.setProperty("callback_seconds", raw);
    object.setProperty("callback_lateness_seconds", lateness);
    object.setProperty("callback_source_progress_frames", progress);
    object.setProperty("callback_excluded", exclusions);
    object.setProperty("callback_after_transition", transitionCallbacks);
    object.setProperty("control_transitions", controlSamples);
    object.setProperty("p50_seconds", percentile(sorted, 0.50));
    object.setProperty("p95_seconds", percentile(sorted, 0.95));
    object.setProperty("p99_seconds", percentile(sorted, 0.99));
    object.setProperty("max_seconds", sorted.empty() ? 0.0 : sorted.back());
    object.setProperty("maximum_deadline_ratio", sorted.empty() ? 0.0 : sorted.back() / period);
    object.setProperty("deadline_misses", missedDeadlines);
    object.setProperty("phase_wall_seconds", wallSeconds);
    object.setProperty("callback_thread_cpu_seconds", work.threadCpuSeconds);
    object.setProperty("process_cpu_seconds", processSeconds);
    object.setProperty("requested_audio_seconds", static_cast<double>(options.count * options.block) / options.rate);
    object.setProperty("callback_seconds_per_audio_second", callbackSeconds / (options.count * period));
    object.setProperty("expected_source_frames", expectedFrames);
    object.setProperty("source_progress_frames", progressedFrames);
    object.setProperty("buffered_source_frames_consumed", bufferedFrames);
    object.setProperty("non_silent_frames", nonSilentFrames);
    object.setProperty("silent_blocks", silentBlocks);
    object.setProperty("excluded_blocks", excludedBlocks);
    object.setProperty("unexpected_silent_blocks", unexpectedSilentBlocks);
    object.setProperty("invalid_samples", audio.invalidSamples);
    object.setProperty("source_failure", sourceFailure);
    object.setProperty("output_energy", audio.energy);
    object.setProperty("output_checksum", juce::String(audio.checksum));
    object.setProperty("valid", valid);
    object.setProperty("starvation_policy", "C++ reports/stops on a total buffering miss. No policy override.");
    object.setProperty("prefill_policy", "Bounded bursts prefetched off callback, rewound between bursts before EOF.");
    return report;
}

/// Applies one real control transition between bursts, recording lifecycle latency separately from rendering.
TransitionSample applyTransition(
    AudioPreviewPlayer& player,
    juce::AudioPluginInstance& processor,
    const Options& options,
    const int burst
)
{
    const auto started = std::chrono::steady_clock::now();
    juce::String operation;
    switch (burst % 4) {
        case 0:
            operation = "insert-detach-reattach";
            player.setTestCallbackChain({});
            player.setTestCallbackChain({&processor});
            break;
        case 1:
            operation = "gain-edit";
            player.setInputGain(LinearGain {.value = 0.75f});
            break;
        case 2:
            operation = "stream-close-reopen";
            player.suspend();
            player.start();
            player.setTestCallbackChain({&processor});
            break;
        default:
            operation = "source-release-reload";
            player.release();
            require(player.load(juce::URL(fixtureFile("sine_stereo_44100_16.wav"))), "Transition reload failed");
            player.start();
            player.setTestCallbackChain({&processor});
            break;
    }
    const auto seconds
        = options.smoke ? 0.0 : std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return {.operation = operation, .seconds = seconds};
}

/// Loads the committed source and attaches the prepared effect after the fake device has established its rate.
void loadAndStart(AudioPreviewPlayer& player, juce::AudioPluginInstance& processor, const bool wet)
{
    require(
        player.load(juce::URL(fixtureFile("sine_stereo_44100_16.wav"))), "Callback benchmark fixture failed to load"
    );
    player.start();
    require(player.isPlaying(), "Fake callback benchmark device failed to start");
    if (wet) {
        player.setTestCallbackChain({&processor});
    }
}

/// Admits a bounded burst only after its real source frames are buffered, leaving starvation policy unchanged.
void prefill(AudioPreviewPlayer& player, const Options& options, const int count)
{
    if (isIdle(options)) {
        return;
    }
    const auto frames
        = static_cast<int>(std::ceil(static_cast<double>(count * options.block) * SOURCE_RATE / options.rate))
        + RESAMPLER_LOOKAHEAD_FRAMES;
    require(player.waitForReadAhead(frames, READ_AHEAD_TIMEOUT_MILLISECONDS), "Callback benchmark read-ahead failed");
}

/// Prepares the real device once, releasing every buffered input before the ready-idle callback begins.
void preparePlayer(
    AudioPreviewPlayer& player,
    juce::AudioPluginInstance& processor,
    const TestAudioDeviceState& device,
    const Options& options,
    const bool wet
)
{
    if (options.caseName == "idle") {
        require(!player.getOutputStatus().open && device.callback == nullptr, "Not-ready idle opened its device");
        return;
    }
    loadAndStart(player, processor, wet);
    require(device.callback != nullptr && player.getOutputStatus().open, "Benchmark device was not prepared");
    if (isIdle(options)) {
        // release() leaves the prepared device ready but removes every buffering/resampling source.
        // No callback in this case may enter JUCE buffering or consume source frames.
        player.release();
        require(
            !player.hasLoadedSource() && !player.isPlaying() && !player.isStopping() && player.getOutputStatus().open
                && device.callback != nullptr,
            "Ready-idle must retain the prepared device with no active renderer or buffered source"
        );
    }
}

/// Runs one workload without user storage, physical hardware, or installed plugins.
juce::var benchmark(const Options& options)
{
    const bool idle = isIdle(options);
    const bool native = options.caseName == "native-wet";
    const bool wet = native || options.caseName == "fake-wet" || options.caseName == "control-contention"
        || options.caseName == "transitions";
    TestAudioDeviceState device;
    device.sampleRate = options.rate;
    device.blockSize = options.block;
    TestPlugin fake({.gain = EFFECT_GAIN});
    auto hosted = native ? createNativePlugin(options) : nullptr;
    auto& processor = native ? *hosted : static_cast<juce::AudioPluginInstance&>(fake);
    preparePlugin(processor, options);
    AudioPreviewPlayer player;
    installTestAudioDevice(player.getDeviceManager(), device);
    preparePlayer(player, processor, device, options, wet);
    CallbackWork work;
    const auto totalCount = options.count + options.warmup + 1;
    work.samples.resize(static_cast<std::size_t>(totalCount));
    work.output.setSize(2, totalCount * options.block);
    work.output.clear();
    std::vector<TransitionSample> transitions;
    const auto burstCount = burstCallbacks(options);
    std::thread worker([&] { callbackThread(player, work, options); });
    const auto stopWorker = [&] {
        if (work.failure == nullptr) {
            work.stop = true;
            work.requested.release();
        }
        worker.join();
    };
    double wallSeconds = 0.0;
    double processSeconds = 0.0;
    juce::int64 bufferedFrames = 0;
    bool sourceFailure = false;
    try {
        prefill(player, options, 1);
        runBurst(work, 0, 1, false, false, false, false);
        // Warmup is not timed as part of the phase and is independently prefetched in bounded bursts.
        for (int first = 1; first <= options.warmup;) {
            const auto count = std::min(burstCount, options.warmup + 1 - first);
            if (!idle) {
                player.setPosition(0.0);
            }
            prefill(player, options, count);
            runBurst(work, first, count, false, false, false, false);
            first += count;
        }
        runBurst(work, 0, 0, false, false, true, false);
        const auto processStart = options.smoke ? 0.0 : processCpuSeconds();
        const auto phaseStart = std::chrono::steady_clock::now();
        int burst = 0;
        for (int offset = 0; offset < options.count; ++burst) {
            const bool transition = options.caseName == "transitions";
            const auto controlBurst
                = transition || options.caseName == "control-contention" ? controlBurstCallbacks(options) : burstCount;
            const auto count = std::min(controlBurst, options.count - offset);
            const bool excluded = options.caseName == "control-contention" && burst % 2 == 0;
            if (transition) {
                transitions.push_back(applyTransition(player, processor, options, burst));
            }
            if (!idle) {
                player.setPosition(0.0);
            }
            prefill(player, options, count);
            const auto before = player.getTestCallbackProgress();
            if (excluded) {
                // Main owns the lock, proving the callback cannot succeed recursively on its own thread.
                const utils::CallbackControlGuard control(player.getRenderingAccess());
                runBurst(work, options.warmup + 1 + offset, count, true, transition, false, false);
            } else {
                runBurst(work, options.warmup + 1 + offset, count, false, transition, false, false);
            }
            const auto after = player.getTestCallbackProgress();
            bufferedFrames += after.bufferedReadPosition - before.bufferedReadPosition;
            sourceFailure = sourceFailure || after.bufferUnderrun || after.readFailed || after.rendererFailed;
            offset += count;
        }
        runBurst(work, 0, 0, false, false, false, true);
        if (!options.smoke) {
            wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - phaseStart).count();
            processSeconds = processCpuSeconds() - processStart;
        }
        stopWorker();
    } catch (...) {
        stopWorker();
        throw;
    }
    player.suspend();
    player.setTestCallbackChain({});
    player.release();
    processor.releaseResources();
    return makeReport(options, work, transitions, wallSeconds, processSeconds, bufferedFrames, sourceFailure);
}

/// Keeps both idle meanings distinct and verifies their real callbacks consume no source or produce audio.
void testIdleReadinessAndSmoke(juce::UnitTest& test)
{
    test.beginTest("Callback benchmark idle readiness and smoke metadata preserve their semantic contracts");
    for (const auto* name : {"idle", "idle-ready"}) {
        const auto report
            = benchmark(Options {.caseName = name, .count = 3, .warmup = 1, .paced = false, .smoke = true});
        test.expect(static_cast<bool>(report["valid"]));
        test.expectEquals(
            report["callback_readiness"].toString(), juce::String(juce::String(name) == "idle" ? "not-ready" : "ready")
        );
        test.expect(report["semantic_note"].toString().isNotEmpty());
        test.expect(static_cast<bool>(report["smoke"]));
        test.expect(!static_cast<bool>(report["timing_valid"]));
        test.expect(report["cold_callback_seconds"].isVoid());
        for (const auto* property :
             {
                 "expected_source_frames",
                 "source_progress_frames",
                 "buffered_source_frames_consumed",
                 "non_silent_frames",
             })
        {
            test.expectEquals(static_cast<juce::int64>(report[property]), juce::int64 {0});
        }
        test.expectEquals(static_cast<int>(report["silent_blocks"]), 3);
        const auto* seconds = report["callback_seconds"].getArray();
        test.expect(seconds != nullptr);
        if (seconds != nullptr) {
            for (const auto& value : *seconds) {
                test.expectWithinAbsoluteError(static_cast<double>(value), 0.0, 0.0);
            }
        }
    }
}

/// Converts parser rejection into a deterministic contract assertion without launching any timed workload.
bool optionsRejected(const juce::StringArray& arguments)
{
    try {
        juce::ignoreUnused(parseOptions(arguments));
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

/// Rejects counts that never reach the first active burst, including large-block read-ahead-limited bursts.
void testContentionMinimumCount(juce::UnitTest& test)
{
    test.beginTest("Callback benchmark contention rejects counts without both active and excluded callbacks");
    for (const auto rate : {44'100, 48'000, 96'000}) {
        for (const auto block : {1, 127, 2048}) {
            const auto minimum = std::min(16, static_cast<int>(8192.0 * rate / 44'100.0) / block) + 1;
            juce::StringArray arguments {
                "--callback-benchmark",
                "--case",
                "control-contention",
                "--rate",
                juce::String(rate),
                "--block",
                juce::String(block),
                "--count",
                juce::String(minimum - 1),
                "--smoke",
            };
            test.expect(optionsRejected(arguments));
            arguments.set(8, juce::String(minimum));
            test.expect(!optionsRejected(arguments));
            arguments.set(8, "1");
            test.expect(optionsRejected(arguments));
        }
    }
}

/// Uses untimed real-source rendering to prove contention contains both silent exclusions and successful audio.
void testActiveSmokeRequiresRendering(juce::UnitTest& test)
{
    test.beginTest("Callback benchmark active workloads must render even when some callbacks are excluded");
    for (const auto* name : {"dry", "fake-wet", "control-contention"}) {
        const auto report
            = benchmark(Options {.caseName = name, .count = 17, .warmup = 0, .paced = false, .smoke = true});
        test.expect(static_cast<bool>(report["valid"]));
        test.expectEquals(report["callback_readiness"].toString(), juce::String("ready"));
        test.expect(static_cast<juce::int64>(report["expected_source_frames"]) > 0);
        test.expect(static_cast<juce::int64>(report["buffered_source_frames_consumed"]) > 0);
        test.expect(static_cast<juce::int64>(report["non_silent_frames"]) > 0);
        if (juce::String(name) == "control-contention") {
            test.expectEquals(static_cast<int>(report["excluded_blocks"]), 16);
            test.expectEquals(static_cast<juce::int64>(report["expected_source_frames"]), juce::int64 {127});
            test.expectEquals(static_cast<juce::int64>(report["source_progress_frames"]), juce::int64 {127});
        }
    }

    // A report must reject a broken workload independently of command-line admission checks.
    const Options options {.caseName = "control-contention", .count = 17, .warmup = 0, .smoke = true};
    CallbackWork empty;
    empty.samples.resize(18);
    empty.output.setSize(2, 18 * options.block);
    empty.output.clear();
    for (auto& sample : empty.samples) {
        sample.excluded = true;
    }
    const auto report = makeReport(options, empty, {}, 0.0, 0.0, 0, false);
    test.expect(!static_cast<bool>(report["valid"]));
}
}  // namespace

/// Runs deterministic benchmark contracts in the existing unit suite, with every real callback in untimed smoke mode.
void runCallbackBenchmarkContractTests(juce::UnitTest& test)
{
    testIdleReadinessAndSmoke(test);
    testContentionMinimumCount(test);
    testActiveSmokeRequiresRendering(test);
}

/// Runs only the explicit callback mode and prints the stable marker used by the comparison runner.
int runCallbackBenchmark(const juce::StringArray& arguments)
{
    if (arguments.contains("--help")) {
        fmt::print(
            "AudioBatchTests --callback-benchmark "
            "[--case idle|idle-ready|dry|fake-wet|native-wet|control-contention|transitions] "
            "[--rate 44100|48000|96000] [--block 127] [--count 256] [--warmup 16] [--mode paced|unpaced] [--smoke]\n"
            "idle preserves the frozen v1 not-ready device shortcut. "
            "idle-ready retains a prepared device with the renderer stopped and its input released.\n"
            "control-contention requires count > min(16, floor(floor(8192 * rate / 44100) / block)) "
            "so measured callbacks include both exclusion and successful active rendering.\n"
            "--smoke checks semantic contracts only, sets smoke=true and timing_valid=false, "
            "and must never be used as performance measurements.\n"
        );
        return 0;
    }
    try {
        const auto report = benchmark(parseOptions(arguments));
        fmt::print("CALLBACK_BENCHMARK_JSON={}\n", juce::JSON::toString(report, true).toStdString());
        return static_cast<bool>(report.getProperty("valid", false)) ? 0 : 1;
    } catch (const std::exception& error) {
        fmt::print(stderr, "Callback benchmark failed: {}\n", error.what());
        return 2;
    }
}
}  // namespace audiobatch::tests
