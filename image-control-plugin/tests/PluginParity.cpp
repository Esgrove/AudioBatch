/// ImageControlParity hosts the two explicit build-tree VST3 bundles without editors or audio hardware.
/// Independent delayed-input and analytical expectations complement absolute full-scale null residual limits.
/// Neither implementation supplies expected PCM. DSP mismatches and unsupported common-domain values fail.
/// Recover is compared across the shared 0..200% domain, including its upper endpoint and automation ramps.

#include <fmt/base.h>
#include <fmt/format.h>
#include <initializer_list>
#include <string_view>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Absolute residual limits use 1.0 as digital full scale, not output-relative error near a null.
constexpr double MAX_RESIDUAL_FS {1.0e-5};
constexpr double RMS_RESIDUAL_FS {1.0e-6};
/// Independent post-support silence is stricter than pairwise peak parity and permits only FFT roundoff.
constexpr double DRAIN_MAX_FS {1.0e-6};
/// Both implementations map normalized automation onto the same Recover percentage range.
constexpr double RECOVER_COMMON_MAX_PERCENT {200.0};
/// Independent FIR/adapter timing constants, not copied from either loaded plugin's report.
constexpr int PARTITION_FRAMES {256};
constexpr int PARAMETER_COUNT {31};
constexpr int OUTPUT_GAIN {27};
constexpr int BYPASS {28};
constexpr int DELTA {29};
constexpr int MONO {30};
/// Deterministic broadband sequence identity, with independent state for each channel.
constexpr std::uint32_t NOISE_SEED {0x243f6a88U};

/// The independently specified shared plain-unit parameter domain.
enum class Kind : std::uint8_t { frequency, gain, width, recover, pan, toggle };
/// Independent expected output laws, deliberately separate from production DSP code.
enum class Oracle : std::uint8_t { none, delayed, silence, gain, panLeft, panRight, mono };
/// Input transforms cover polarity consistency and distinct channel ordering without assuming recovery symmetry.
enum class Signal : std::uint8_t { ordinary, inverted, swapped, antiPhase, impulse, silence };

/// One semantic parameter maps differing host-visible names without relying on either host index order.
struct ParameterSpec {
    std::string cppName;
    std::string rustName;
    Kind kind {Kind::gain};
    double neutral {0.0};
};
/// One plain-unit target is converted through the actual hosted controller's text interface.
struct Setting {
    int parameter {0};
    double value {0.0};
};
/// Block-sampled automation is delivered at identical absolute input-frame boundaries in both hosts.
struct Event {
    int frame {0};
    std::vector<Setting> settings;
};
/// A bounded workload includes an independent oracle where the fullband law is known analytically.
struct Scenario {
    /// Owns one case's explicit controls and events, leaving omitted controls at independently specified neutrality.
    Scenario(
        std::string caseName,
        std::vector<Setting> controls,
        std::vector<Event> automation = {},
        Oracle expectation = Oracle::none,
        Signal source = Signal::ordinary
    ) :
        name(std::move(caseName)),
        settings(std::move(controls)),
        events(std::move(automation)),
        oracle(expectation),
        signal(source)
    {}

    std::string name;
    std::vector<Setting> settings;
    std::vector<Event> events;
    Oracle oracle {Oracle::none};
    Signal signal {Signal::ordinary};
};
/// Command-line paths are explicit. No installed plugin search or application storage is used.
struct Options {
    juce::File cpp;
    juce::File rust;
    juce::File report;
    bool quick {false};
};
/// Statistics retain absolute error for silence and near-zero Delta, where ratios are undefined.
struct Metrics {
    double maximum {0.0};
    double squared {0.0};
    std::uint64_t count {0};
    std::uint64_t nonFinite {0};
    std::uint64_t bitMismatches {0};
    int worstFrame {0};
    int worstChannel {0};
};
/// A render owns all captured samples, including initial latency silence and the complete FIR tail.
struct Render {
    juce::AudioBuffer<float> audio;
    juce::Array<juce::var> parameters;
    juce::Array<juce::var> errors;
    int latency {0};
    int warmup {0};
    double tail {0.0};
};
/// The instance owns the parameters observed by this non-null map until after the map is destroyed.
using ParameterMap = std::vector<std::reference_wrapper<juce::AudioProcessorParameter>>;

/// Rejects invalid setup before any result can accidentally be treated as a successful comparison.
void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

/// Transfers a JSON object's ownership directly to JUCE's reference-counted variant.
juce::var object(std::initializer_list<std::pair<juce::Identifier, juce::var>> properties)
{
    auto result = std::make_unique<juce::DynamicObject>();
    for (const auto& [name, value] : properties) {
        result->setProperty(name, value);
    }
    return {result.release()};
}

