/// Measures the real AudioBatch Test MS VST3 with decoded audio and preallocated buffers.
/// Plugin loading, preparation, state restoration, decoding, and serialization stay outside timing.
/// Both host and production-chain paths include input/output transfers, with finite checks in the chain path.

#include "PluginBenchmark.h"

#include "plugins/PluginChainPreparation.h"
#include "plugins/PluginProcessing.h"
#include "utils/AudioFormatUtils.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"
#include <fmt/base.h>
#include <system_error>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Repeated source samples and their rate, prepared before the timed processing.
struct BenchmarkInput {
    juce::AudioBuffer<float> samples;
    int sampleRate {};
};

/// Parameters whose arithmetic is independently reproduced by the comparison runner.
constexpr float benchmarkGain {0.5f};
constexpr float benchmarkWidth {0.5f};
/// Maximum decoded workload, preventing unreasonable test-harness allocations.
constexpr int maximumFrames {32'000'000};

/// Parses a complete bounded decimal integer, refusing missing or malformed values.
int boundedInteger(const juce::String& text, const int minimum, const int maximum)
{
    const auto value = text.toStdString();
    const auto* const end = std::to_address(value.end());
    int result = 0;
    const auto parsed = std::from_chars(value.data(), end, result);
    if (parsed.ec != std::errc() || parsed.ptr != end || result < minimum || result > maximum) {
        throw std::runtime_error(utils::format("Invalid benchmark integer {}", text.quoted()).toStdString());
    }
    return result;
}

/// Writes planar little-endian binary32 samples after measurement, checking every output failure.
void writeSamples(
    const juce::File& destination,
    const juce::AudioBuffer<float>& samples,
    const int first,
    const int frames
)
{
    const auto stream = destination.createOutputStream();
    if (stream == nullptr || !stream->setPosition(0) || !stream->truncate().wasOk()) {
        throw std::runtime_error("Could not open benchmark sample output");
    }
    for (int channel = 0; channel < samples.getNumChannels(); ++channel) {
        const std::span plane(samples.getReadPointer(channel, first), static_cast<std::size_t>(frames));
        for (const auto sample : plane) {
            if (!stream->writeFloat(sample)) {
                throw std::runtime_error("Could not write benchmark samples");
            }
        }
    }
    stream->flush();
    if (stream->getStatus().failed()) {
        throw std::runtime_error("Could not flush benchmark samples");
    }
}

/// Decodes and repeats a bounded mono/stereo source without processing or timed allocations.
BenchmarkInput loadInput(const juce::File& source, const int repeats)
{
    const auto reader = utils::createAudioReader(source);
    if (reader == nullptr || reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples <= 0
        || reader->lengthInSamples > maximumFrames / repeats || !std::isfinite(reader->sampleRate)
        || reader->sampleRate < 16.0 || reader->sampleRate > 2'822'400.0)
    {
        throw std::runtime_error("Benchmark needs bounded mono/stereo audio at a supported sample rate");
    }
    const auto channels = static_cast<int>(reader->numChannels);
    const auto sourceFrames = static_cast<int>(reader->lengthInSamples);
    const auto frames = sourceFrames * repeats;
    juce::AudioBuffer<float> decoded(channels, sourceFrames);
    if (!utils::readFloatSamples(*reader, utils::writePointers(decoded), 0, sourceFrames)
        || !isBlockFinite(decoded, sourceFrames))
    {
        throw std::runtime_error("Benchmark input could not be decoded completely into finite samples");
    }
    juce::AudioBuffer<float> input(channels, frames);
    for (int repeat = 0; repeat < repeats; ++repeat) {
        for (int channel = 0; channel < channels; ++channel) {
            input.copyFrom(channel, repeat * sourceFrames, decoded.getReadPointer(channel), sourceFrames);
        }
    }
    return {.samples = std::move(input), .sampleRate = juce::roundToInt(reader->sampleRate)};
}