/// Defines all 31 semantic controls independently of production parameter registries.
std::vector<ParameterSpec> parameterSpecs()
{
    std::vector<ParameterSpec> result;
    result.reserve(PARAMETER_COUNT);
    constexpr std::array cppCrossovers {"Low Crossover", "Mid Crossover", "High Crossover"};
    constexpr std::array frequencies {120.0, 1000.0, 6000.0};
    for (int index = 0; index < 3; ++index) {
        result.push_back({
            .cppName = cppCrossovers.at(static_cast<std::size_t>(index)),
            .rustName = fmt::format("Crossover {}", index + 1),
            .kind = Kind::frequency,
            .neutral = frequencies.at(static_cast<std::size_t>(index)),
        });
    }
    constexpr std::array bands {"Low", "Low Mid", "High Mid", "High"};
    constexpr std::array fields {"Mid", "Side", "Width", "Recover", "Pan", "Solo"};
    constexpr std::array kinds {Kind::gain, Kind::gain, Kind::width, Kind::recover, Kind::pan, Kind::toggle};
    for (std::size_t band = 0; band < bands.size(); ++band) {
        for (std::size_t field = 0; field < fields.size(); ++field) {
            result.push_back({
                .cppName = fmt::format("{}: {}", bands[band], fields[field]),
                .rustName = fmt::format("Band {} {}", band + 1, fields[field]),
                .kind = kinds[field],
                .neutral = field == 2 ? 1.0 : 0.0,
            });
        }
    }
    result.push_back({.cppName = "Output Gain", .rustName = "Output Gain", .kind = Kind::gain, .neutral = 0.0});
    result.push_back({.cppName = "Bypass", .rustName = "Bypass", .kind = Kind::toggle, .neutral = 0.0});
    result.push_back({.cppName = "Delta", .rustName = "Delta", .kind = Kind::toggle, .neutral = 0.0});
    result.push_back({.cppName = "Mono", .rustName = "Mono", .kind = Kind::toggle, .neutral = 0.0});
    require(result.size() == PARAMETER_COUNT, "Internal semantic parameter count must be 31");
    return result;
}

/// Returns the semantic band field index without using either plugin's exposed host indices.
constexpr int bandParameter(const int band, const int field)
{
    return 3 + band * 6 + field;
}

/// Specifies matching per-band controls within the explicitly reported shared plain-unit domain.
std::vector<Setting> allBands(const int field, const double value)
{
    std::vector<Setting> result;
    result.reserve(4);
    for (int band = 0; band < 4; ++band) {
        result.push_back({.parameter = bandParameter(band, field), .value = value});
    }
    return result;
}

/// Appends related plain-unit controls to a scenario without changing their explicit values.
void append(std::vector<Setting>& destination, const std::vector<Setting>& source)
{
    destination.insert(destination.end(), source.begin(), source.end());
}

/// Generates both fast smoke cases and the extended fixed-control and automation contracts.
std::vector<Scenario> scenarios()
{
    std::vector<Scenario> result {
        {"neutral", {}, {}, Oracle::delayed},
        {
            "active",
            {
                {.parameter = 0, .value = 180.0},
                {.parameter = 1, .value = 1200.0},
                {.parameter = 2, .value = 6500.0},
                {.parameter = 3, .value = -3.0},
                {.parameter = 4, .value = 2.0},
                {.parameter = 11, .value = 0.4},
                {.parameter = 12, .value = 75.0},
                {.parameter = 19, .value = -45.0},
                {.parameter = 26, .value = 1.0},
                {.parameter = OUTPUT_GAIN, .value = -2.0},
            },
        },
        {
            "automation",
            {},
            {
                {.frame = 4096,
                 .settings
                 = {{.parameter = 5, .value = 0.25},
                    {.parameter = 6, .value = 100.0},
                    {.parameter = 19, .value = 35.0},
                    {.parameter = OUTPUT_GAIN, .value = -3.0},},},
                {.frame = 8192,
                 .settings
                 = {{.parameter = 0, .value = 240.0},
                    {.parameter = 1, .value = 1800.0},
                    {.parameter = 2, .value = 8000.0},
                    {.parameter = 20, .value = 1.0},
                    {.parameter = DELTA, .value = 1.0},},},
                {.frame = 12'288, .settings = {{.parameter = BYPASS, .value = 1.0}, {.parameter = MONO, .value = 1.0}}},
                {.frame = 16'384,
                 .settings
                 = {{.parameter = BYPASS, .value = 0.0},
                    {.parameter = DELTA, .value = 0.0},
                    {.parameter = MONO, .value = 0.0},
                    {.parameter = 5, .value = 1.0},
                    {.parameter = 20, .value = 0.0},},},
            },
        },
        {"output_gain", {{.parameter = OUTPUT_GAIN, .value = -6.0}}, {}, Oracle::gain},
        {"pan_left", allBands(4, -100.0), {}, Oracle::panLeft},
        {"pan_right", allBands(4, 100.0), {}, Oracle::panRight},
        {"final_mono", {{.parameter = MONO, .value = 1.0}}, {}, Oracle::mono},
        {"neutral_delta", {{.parameter = DELTA, .value = 1.0}}, {}, Oracle::silence},
        {"neutral_impulse", {}, {}, Oracle::delayed, Signal::impulse},
        {"neutral_silence", {}, {}, Oracle::delayed, Signal::silence},
        {"width_zero", allBands(2, 0.0)},
        {"width_double", allBands(2, 2.0)},
        {"recover_100", allBands(2, 0.25)},
        {"recover_150", allBands(2, 0.25)},
        {"recover_200", allBands(2, 0.25)},
        {"recovery_automation_200",
         allBands(2, 0.0),
         {{.frame = 4096, .settings = allBands(3, 100.0)},
          {.frame = 8192, .settings = allBands(3, RECOVER_COMMON_MAX_PERCENT)},
          {.frame = 12'288, .settings = allBands(3, 0.0)},},
         Oracle::none,
         Signal::antiPhase,},
        {"recover_anti_phase", allBands(2, 0.0), {}, Oracle::none, Signal::antiPhase},
        {"recover_inverted", allBands(2, 0.4), {}, Oracle::none, Signal::inverted},
        {"recover_swapped", allBands(2, 0.4), {}, Oracle::none, Signal::swapped},
        {"mid_off_recovery", allBands(0, -60.0)},
        {"side_off_recovery", allBands(1, -60.0)},
        {
            "solo_delta",
            {
                {.parameter = bandParameter(1, 5), .value = 1.0},
                {.parameter = bandParameter(2, 5), .value = 1.0},
                {.parameter = bandParameter(1, 2), .value = 0.5},
                {.parameter = bandParameter(1, 3), .value = 100.0},
                {.parameter = DELTA, .value = 1.0},
            },
        },
        {
            "bypass_active",
            {{.parameter = bandParameter(0, 2), .value = 0.0},
             {.parameter = bandParameter(0, 3), .value = 100.0},
             {.parameter = OUTPUT_GAIN, .value = -12.0},
             {.parameter = DELTA, .value = 1.0},
             {.parameter = BYPASS, .value = 1.0},},
            {},
            Oracle::delayed,
        },
        {"custom_crossovers",
         {{.parameter = 0, .value = 70.0},
          {.parameter = 1, .value = 850.0},
          {.parameter = 2, .value = 10'000.0},
          {.parameter = 5, .value = 0.2},
          {.parameter = 6, .value = 100.0},
          {.parameter = 19, .value = -35.0},},},
    };
    for (auto& scenario : result) {
        if (scenario.name.starts_with("recover_") || scenario.name.ends_with("_off_recovery")) {
            auto recovery = RECOVER_COMMON_MAX_PERCENT;
            if (scenario.name == "recover_100") {
                recovery = 100.0;
            } else if (scenario.name == "recover_150") {
                recovery = 150.0;
            }
            append(scenario.settings, allBands(3, recovery));
        }
        if (scenario.name.ends_with("_off_recovery")) {
            append(scenario.settings, allBands(2, 0.0));
        }
    }
    return result;
}

/// Compares canonical path components rather than string prefixes, rejecting sibling names and symlink escapes.
bool insideRoot(const std::filesystem::path& path, const std::filesystem::path& root)
{
    return std::ranges::mismatch(root, path).in1 == root.end();
}

/// Converts JUCE's Unicode path through the platform-native filesystem encoding without lossy narrow Windows paths.
std::filesystem::path filesystemPath(const juce::String& text)
{
#if JUCE_WINDOWS
    return std::filesystem::path(text.toWideCharPointer());
#else
    return {text.toRawUTF8()};
#endif
}

/// Converts a canonical native filesystem path back to JUCE without depending on the process locale.
juce::File juceFile(const std::filesystem::path& path)
{
#if JUCE_WINDOWS
    return juce::File(juce::String(path.c_str()));
#else
    return {juce::String::fromUTF8(path.c_str())};
#endif
}

/// Authorizes only explicit repository artifacts or descendants of the caller's explicit Cargo target root.
/// Relative CARGO_TARGET_DIR follows the invocation directory, matching Rust's canonicalize contract.
juce::File checkedBundle(const juce::File& bundle)
{
    require(
        bundle.isDirectory() && bundle.hasFileExtension("vst3"), "Both explicit .vst3 bundle directories are required"
    );
    const auto path = std::filesystem::weakly_canonical(filesystemPath(bundle.getFullPathName()));
    const auto repository = std::filesystem::weakly_canonical(std::filesystem::path(IMAGE_CONTROL_PARITY_REPOSITORY));
    if (!insideRoot(path, repository)) {
        const auto target = juce::SystemStats::getEnvironmentVariable("CARGO_TARGET_DIR", {});
        require(target.isNotEmpty(), "External bundles require an explicit CARGO_TARGET_DIR build root");
        const auto root = std::filesystem::weakly_canonical(filesystemPath(target));
        require(
            std::filesystem::is_directory(root) && insideRoot(path, root),
            "Bundle must be inside the repository or the explicit CARGO_TARGET_DIR build root"
        );
    }
    return juceFile(path);
}

/// Resolves the explicit paths only after --help has been handled, never scanning standard plugin locations.
Options parseOptions(const std::span<char*> arguments)
{
    Options result;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument(arguments[index]);
        if (argument == "--quick" || argument == "-q") {
            result.quick = true;
            continue;
        }
        require(index + 1 < arguments.size(), fmt::format("Missing value for {}", argument));
        const juce::String path(arguments[++index]);
        require(juce::File::isAbsolutePath(path), "Plugin and report paths must be absolute");
        if (argument == "--cpp" || argument == "-C" || argument == "-c") {
            result.cpp = juce::File(path);
        } else if (argument == "--rust" || argument == "-r") {
            result.rust = juce::File(path);
        } else if (argument == "--report" || argument == "-o") {
            result.report = juce::File(path);
        } else {
            require(false, fmt::format("Unknown option {}", argument));
        }
    }
    result.cpp = checkedBundle(result.cpp);
    result.rust = checkedBundle(result.rust);
    require(result.cpp != result.rust, "The two plugin bundle paths must differ");
    return result;
}

/// Loads only the named effect from one exact bundle through JUCE's actual VST3 format manager.
std::unique_ptr<juce::AudioPluginInstance> load(
    juce::AudioPluginFormatManager const& formats,
    const juce::File& bundle,
    const bool rust,
    const double rate
)
{
    juce::OwnedArray<juce::PluginDescription> descriptions;
    require(formats.getNumFormats() == 1, "Only VST3 may be registered");
    formats.getFormat(0)->findAllTypesForFile(descriptions, bundle.getFullPathName());
    require(descriptions.size() == 1, "Expected exactly one effect in the explicit bundle");
    require(descriptions[0]->name == (rust ? "ImageControl RS" : "ImageControl"), "Unexpected plugin identity");
    juce::String error;
    auto instance = formats.createPluginInstance(*descriptions[0], rate, 512, error);
    require(
        instance != nullptr,
        fmt::format("Cannot load {}: {}", bundle.getFullPathName().toStdString(), error.toStdString())
    );
    return instance;
}

/// Discovers all names once per instance, asserts a one-to-one 31-control map, and retains no nullable parameters.
ParameterMap mapParameters(
    const juce::AudioPluginInstance& instance,
    const std::vector<ParameterSpec>& specs,
    const bool rust
)
{
    require(instance.getParameters().size() == PARAMETER_COUNT, "Expected exactly 31 exposed sound parameters");
    ParameterMap result;
    for (const auto& spec : specs) {
        const auto& name = rust ? spec.rustName : spec.cppName;
        int matches = 0;
        for (auto* parameter : instance.getParameters()) {
            if (parameter->getName(100).toStdString() == name) {
                result.emplace_back(*parameter);
                ++matches;
            }
        }
        require(matches == 1, fmt::format("Expected one parameter named {}", name));
    }
    return result;
}

/// Produces unambiguous host text, avoiding any assumed normalized-to-plain range for hosted parameters.
juce::String targetText(const ParameterSpec& spec, const double value)
{
    if (spec.kind == Kind::toggle) {
        return value >= 0.5 ? "On" : "Off";
    }
    return {value, 6};
}

/// Parses the actual readback independently, including both implementations' units and muted gain spelling.
double readPlain(const juce::String& text, const Kind kind)
{
    const auto value = text.trim().toLowerCase();
    if (kind == Kind::toggle) {
        require(value == "on" || value == "off", "Invalid boolean readback");
        return value == "on" ? 1.0 : 0.0;
    }
    if (value == "off" || value.contains("inf")) {
        return kind == Kind::gain ? -60.0 : 0.0;
    }
    if (value == "c") {
        return 0.0;
    }
    const auto number = value.retainCharacters("-+.0123456789");
    require(number.isNotEmpty(), fmt::format("Unreadable parameter text {}", text.toStdString()));
    auto result = number.getDoubleValue();
    if (kind == Kind::frequency && value.contains("khz")) {
        result *= 1000.0;
    }
    if (kind == Kind::pan && value.endsWith("l")) {
        result = -std::abs(result);
    }
    return result;
}

/// Applies a value through the real controller and records quantization/domain failures instead of silently clamping.
void apply(
    const Setting& setting,
    const ParameterMap& parameters,
    const std::vector<ParameterSpec>& specs,
    Render& render,
    const int frame
)
{
    const auto index = static_cast<std::size_t>(setting.parameter);
    auto& parameter = parameters.at(index).get();
    const auto& spec = specs.at(index);
    require(
        spec.kind != Kind::recover || (setting.value >= 0.0 && setting.value <= RECOVER_COMMON_MAX_PERCENT),
        "Recover parity targets must be inside the explicit shared 0..200% domain"
    );
    const auto text = targetText(spec, setting.value);
    const auto normalized = parameter.getValueForText(text);
    parameter.setValueNotifyingHost(normalized);
    const auto readback = parameter.getText(parameter.getValue(), 128);
    const auto plain = readPlain(readback, spec.kind);
    // Host displays round frequencies. Test targets are exactly on the common display/parameter grid.
    const double tolerance = spec.kind == Kind::frequency ? 0.51 : 0.011;
    const bool accepted = std::isfinite(normalized) && std::abs(plain - setting.value) <= tolerance;
    render.parameters.add(object({
        {"parameter", juce::String(spec.rustName)},
        {"frame", frame},
        {"requested", setting.value},
        {"normalized", static_cast<double>(normalized)},
        {"readback", readback},
        {"accepted", accepted},
    }));
    if (!accepted) {
        render.errors.add(
            juce::String(
                fmt::format(
                    "{} at {}: requested {}, read back {}", spec.rustName, frame, setting.value, readback.toStdString()
                )
            )
        );
    }
}

/// Computes the independent kernel size from the documented power-of-two sample-rate scaling rule.
int halfLength(const double rate)
{
    int result = 4096;
    double reference = 48'000.0;
    while (rate > reference) {
        result *= 2;
        reference *= 2.0;
    }
    return result;
}

/// Applies source transforms independently of production processing, without assuming recovery channel symmetry.
std::array<float, 2> transformInput(std::array<float, 2> values, const Signal signal, const int frame, const int frames)
{
    switch (signal) {
        case Signal::ordinary:
            break;
        case Signal::antiPhase:
            values[1] = -values[0];
            break;
        case Signal::swapped:
            std::swap(values[0], values[1]);
            break;
        case Signal::inverted:
            values[0] = -values[0];
            values[1] = -values[1];
            break;
        case Signal::impulse:
            values = {frame == 0 ? 0.25F : 0.0F, frame == frames - 1 ? -0.125F : 0.0F};
            break;
        case Signal::silence:
            values = {};
            break;
    }
    return values;
}

/// Generates deterministic, sub-full-scale multitone/noise with endpoint impulses and explicit channel transforms.
juce::AudioBuffer<float> makeInput(const int channels, const int frames, const double rate, const Signal signal)
{
    if ((channels != 1 && channels != 2) || frames <= 0) {
        throw std::runtime_error("Independent source requires positive mono/stereo dimensions");
    }
    juce::AudioBuffer<float> result(channels, frames);
    std::uint32_t state = NOISE_SEED;
    constexpr std::array frequencies {37.0, 173.0, 997.0, 4321.0, 15'731.0};
    for (int frame = 0; frame < frames; ++frame) {
        std::array<float, 2> values {};
        for (int channel = 0; channel < 2; ++channel) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            // A 24-bit bounded uniform source avoids implementation-defined integer distributions.
            double value = (static_cast<double>(state >> 8U) / 16'777'216.0 - 0.5) * 0.04;
            for (std::size_t tone = 0; tone < frequencies.size(); ++tone) {
                const auto phase = 2.0 * std::numbers::pi * frequencies[tone] * static_cast<double>(frame) / rate
                    + static_cast<double>(channel) * (0.3 + static_cast<double>(tone) * 0.41);
                value += 0.035 * std::sin(phase);
            }
            if (frame == 0 || frame == frames - 1) {
                value += channel == 0 ? 0.125 : -0.09375;
            }
            values.at(static_cast<std::size_t>(channel)) = static_cast<float>(value);
        }
        values = transformInput(values, signal, frame, frames);
        for (int channel = 0; channel < channels; ++channel) {
            result.setSample(channel, frame, values.at(static_cast<std::size_t>(channel)));
        }
    }
    return result;
}

/// Uses an explicitly byte-ordered FNV-1a fingerprint, preserving signed zero and exact-neutral sample identity.
std::string checksum(const juce::AudioBuffer<float>& audio)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (int frame = 0; frame < audio.getNumSamples(); ++frame) {
        for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
            const auto bits = std::bit_cast<std::uint32_t>(audio.getSample(channel, frame));
            for (unsigned shift = 0; shift < 32U; shift += 8U) {
                hash ^= (bits >> shift) & 0xffU;
                hash *= 0x100000001b3ULL;
            }
        }
    }
    return fmt::format("{:016x}", hash);
}

/// Performs a complete offline render after silent settling, with sample-identical automation boundaries and no editor.
Render render(
    juce::AudioPluginInstance& instance,
    const ParameterMap& parameters,
    const std::vector<ParameterSpec>& specs,
    const Scenario& scenario,
    const juce::AudioBuffer<float>& input,
    const double rate,
    const std::vector<int>& blocks
)
{
    Render result;
    require(
        scenario.events.empty() || scenario.events.back().frame < input.getNumSamples(),
        "Automation must stop before the independent input endpoint"
    );
    instance.releaseResources();
    const auto channels = input.getNumChannels();
    if (channels != 1 && channels != 2) {
        throw std::runtime_error("Parity rendering requires mono or stereo input");
    }
    auto layout = instance.getBusesLayout();
    layout.inputBuses.set(0, juce::AudioChannelSet::canonicalChannelSet(channels));
    layout.outputBuses.set(0, juce::AudioChannelSet::canonicalChannelSet(channels));
    require(instance.setBusesLayout(layout), "Plugin rejected matching mono/stereo buses");
    instance.setNonRealtime(true);
    instance.prepareToPlay(rate, 512);
    for (std::size_t index = 0; index < specs.size(); ++index) {
        apply({.parameter = static_cast<int>(index), .value = specs[index].neutral}, parameters, specs, result, -1);
    }
    for (const auto& setting : scenario.settings) {
        apply(setting, parameters, specs, result, -1);
    }
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer(channels, 512);
    // Text edits travel through VST3 parameter queues on process(), not prepare().
    // Warm zeros settle all 20 ms ramps and synchronous crossover replacements before recording the entire response.
    const int expectedLatency = halfLength(rate) + PARTITION_FRAMES;
    result.warmup = 2 * expectedLatency + static_cast<int>(std::ceil(rate * 0.025));
    for (int offset = 0; offset < result.warmup; offset += 512) {
        buffer.setSize(channels, std::min(512, result.warmup - offset), false, false, true);
        buffer.clear();
        instance.processBlock(buffer, midi);
    }
    result.latency = instance.getLatencySamples();
    result.tail = instance.getTailLengthSeconds();
    if (result.latency != expectedLatency) {
        result.errors.add(
            juce::String(
                fmt::format("Latency {} differs from independent expectation {}", result.latency, expectedLatency)
            )
        );
    }
    if (!std::isfinite(result.tail) || std::abs(result.tail * rate - static_cast<double>(halfLength(rate))) > 1.0) {
        result.errors.add("Tail report differs from independent FIR support");
    }
    // Independent support, plus two partitions of verified drain, cannot shrink when a plugin under-reports its tail.
    const int total = input.getNumSamples() + expectedLatency + halfLength(rate) + 2 * PARTITION_FRAMES;
    result.audio.setSize(channels, total);
    std::size_t blockIndex = 0;
    std::size_t eventIndex = 0;
    for (int offset = 0; offset < total;) {
        if (eventIndex < scenario.events.size() && scenario.events[eventIndex].frame == offset) {
            for (const auto& setting : scenario.events[eventIndex].settings) {
                apply(setting, parameters, specs, result, offset);
            }
            ++eventIndex;
        }
        int count = std::min(blocks[blockIndex++ % blocks.size()], total - offset);
        if (eventIndex < scenario.events.size()) {
            count = std::min(count, scenario.events[eventIndex].frame - offset);
        }
        require(count > 0, "Automation events must be strictly increasing");
        buffer.setSize(channels, count, false, false, true);
        buffer.clear();
        const int available = std::min(count, input.getNumSamples() - offset);
        if (available > 0) {
            for (int channel = 0; channel < channels; ++channel) {
                buffer.copyFrom(channel, 0, input, channel, offset, available);
            }
        }
        midi.clear();
        instance.processBlock(buffer, midi);
        require(instance.getLatencySamples() == result.latency, "Automation changed latency");
        for (int channel = 0; channel < channels; ++channel) {
            result.audio.copyFrom(channel, offset, buffer, channel, 0, count);
        }
        offset += count;
    }
    instance.releaseResources();
    return result;
}

/// Evaluates one independent global signal law without using production conversion or mixing helpers.
double expectedSample(
    const double source,
    const double left,
    const double right,
    const int channel,
    const int channels,
    const Oracle oracle
)
{
    switch (oracle) {
        case Oracle::none:
        case Oracle::delayed:
            return source;
        case Oracle::silence:
            return 0.0;
        case Oracle::gain:
            return source * std::pow(10.0, -6.0 / 20.0);
        case Oracle::panLeft:
            if (channels == 1) {
                return source;
            }
            return channel == 0 ? left + right : 0.0;
        case Oracle::panRight:
            if (channels == 1) {
                return source;
            }
            return channel == 1 ? left + right : 0.0;
        case Oracle::mono:
            return (left + right) * 0.5;
    }
    throw std::runtime_error("Unknown independent oracle");
}

/// Builds full-stream expected PCM from source samples and analytical fullband laws, not filtered plugin output.
juce::AudioBuffer<float> expected(
    const juce::AudioBuffer<float>& input,
    const int total,
    const int latency,
    const Oracle oracle
)
{
    const auto channels = input.getNumChannels();
    if ((channels != 1 && channels != 2) || total <= 0) {
        throw std::runtime_error("Independent oracle requires positive mono/stereo dimensions");
    }
    juce::AudioBuffer<float> result(channels, total);
    result.clear();
    for (int frame = 0; frame < input.getNumSamples(); ++frame) {
        const auto left = static_cast<double>(input.getSample(0, frame));
        const auto right = static_cast<double>(input.getSample(input.getNumChannels() - 1, frame));
        for (int channel = 0; channel < input.getNumChannels(); ++channel) {
            const auto value = expectedSample(
                static_cast<double>(input.getSample(channel, frame)), left, right, channel, channels, oracle
            );
            result.setSample(channel, frame + latency, static_cast<float>(value));
        }
    }
    return result;
}

/// Counts every sample without latency fitting, residual trimming, RMS ratios, or ignored pre/tail regions.
Metrics compare(const juce::AudioBuffer<float>& first, const juce::AudioBuffer<float>& second)
{
    require(
        first.getNumChannels() == second.getNumChannels() && first.getNumSamples() == second.getNumSamples(),
        "Render dimensions differ"
    );
    Metrics result;
    for (int channel = 0; channel < first.getNumChannels(); ++channel) {
        for (int frame = 0; frame < first.getNumSamples(); ++frame) {
            const float firstValue = first.getSample(channel, frame);
            const float secondValue = second.getSample(channel, frame);
            ++result.count;
            if (std::bit_cast<std::uint32_t>(firstValue) != std::bit_cast<std::uint32_t>(secondValue)) {
                ++result.bitMismatches;
            }
            if (!std::isfinite(firstValue) || !std::isfinite(secondValue)) {
                ++result.nonFinite;
                continue;
            }
            const double difference = static_cast<double>(firstValue) - static_cast<double>(secondValue);
            result.squared += difference * difference;
            if (std::abs(difference) > result.maximum) {
                result.maximum = std::abs(difference);
                result.worstFrame = frame;
                result.worstChannel = channel;
            }
        }
    }
    return result;
}

/// Converts accumulated energy to full-scale RMS, well-defined even for a complete null.
double rms(const Metrics& metrics)
{
    return std::sqrt(metrics.squared / static_cast<double>(std::max(std::uint64_t {1}, metrics.count)));
}

/// Makes independent exact-neutral checks stricter than the deliberately non-bit-exact parity comparison.
bool passes(const Metrics& metrics, const bool exact = false)
{
    return metrics.nonFinite == 0 && metrics.maximum <= MAX_RESIDUAL_FS && rms(metrics) <= RMS_RESIDUAL_FS
        && (!exact || metrics.bitMismatches == 0);
}

/// Serializes null-safe absolute residuals and exact-identity diagnostics for artifact inspection.
juce::var metricJson(const Metrics& metrics)
{
    return object({
        {"max_residual_fs", metrics.maximum},
        {"rms_residual_fs", rms(metrics)},
        {"sample_count", static_cast<juce::int64>(metrics.count)},
        {"non_finite_samples", static_cast<juce::int64>(metrics.nonFinite)},
        {"bit_mismatches", static_cast<juce::int64>(metrics.bitMismatches)},
        {"worst_frame", metrics.worstFrame},
        {"worst_channel", metrics.worstChannel},
    });
}

/// Measures each implementation's final two partitions against independent silence, not against its counterpart.
Metrics measureDrain(const juce::AudioBuffer<float>& audio)
{
    constexpr int drainFrames = 2 * PARTITION_FRAMES;
    require(audio.getNumSamples() >= drainFrames, "Render must include both independent drain partitions");
    Metrics result;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
        for (int frame = audio.getNumSamples() - drainFrames; frame < audio.getNumSamples(); ++frame) {
            ++result.count;
            const auto value = static_cast<double>(audio.getSample(channel, frame));
            if (!std::isfinite(value)) {
                ++result.nonFinite;
                continue;
            }
            result.squared += value * value;
            if (std::abs(value) > result.maximum) {
                result.maximum = std::abs(value);
                result.worstFrame = frame;
                result.worstChannel = channel;
            }
        }
    }
    return result;
}