/// Runs the timed host or production-chain loop, including transfers and the chain's finite checks.
void render(
    const juce::AudioBuffer<float>& input,
    juce::AudioBuffer<float>& output,
    juce::AudioBuffer<float>& scratch,
    const std::vector<juce::AudioPluginInstance*>& instances,
    juce::MidiBuffer& midi,
    const bool hostOnly
)
{
    constexpr auto block = plugin_processing::processingBlockSize;
    const auto channels = input.getNumChannels();
    const auto frames = input.getNumSamples();
    for (int position = 0; position < output.getNumSamples(); position += block) {
        scratch.clear();
        const auto available = std::min(block, std::max(0, frames - position));
        for (int channel = 0; channel < channels; ++channel) {
            if (available > 0) {
                scratch.copyFrom(channel, 0, input, channel, position, available);
            }
        }
        if (hostOnly) {
            midi.clear();
            instances.front()->processBlock(scratch, midi);
        } else {
            static_cast<void>(processBlockThroughChain(scratch, instances, channels, midi));
            if (!isBlockFinite(scratch, block)) {
                throw std::runtime_error("Test plugin produced invalid samples");
            }
        }
        for (int channel = 0; channel < channels; ++channel) {
            output.copyFrom(channel, position, scratch, channel, 0, block);
        }
    }
}

/// Sets the three known test parameters without rejecting additional host-exposed parameters such as bypass.
void setBenchmarkParameters(const juce::AudioPluginInstance& plugin, const int latency)
{
    const auto& parameters = plugin.getParameters();
    if (parameters.size() < 3 || parameters[0]->getName(32) != "Gain" || parameters[1]->getName(32) != "Width"
        || parameters[2]->getName(32) != "Latency")
    {
        throw std::runtime_error("Test plugin is missing the expected Gain, Width, or Latency parameters");
    }
    parameters[0]->setValueNotifyingHost(benchmarkGain * 0.5f);
    parameters[1]->setValueNotifyingHost(benchmarkWidth * 0.5f);
    parameters[2]->setValueNotifyingHost(static_cast<float>(latency) / 1024.0f);
}

/// Captures the warmup output or checks that a restored render retains every sample.
void verifyRepeat(const juce::AudioBuffer<float>& output, juce::AudioBuffer<float>& reference, const bool capture)
{
    const auto frames = output.getNumSamples();
    for (int channel = 0; channel < output.getNumChannels(); ++channel) {
        if (capture) {
            reference.copyFrom(channel, 0, output, channel, 0, frames);
        } else if (!std::ranges::equal(
                       std::span(output.getReadPointer(channel), static_cast<std::size_t>(frames)),
                       std::span(reference.getReadPointer(channel), static_cast<std::size_t>(frames))
                   ))
        {
            throw std::runtime_error("Repeated renders differ after state restoration");
        }
    }
}

/// Loads and repeats the source once, then benchmarks fresh restored instances without file I/O.
juce::var benchmark(const juce::StringArray& arguments)
{
    if (arguments.size() != 9 || arguments[0] != "--vst3-benchmark") {
        throw std::runtime_error(
            "Usage: AudioBatchTests --vst3-benchmark BUNDLE INPUT OUTPUT_PREFIX host|chain INSTANCES LATENCY "
            "ITERATIONS REPEAT"
        );
    }
    const auto hostOnly = arguments[4] == "host";
    if (!hostOnly && arguments[4] != "chain") {
        throw std::runtime_error("Benchmark mode must be host or chain");
    }
    const auto instanceCount = boundedInteger(arguments[5], 1, 8);
    const auto latencyPerPlugin = boundedInteger(arguments[6], 0, 1024);
    const auto iterations = boundedInteger(arguments[7], 1, 1000);
    const auto repeats = boundedInteger(arguments[8], 1, 1000);
    if (hostOnly && instanceCount != 1) {
        throw std::runtime_error("Host mode requires one plugin instance");
    }
    const juce::File bundle(arguments[1]);
    const juce::File prefix(arguments[3]);
    const auto loaded = loadInput(juce::File(arguments[2]), repeats);
    const auto& input = loaded.samples;
    const auto channels = input.getNumChannels();
    const auto frames = input.getNumSamples();
    const auto rate = loaded.sampleRate;
    if (channels < 1 || channels > 2) {
        throw std::runtime_error("Benchmark input must retain its mono/stereo channel layout");
    }
    juce::AudioPluginFormatManager formats;
    formats.addFormat(std::make_unique<juce::VST3PluginFormat>());
    juce::OwnedArray<juce::PluginDescription> descriptions;
    formats.getFormat(0)->findAllTypesForFile(descriptions, bundle.getFullPathName());
    const auto* const description
        = std::ranges::find_if(descriptions, [](const auto* item) { return item->name == "AudioBatch Test MS"; });
    if (description == descriptions.end()) {
        throw std::runtime_error("Bundle does not contain AudioBatch Test MS");
    }
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> plugins;
    std::vector<juce::AudioPluginInstance*> instances;
    PluginProcessingOptions options;
    juce::MidiBuffer midi;
    midi.ensureSize(2048);
    juce::AudioBuffer<float> scratch(channels, plugin_processing::processingBlockSize);
    for (int instance = 0; instance < instanceCount; ++instance) {
        juce::String error;
        auto plugin = formats.createPluginInstance(**description, rate, plugin_processing::processingBlockSize, error);
        if (plugin == nullptr || !plugin_processing::configurePluginChannels(*plugin, channels)) {
            throw std::runtime_error(utils::format("Could not create test plugin: {}", error).toStdString());
        }
        plugin->setNonRealtime(true);
        plugin->prepareToPlay(rate, plugin_processing::processingBlockSize);
        setBenchmarkParameters(*plugin, latencyPerPlugin);
        scratch.clear();
        midi.clear();
        plugin->processBlock(scratch, midi);
        PluginDescriptorRef descriptor;
        plugin->getStateInformation(descriptor.state);
        plugin->releaseResources();
        options.plugins.push_back(std::move(descriptor));
        instances.push_back(plugin.get());
        plugins.push_back(std::move(plugin));
    }
    const auto latency = instanceCount * latencyPerPlugin;
    constexpr auto block = plugin_processing::processingBlockSize;
    const auto renderedFrames = ((frames + latency + block - 1) / block) * block;
    juce::AudioBuffer<float> output(channels, renderedFrames);
    juce::AudioBuffer<float> reference(channels, renderedFrames);
    juce::Array<juce::var> seconds;
    const utils::CancellationCheck cancellation = [] { return false; };
    for (int iteration = 0; iteration <= iterations; ++iteration) {
        plugin_processing::PreparedChain prepared(instances, cancellation);
        const auto preparation = prepared.prepare(options, channels, rate);
        if (!preparation || preparation->errorMessage.isNotEmpty() || preparation->latencySamples != latency) {
            throw std::runtime_error("Test plugin preparation or reported latency differs from the request");
        }
        const auto start = std::chrono::steady_clock::now();
        render(input, output, scratch, instances, midi, hostOnly);
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (findFailedProcessCall(instances) >= 0 || !isBlockFinite(output, renderedFrames)) {
            throw std::runtime_error("Test plugin processing failed");
        }
        verifyRepeat(output, reference, iteration == 0);
        if (iteration > 0) {
            seconds.add(elapsed);
        }
    }
    const auto inputFile = prefix.getSiblingFile(prefix.getFileName() + ".input.f32");
    const auto outputFile = prefix.getSiblingFile(prefix.getFileName() + ".output.f32");
    writeSamples(inputFile, input, 0, frames);
    writeSamples(outputFile, output, latency, frames);
    juce::var report(new juce::DynamicObject());
    auto* object = report.getDynamicObject();
    object->setProperty("host", "cpp");
    object->setProperty("mode", arguments[4]);
    object->setProperty("channels", channels);
    object->setProperty("sample_rate", rate);
    object->setProperty("input_frames", frames);
    object->setProperty("rendered_frames", renderedFrames);
    object->setProperty("block_frames", block);
    object->setProperty("instances", instanceCount);
    object->setProperty("latency_frames", latency);
    object->setProperty("gain", static_cast<double>(benchmarkGain));
    object->setProperty("width", static_cast<double>(benchmarkWidth));
    object->setProperty("seconds", seconds);
    object->setProperty("input_file", inputFile.getFullPathName());
    object->setProperty("output_file", outputFile.getFullPathName());
    return report;
}
}  // namespace

/// Runs only the explicit benchmark mode and prints a machine-readable report or a clear failure.
int runPluginBenchmark(const juce::StringArray& arguments)
{
    try {
        fmt::print("{}\n", juce::JSON::toString(benchmark(arguments), false).toStdString());
        return 0;
    } catch (const std::exception& error) {
        fmt::print(stderr, "VST3 benchmark failed: {}\n", error.what());
        return 1;
    }
}
}  // namespace audiobatch::tests