/// Requires the finite FIR support to have ended even when both implementations share the same residual signal.
bool drainPasses(const Metrics& metrics)
{
    return metrics.nonFinite == 0 && metrics.maximum <= DRAIN_MAX_FS && rms(metrics) <= RMS_RESIDUAL_FS;
}

/// Proves the independent drain gate rejects matching ringing and non-finite output without changing the case matrix.
void verifyDrainGate()
{
    juce::AudioBuffer<float> first(2, 2 * PARTITION_FRAMES);
    first.clear();
    require(drainPasses(measureDrain(first)), "Drain gate rejected independent silence");
    first.setSample(1, PARTITION_FRAMES, static_cast<float>(DRAIN_MAX_FS * 0.25));
    require(drainPasses(measureDrain(first)), "Drain gate rejected bounded FFT residue");
    first.setSample(1, PARTITION_FRAMES, static_cast<float>(DRAIN_MAX_FS * 2.0));
    const auto second = first;
    require(
        passes(compare(first, second)) && !drainPasses(measureDrain(first)) && !drainPasses(measureDrain(second)),
        "Matching ringing must fail independent drain despite a perfect pairwise null"
    );
    first.setSample(1, PARTITION_FRAMES, std::numeric_limits<float>::quiet_NaN());
    require(!drainPasses(measureDrain(first)), "Drain gate accepted non-finite output");
}

/// Returns one implementation's lifecycle identity and every plain-unit controller readback.
juce::var renderJson(const Render& value)
{
    return object({
        {"latency_samples", value.latency},
        {"tail_seconds", value.tail},
        {"warmup_frames", value.warmup},
        {"checksum_fnv1a64_le", juce::String(checksum(value.audio))},
        {"parameters", value.parameters},
        {"errors", value.errors},
        {"drain", metricJson(measureDrain(value.audio))},
        {"drain_passed", drainPasses(measureDrain(value.audio))},
    });
}

/// Adds source-independent analytical expectations without trusting either plugin as the oracle.
bool checkIndependent(
    juce::DynamicObject& details,
    const juce::AudioBuffer<float>& input,
    const Render& first,
    const Render& second,
    const Scenario& scenario,
    const double rate
)
{
    if (scenario.oracle == Oracle::none) {
        return true;
    }
    const auto reference
        = expected(input, first.audio.getNumSamples(), halfLength(rate) + PARTITION_FRAMES, scenario.oracle);
    const auto cppExpected = compare(first.audio, reference);
    const auto rustExpected = compare(second.audio, reference);
    const bool exact = scenario.oracle == Oracle::delayed || scenario.oracle == Oracle::silence;
    details.setProperty("expected_checksum_fnv1a64_le", juce::String(checksum(reference)));
    details.setProperty("cpp_independent", metricJson(cppExpected));
    details.setProperty("rust_independent", metricJson(rustExpected));
    details.setProperty("independent_exact", exact);
    return passes(cppExpected, exact) && passes(rustExpected, exact);
}

/// Executes an isolated workload and records exceptions as failed cases rather than aborting remaining coverage.
juce::var runCase(
    juce::AudioPluginFormatManager const& formats,
    const Options& options,
    const std::vector<ParameterSpec>& specs,
    const Scenario& scenario,
    const double rate,
    const int channels,
    const std::vector<int>& blocks,
    const int pattern
)
{
    const auto identity = fmt::format("{}/{}/{}/{}", scenario.name, rate, channels, pattern);
    const auto entry = object({
        {"identity", juce::String(identity)},
        {"scenario", juce::String(scenario.name)},
        {"sample_rate", rate},
        {"channels", channels},
        {"block_pattern_index", pattern},
        {"exceptions", juce::Array<juce::var> {}},
    });
    auto& details = *entry.getDynamicObject();
    juce::Array<juce::var> blockPattern;
    for (const auto block : blocks) {
        blockPattern.add(block);
    }
    details.setProperty("block_frames", blockPattern);
    try {
        auto cpp = load(formats, options.cpp, false, rate);
        auto rust = load(formats, options.rust, true, rate);
        const auto cppParameters = mapParameters(*cpp, specs, false);
        const auto rustParameters = mapParameters(*rust, specs, true);
        constexpr int inputFrames = 20'483;
        const auto input = makeInput(channels, inputFrames, rate, scenario.signal);
        const auto first = render(*cpp, cppParameters, specs, scenario, input, rate, blocks);
        const auto second = render(*rust, rustParameters, specs, scenario, input, rate, blocks);
        const auto residual = compare(first.audio, second.audio);
        const bool independent = checkIndependent(details, input, first, second, scenario, rate);
        const bool passed = passes(residual) && first.errors.isEmpty() && second.errors.isEmpty() && independent
            && drainPasses(measureDrain(first.audio)) && drainPasses(measureDrain(second.audio));
        details.setProperty("input_frames", inputFrames);
        details.setProperty("rendered_frames", first.audio.getNumSamples());
        details.setProperty("independent_drain_start_frame", inputFrames + 2 * halfLength(rate) + PARTITION_FRAMES);
        details.setProperty("independent_drain_frames", 2 * PARTITION_FRAMES);
        details.setProperty("input_checksum_fnv1a64_le", juce::String(checksum(input)));
        details.setProperty("cpp", renderJson(first));
        details.setProperty("rust", renderJson(second));
        details.setProperty("residual", metricJson(residual));
        details.setProperty("passed", passed);
        fmt::print(
            "{} {} max={:.9g} rms={:.9g} parameter/lifecycle-errors={}/{}\n",
            passed ? "PASS" : "FAIL",
            identity,
            residual.maximum,
            rms(residual),
            first.errors.size(),
            second.errors.size()
        );
    } catch (const std::exception& error) {
        details.setProperty("passed", false);
        details.setProperty("error", juce::String(error.what()));
        fmt::print(stderr, "FAIL {}: {}\n", identity, error.what());
    }
    return entry;
}

/// Exercises every selected scenario for one rate, layout, and host-block pattern.
void runPattern(
    const juce::AudioPluginFormatManager& formats,
    const Options& options,
    const std::vector<ParameterSpec>& specs,
    const std::vector<Scenario>& cases,
    const double rate,
    const int channels,
    const std::vector<int>& blocks,
    const int pattern,
    const bool extended,
    juce::Array<juce::var>& results
)
{
    const auto count = extended ? cases.size() : std::size_t {3};
    for (std::size_t index = 0; index < count; ++index) {
        results.add(runCase(formats, options, specs, cases[index], rate, channels, blocks, pattern));
    }
}

/// Exercises the common block/rate/layout matrix, then extended cases with a bounded irregular host workload.
int run(const Options& options)
{
    verifyDrainGate();
    juce::AudioPluginFormatManager formats;
    formats.addFormat(std::make_unique<juce::VST3PluginFormat>());
    const auto specs = parameterSpecs();
    const auto cases = scenarios();
    const std::vector<double> rates = options.quick ? std::vector<double> {48'000.0}
                                                    : std::vector<double> {44'100.0, 48'000.0, 96'000.0, 192'000.0};
    const std::vector<std::vector<int>> patterns = options.quick
        ? std::vector<std::vector<int>> {{256}, {17, 511, 1, 73, 256}}
        : std::vector<std::vector<int>> {{32}, {256}, {512}, {17, 511, 1, 73, 256}};
    juce::Array<juce::var> results;
    for (const double rate : rates) {
        for (const int channels : {1, 2}) {
            for (std::size_t pattern = 0; pattern < patterns.size(); ++pattern) {
                runPattern(
                    formats,
                    options,
                    specs,
                    cases,
                    rate,
                    channels,
                    patterns[pattern],
                    static_cast<int>(pattern),
                    pattern + 1 == patterns.size(),
                    results
                );
            }
        }
    }
    const auto failed = static_cast<int>(std::ranges::count_if(results, [](const juce::var& entry) {
        return !static_cast<bool>(entry["passed"]);
    }));
    const auto report = object({
        {"schema_version", 1},
        {"cpp_bundle", options.cpp.getFullPathName()},
        {"rust_bundle", options.rust.getFullPathName()},
        {"quick", options.quick},
        {"max_residual_fs_limit", MAX_RESIDUAL_FS},
        {"rms_residual_fs_limit", RMS_RESIDUAL_FS},
        {"drain_max_fs_limit", DRAIN_MAX_FS},
        {"drain_rms_fs_limit", RMS_RESIDUAL_FS},
        {"drain_gate_mutation_check_passed", true},
        {"recover_common_max_percent", RECOVER_COMMON_MAX_PERCENT},
        {"capability_differences", juce::Array<juce::var> {}},
        {"noise_seed", static_cast<juce::int64>(NOISE_SEED)},
        {"input_peak_bound_fs", 0.32},
        {"input_tone_amplitude_fs", 0.035},
        {"input_uniform_noise_peak_fs", 0.02},
        {"cases", results},
        {"failed", failed},
    });
    if (options.report != juce::File {}) {
        require(options.report.getParentDirectory().createDirectory().wasOk(), "Cannot create report directory");
        require(options.report.replaceWithText(juce::JSON::toString(report)), "Cannot write parity report");
    }
    fmt::print(
        "ImageControl parity: {} cases, {} failed. Absolute limits: max 1e-5 FS (-100 dBFS), RMS 1e-6 FS (-120 "
        "dBFS).\n",
        results.size(),
        failed
    );
    return failed == 0 ? 0 : 1;
}
}  // namespace
}  // namespace esgrove::image_control::test

/// Handles help before initializing JUCE or inspecting bundles, then runs only explicit offline comparisons.
int main(const int argc, char* argv[])
{
    const std::span arguments(argv, static_cast<std::size_t>(argc));
    for (const auto* argument : arguments.subspan(1)) {
        const std::string_view value(argument);
        if (value == "--help" || value == "-h") {
            fmt::print(
                "Usage: ImageControlParity -C <absolute C++ .vst3> -r <absolute Rust .vst3> [options]\n"
                "  -C, --cpp PATH    Explicit build-tree C++ bundle (-c is a compatibility alias)\n"
                "  -r, --rust PATH   Explicit build-tree Rust bundle\n"
                "  -o, --report PATH Write per-case JSON diagnostics to this absolute path\n"
                "  -q, --quick       Use 48 kHz with fixed and irregular blocks (all extended scenarios)\n"
                "  -h, --help        Show help without loading plugins\n"
                "Full: 44.1/48/96/192 kHz, mono/stereo, blocks 32/256/512/irregular. No editors or audio device.\n"
                "Recover common domain: 0..200%, including endpoint and automation comparisons.\n"
                "Bundles must be inside the repository or explicit CARGO_TARGET_DIR. No installed-plugin search.\n"
            );
            return 0;
        }
    }
    try {
        const auto options = esgrove::image_control::test::parseOptions(arguments);
        const juce::ScopedJuceInitialiser_GUI initializer;
        return esgrove::image_control::test::run(options);
    } catch (const std::exception& error) {
        fmt::print(stderr, "ImageControl parity setup failed: {}\n", error.what());
        return 2;
    }
}
