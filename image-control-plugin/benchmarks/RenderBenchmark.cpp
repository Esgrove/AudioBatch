/// Runs the production SpectrogramRenderer offscreen with captured, versioned particle workloads.
/// A small native helper window owns JUCE's real context, while explicit GL-thread batches draw without presentation.
/// Reports raw CPU submission and asynchronous GPU elapsed samples separately from completed-frame throughput.

#include "Parameters.h"
#include "ui/SpectrogramParticles.h"
#include "ui/SpectrogramRenderer.h"
#include <fmt/base.h>
#include <fmt/format.h>
#include <string_view>
#include <system_error>

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace esgrove::image_control::benchmark
{
namespace
{
/// Maximum in-flight frames before a completion fence bounds driver queue depth and query storage.
constexpr std::size_t BATCH_FRAMES {32};
/// Binary workload header and particle layout, in bytes.
constexpr std::uint64_t HEADER_BYTES {24};
constexpr std::uint64_t PARTICLE_BYTES {32};
/// Bound input allocation and accidental command-line run sizes.
constexpr std::uint64_t MAX_INPUT_BYTES {512ULL * 1024 * 1024};
constexpr float MAX_SCALE {8.0F};
constexpr float MAX_FRAME_STEP_SECONDS {0.1F};
constexpr double NANOSECONDS_PER_MILLISECOND {1.0e6};
constexpr double MILLISECONDS_PER_SECOND {1000.0};
constexpr int POLL_INTERVAL_MILLISECONDS {20};
constexpr auto CONTEXT_TIMEOUT {std::chrono::minutes(5)};
constexpr std::uint64_t FNV_OFFSET {0xcbf29ce484222325ULL};
constexpr std::uint64_t FNV_PRIME {0x100000001b3ULL};
constexpr int MAX_FRAMES {1'000'000};
constexpr int MAX_DIMENSION {16'384};
/// Native helper geometry never controls the offscreen target geometry.
constexpr int HELPER_SIZE {64};

/// Parsed command-line geometry and measurement controls.
struct Options {
    std::string input;
    std::string report;
    SpectrogramRenderMode mode {SpectrogramRenderMode::particles};
    int width {1280};
    int height {720};
    int frames {600};
    int warmup {60};
    float scale {1.0F};
    bool gpuTiming {true};
};

/// Decoded workload owns frame-major particles, loaded and validated before context creation.
struct Workload {
    std::vector<SpectrogramParticle> particles;
    std::string inputFnv1a64;
    std::uint32_t frameCount {0};
    std::uint32_t particlesPerFrame {0};
    float frameStepSeconds {0.0F};
    float persistenceSeconds {0.0F};
};

/// Completed measurement phase, with raw frame identities retained for optional GPU samples.
struct Results {
    std::vector<double> cpuFrameMilliseconds;
    std::vector<SpectrogramBenchmarkGpuSample> gpuFrames;
    SpectrogramBenchmarkDevice device;
    double completedMilliseconds {0.0};
    std::size_t completedFrames {0};
};

/// Prints help without opening input, initializing JUCE, creating windows, or writing reports.
void printHelp()
{
    fmt::print(
        "ImageControlRenderBenchmark - production offscreen OpenGL throughput\n"
        "Usage: ImageControlRenderBenchmark --input PATH --report PATH [OPTIONS]\n"
        "  -i, --input PATH       ICRB0001 little-endian particle workload\n"
        "  -W, --width W          Physical target width (default 1280)\n"
        "  -H, --height H         Physical target height (default 720)\n"
        "  -s, --scale F          Physical pixels per logical unit (default 1)\n"
        "  -m, --mode MODE        particles or solid (default particles)\n"
        "  -f, --frames N         Measured frames (default 600)\n"
        "  -w, --warmup N         Unmeasured warmup frames (default 60)\n"
        "  -r, --report PATH      Versioned JSON output\n"
        "  -G, --no-gpu-timing    Disable asynchronous GPU queries\n"
        "  -h, --help             Show help before any side effects\n"
        "Frames cycle through input in file order. Measurement continues after warmup,\n"
        "retaining warmup accumulation. A visible 64x64 context helper is not the measured surface.\n"
        "No audio processing, analyzer, overlays, readback, swap, or compositor time is measured.\n"
        "Measurements require Release with -DAUDIOBATCH_NATIVE_CPU=ON. CI builds support --help only.\n"
    );
}

/// Rejects unoptimized or non-native measurements before input access, JUCE initialization, or GPU work.
void requireMeasurementBuild()
{
#if !IMAGE_CONTROL_BENCHMARK_OPTIMIZED_BUILD || defined(JUCE_DEBUG) || !defined(NDEBUG)
    throw std::runtime_error(
        "Measurements require an optimized Release build. Configure with -DCMAKE_BUILD_TYPE=Release and "
        "-DAUDIOBATCH_NATIVE_CPU=ON, then build with --config Release. --help remains available in every build"
    );
#elif !IMAGE_CONTROL_BENCHMARK_NATIVE_CPU
    throw std::runtime_error(
        "Measurements require native CPU optimizations. Reconfigure a local Release build with "
        "-DAUDIOBATCH_NATIVE_CPU=ON and rebuild. Native targeting is disabled in CI, for cross-compilation, "
        "and for non-host or universal architectures. --help remains available"
    );
#endif
}

/// Parses a full numeric token without silently accepting a prefix or overflowing its destination type.
template<typename Value>
Value parseNumber(const std::string_view text)
{
    Value value {0};
    const auto* end = std::to_address(text.end());
    const auto parsed = std::from_chars(std::to_address(text.begin()), end, value);
    if (parsed.ec != std::errc {} || parsed.ptr != end) {
        throw std::runtime_error("Expected a complete numeric option value");
    }
    return value;
}

/// Applies one value-taking option, keeping token iteration separate from argument interpretation.
void applyOption(Options& options, const std::string_view option, const std::string_view value)
{
    if (option == "--input" || option == "-i") {
        options.input = value;
    } else if (option == "--report" || option == "-r") {
        options.report = value;
    } else if (option == "--width" || option == "-W") {
        options.width = parseNumber<int>(value);
    } else if (option == "--height" || option == "-H") {
        options.height = parseNumber<int>(value);
    } else if (option == "--frames" || option == "-f") {
        options.frames = parseNumber<int>(value);
    } else if (option == "--warmup" || option == "-w") {
        options.warmup = parseNumber<int>(value);
    } else if (option == "--scale" || option == "-s") {
        options.scale = parseNumber<float>(value);
    } else if (option == "--mode" || option == "-m") {
        if (value != "particles" && value != "solid") {
            throw std::runtime_error("Mode must be particles or solid");
        }
        options.mode = value == "solid" ? SpectrogramRenderMode::solid : SpectrogramRenderMode::particles;
    } else {
        throw std::runtime_error(fmt::format("Unknown option: {}", option));
    }
}

/// Parses all required values and rejects unsupported modes or unsafe allocation sizes before GUI setup.
Options parseOptions(const std::span<char*> arguments)
{
    Options options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view option(arguments[index]);
        if (option == "--no-gpu-timing" || option == "-G") {
            options.gpuTiming = false;
            continue;
        }
        if (++index == arguments.size()) {
            throw std::runtime_error("Missing option value");
        }
        applyOption(options, option, arguments[index]);
    }
    if (options.input.empty() || options.report.empty()) {
        throw std::runtime_error("--input and --report are required");
    }
    if (options.width <= 0 || options.height <= 0 || options.width > MAX_DIMENSION || options.height > MAX_DIMENSION
        || options.frames <= 0 || options.frames > MAX_FRAMES || options.warmup < 0 || options.warmup > MAX_FRAMES
        || !std::isfinite(options.scale) || options.scale <= 0.0F || options.scale > MAX_SCALE)
    {
        throw std::runtime_error("Invalid geometry, scale, or frame count");
    }
    return options;
}

/// Decodes IEEE binary32 fields explicitly from little-endian words, independent of native particle padding.
float readFloat(juce::InputStream& input)
{
    return std::bit_cast<float>(static_cast<std::uint32_t>(input.readInt()));
}

/// Validates the exact binary length, finite values, and production queue limits before allocating the workload.
Workload loadWorkload(const Options& options)
{
    const juce::File file(juce::String::fromUTF8(options.input.c_str()));
    const auto input = file.createInputStream();
    if (input == nullptr || file.getSize() < static_cast<juce::int64>(HEADER_BYTES)
        || file.getSize() > static_cast<juce::int64>(MAX_INPUT_BYTES))
    {
        throw std::runtime_error("Cannot read workload, or workload size exceeds 512 MiB");
    }
    std::array<char, 8> magic {};
    if (input->read(magic.data(), static_cast<int>(magic.size())) != static_cast<int>(magic.size())
        || std::string_view(magic.data(), magic.size()) != "ICRB0001")
    {
        throw std::runtime_error("Workload magic must be ICRB0001");
    }
    Workload workload;
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes)) {
        throw std::runtime_error("Cannot hash workload bytes");
    }
    std::uint64_t identity = FNV_OFFSET;
    for (const auto byte : std::span(static_cast<const unsigned char*>(bytes.getData()), bytes.getSize())) {
        identity = (identity ^ byte) * FNV_PRIME;
    }
    workload.inputFnv1a64 = fmt::format("{:016x}", identity);
    workload.frameCount = static_cast<std::uint32_t>(input->readInt());
    workload.particlesPerFrame = static_cast<std::uint32_t>(input->readInt());
    workload.frameStepSeconds = readFloat(*input);
    workload.persistenceSeconds = readFloat(*input);
    const auto count = static_cast<std::uint64_t>(workload.frameCount) * workload.particlesPerFrame;
    if (workload.frameCount == 0 || workload.particlesPerFrame > SPECTROGRAM_MAX_PENDING_PARTICLES
        || count > (MAX_INPUT_BYTES - HEADER_BYTES) / PARTICLE_BYTES
        || HEADER_BYTES + count * PARTICLE_BYTES != static_cast<std::uint64_t>(file.getSize())
        || !std::isfinite(workload.frameStepSeconds) || workload.frameStepSeconds <= 0.0F
        || workload.frameStepSeconds > MAX_FRAME_STEP_SECONDS || !std::isfinite(workload.persistenceSeconds)
        || static_cast<double>(workload.persistenceSeconds) < MIN_PERSISTENCE_SECONDS
        || static_cast<double>(workload.persistenceSeconds) > MAX_PERSISTENCE_SECONDS)
    {
        throw std::runtime_error("Invalid workload counts, length, frame step, or persistence");
    }
    workload.particles.resize(static_cast<std::size_t>(count));
    for (auto& particle : workload.particles) {
        particle = {
            .x = readFloat(*input),
            .y = readFloat(*input),
            .spread = readFloat(*input),
            .red = readFloat(*input),
            .green = readFloat(*input),
            .blue = readFloat(*input),
            .size = readFloat(*input),
            .emphasis = readFloat(*input),
        };
        const std::array fields {
            particle.x,
            particle.y,
            particle.spread,
            particle.red,
            particle.green,
            particle.blue,
            particle.size,
            particle.emphasis,
        };
        if (!std::ranges::all_of(
                fields, [](const float value) { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; }
            )
            || particle.size <= 0.0F)
        {
            throw std::runtime_error("Workload contains invalid normalized particle values");
        }
    }
    if (input->getPosition() != file.getSize()) {
        throw std::runtime_error("Workload read did not consume the exact declared payload");
    }
    return workload;
}

/// Measures successful production submissions in bounded batches, with one completion fence per batch.
/// All calls run on JUCE's current GL thread, without message-thread GUI access.
Results runPhase(
    SpectrogramRenderer& renderer,
    const Options& options,
    const Workload& workload,
    const int frames,
    const std::size_t inputOffset
)
{
    Results results;
    results.cpuFrameMilliseconds.reserve(static_cast<std::size_t>(frames));
    results.gpuFrames.reserve(static_cast<std::size_t>(frames));
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t frame = 0; std::cmp_less(frame, frames);) {
        const auto end = std::min(frame + BATCH_FRAMES, static_cast<std::size_t>(frames));
        for (; frame < end; ++frame) {
            const auto inputFrame = (inputOffset + frame) % workload.frameCount;
            const auto particles = std::span<const SpectrogramParticle>(workload.particles)
                                       .subspan(inputFrame * workload.particlesPerFrame, workload.particlesPerFrame);
            const auto cpuStart = std::chrono::steady_clock::now();
            renderer.submitParticles(particles, frame == 0 && renderer.renderedFrameCount() == 0);
            renderer.submitFrame(
                static_cast<double>(workload.frameStepSeconds),
                static_cast<double>(workload.persistenceSeconds),
                {},
                options.mode
            );
            if (!renderer.drawBenchmarkFrame()) {
                throw std::runtime_error("Production renderer did not encode a complete frame");
            }
            const auto cpuEnd = std::chrono::steady_clock::now();
            results.cpuFrameMilliseconds.push_back(
                std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count()
            );
        }
        if (!renderer.finishBenchmarkBatch()) {
            throw std::runtime_error("GPU completion fence failed, timed out, or OpenGL reported an error");
        }
        results.completedFrames = frame;
        auto samples = renderer.takeBenchmarkGpuSamples();
        results.gpuFrames.insert(results.gpuFrames.end(), samples.begin(), samples.end());
    }
    results.completedMilliseconds
        = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return results;
}

/// Owns the native peer, real OpenGL context, and renderer in their required destruction order.
/// A timer only boots the context and observes completion. It never paces the measured offscreen frames.
class BenchmarkHost final : private juce::Timer
{
public:
    /// Creates the small visible context helper on the message thread, without production editor or audio work.
    BenchmarkHost(const Options& optionsToUse, const Workload& workloadToUse) :
        renderer(context),
        options(optionsToUse),
        workload(workloadToUse)
    {
        component.setName("ImageControl renderer benchmark context");
        component.setSize(HELPER_SIZE, HELPER_SIZE);
        component.addToDesktop(juce::ComponentPeer::windowIsTemporary);
        component.setVisible(true);
        context.setPreferredVersion({4, 1});
        context.setPreferredProfile(juce::OpenGLProfile::core);
        context.setRenderer(&renderer);
        context.setComponentPaintingEnabled(false);
        context.setContinuousRepainting(false);
        context.attachTo(component);
        startTimer(POLL_INTERVAL_MILLISECONDS);
    }

    /// Joins all queued GL work and releases renderer resources before removing the native component peer.
    ~BenchmarkHost() override
    {
        stopTimer();
        context.detach();
        component.removeFromDesktop();
    }

    BenchmarkHost(const BenchmarkHost&) = delete;
    BenchmarkHost(BenchmarkHost&&) = delete;
    BenchmarkHost& operator=(const BenchmarkHost&) = delete;
    BenchmarkHost& operator=(BenchmarkHost&&) = delete;

    /// Returns only fully completed results after the GL thread publishes them with release semantics.
    Results result()
    {
        if (!finished.load(std::memory_order_acquire)) {
            throw std::runtime_error("Context initialization timed out before benchmark execution");
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        return std::move(results);
    }

private:
    /// Queues one explicit batch job after native attachment and keeps dispatching native lifecycle events.
    void timerCallback() override
    {
        if (finished.load(std::memory_order_acquire)) {
            stopTimer();
            juce::MessageManager::getInstance()->stopDispatchLoop();
            return;
        }
        if (!queued && context.isAttached()) {
            queued = true;
            context.executeOnGLThread([this](juce::OpenGLContext&) { execute(); }, false);
        }
        if (std::chrono::steady_clock::now() - created > CONTEXT_TIMEOUT) {
            stopTimer();
            juce::MessageManager::getInstance()->stopDispatchLoop();
        }
    }

    /// Performs allocation, warmup, and measurement on the GL thread, publishing errors rather than fake samples.
    void execute()
    {
        try {
            if (!renderer.prepareBenchmark(options.width, options.height, options.scale, options.gpuTiming)) {
                throw std::runtime_error("Cannot initialize the production offscreen renderer");
            }
            if (!renderer.finishBenchmarkBatch()) {
                throw std::runtime_error("Cannot complete renderer initialization before warmup");
            }
            static_cast<void>(runPhase(renderer, options, workload, options.warmup, 0));
            if (!renderer.prepareBenchmark(options.width, options.height, options.scale, options.gpuTiming)) {
                throw std::runtime_error("Cannot reset benchmark query indexing after warmup");
            }
            results = runPhase(renderer, options, workload, options.frames, static_cast<std::size_t>(options.warmup));
            results.device = SpectrogramRenderer::benchmarkDevice();
        } catch (...) {
            failure = std::current_exception();
        }
        finished.store(true, std::memory_order_release);
    }

    /// Native peer outlives the context attachment, and context outlives renderer resource cleanup.
    juce::Component component;
    juce::OpenGLContext context;
    SpectrogramRenderer renderer;
    /// Caller-owned immutable workload and options outlive the host and every queued job.
    const Options& options;
    const Workload& workload;
    Results results;
    std::exception_ptr failure;
    std::chrono::steady_clock::time_point created {std::chrono::steady_clock::now()};
    std::atomic<bool> finished {false};
    bool queued {false};
};

/// Creates an owned JUCE JSON object without retaining raw owning pointers at call sites.
juce::var jsonObject()
{
    auto object = std::make_unique<juce::DynamicObject>();
    return {object.release()};
}

/// Writes the versioned report outside all timing boundaries, with no invented GPU values when unavailable.
void writeReport(const Options& options, const Workload& workload, const Results& results)
{
    auto const report = jsonObject();
    auto& root = *report.getDynamicObject();
    root.setProperty("schema_version", 1);
    root.setProperty("implementation", "cpp");
    root.setProperty("benchmark", "image-control-renderer");
    root.setProperty("backend", "OpenGL");
    root.setProperty("render_scope", "offscreen-production-renderer");
    root.setProperty("input_fnv1a64", juce::String::fromUTF8(workload.inputFnv1a64.c_str()));
    root.setProperty("gpu", results.device.gpu);
    root.setProperty("vendor", results.device.vendor);
    root.setProperty("driver", results.device.driver);
#ifdef __clang__
    root.setProperty("compiler", "Clang " __clang_version__);
#elifdef _MSC_VER
    root.setProperty("compiler", juce::String("MSVC ") + juce::String(_MSC_VER));
#endif
    root.setProperty("width", options.width);
    root.setProperty("height", options.height);
    root.setProperty("frames", options.frames);
    root.setProperty("warmup", options.warmup);
    root.setProperty("step_seconds", static_cast<double>(workload.frameStepSeconds));
    root.setProperty("batch_wall_ns", results.completedMilliseconds * NANOSECONDS_PER_MILLISECOND);
    root.setProperty("gpu_batch_ns", juce::var());
    root.setProperty("build_profile", IMAGE_CONTROL_BENCHMARK_BUILD_TYPE);
    root.setProperty("build_type", IMAGE_CONTROL_BENCHMARK_BUILD_TYPE);
    root.setProperty("optimized_build", IMAGE_CONTROL_BENCHMARK_OPTIMIZED_BUILD != 0);
    root.setProperty("native_cpu_enabled", IMAGE_CONTROL_BENCHMARK_NATIVE_CPU != 0);
    root.setProperty("mode", options.mode == SpectrogramRenderMode::solid ? "solid" : "particles");
    root.setProperty("input", juce::String::fromUTF8(options.input.c_str()));
    root.setProperty("input_format", "ICRB0001");
    root.setProperty("input_frames", static_cast<juce::int64>(workload.frameCount));
    root.setProperty("particles_per_frame", static_cast<juce::int64>(workload.particlesPerFrame));
    root.setProperty("frame_step_seconds", static_cast<double>(workload.frameStepSeconds));
    root.setProperty("persistence_seconds", static_cast<double>(workload.persistenceSeconds));
    root.setProperty("warmup_frames", options.warmup);
    root.setProperty("requested_frames", options.frames);
    root.setProperty("encoded_frames", static_cast<juce::int64>(results.cpuFrameMilliseconds.size()));
    root.setProperty("completed_frames", static_cast<juce::int64>(results.completedFrames));
    root.setProperty("physical_width", options.width);
    root.setProperty("physical_height", options.height);
    root.setProperty("scale", static_cast<double>(options.scale));
    root.setProperty("logical_width", static_cast<double>(options.width) / static_cast<double>(options.scale));
    root.setProperty("logical_height", static_cast<double>(options.height) / static_cast<double>(options.scale));
#if JUCE_MAC
    root.setProperty("accumulation_format", "RGBA16F");
#else
    root.setProperty("accumulation_format", "RGBA32F");
#endif
    root.setProperty("bloom_format", "RGBA16F");
    root.setProperty("solid_field_format", "RGBA16F");
    root.setProperty("output_format", "RGBA8");
    root.setProperty("bloom_levels", static_cast<int>(SPECTROGRAM_BLOOM_LEVEL_COUNT));
    root.setProperty("batch_frames", static_cast<int>(BATCH_FRAMES));
    root.setProperty("completed_wall_ms", results.completedMilliseconds);
    root.setProperty(
        "completed_frames_per_second",
        static_cast<double>(results.completedFrames) * MILLISECONDS_PER_SECOND / results.completedMilliseconds
    );
    root.setProperty("gpu_timing_enabled", options.gpuTiming);
    const auto* gpuStatus = results.gpuFrames.empty() ? "unavailable" : "available";
    root.setProperty("gpu_timing_status", options.gpuTiming ? gpuStatus : "disabled");
    root.setProperty(
        "cpu_timing_boundary",
        "submitParticles + submitFrame + production offscreen frame encoding + GL binding restore; excludes batch "
        "fence wait"
    );
    root.setProperty(
        "gpu_timing_boundary",
        "asynchronous GL_TIME_ELAPSED around production clear/fade/particles/bloom/solid-field/composite; excludes "
        "target resize and output background clear"
    );
    root.setProperty(
        "throughput_timing_boundary",
        "GL-thread measured phase from first submission through final batch fence completion, including query "
        "collection; excludes context setup, allocation, warmup, JSON, swap and presentation"
    );
    root.setProperty("measurement_history", "retains warmup accumulation and continues input sequence");
    juce::Array<juce::var> cpuSamples;
    for (const auto milliseconds : results.cpuFrameMilliseconds) {
        cpuSamples.add(milliseconds);
    }
    root.setProperty("cpu_frame_ms", cpuSamples);
    juce::Array<juce::var> cpuNanoseconds;
    for (const auto milliseconds : results.cpuFrameMilliseconds) {
        cpuNanoseconds.add(milliseconds * NANOSECONDS_PER_MILLISECOND);
    }
    root.setProperty("cpu_frame_ns", cpuNanoseconds);
    juce::Array<juce::var> gpuSamples;
    juce::Array<juce::var> gpuIndices;
    juce::Array<juce::var> gpuNanoseconds;
    auto orderedGpu = results.gpuFrames;
    std::ranges::sort(orderedGpu, {}, &SpectrogramBenchmarkGpuSample::frameIndex);
    for (const auto& sample : orderedGpu) {
        gpuSamples.add(sample.milliseconds);
        gpuNanoseconds.add(sample.milliseconds * NANOSECONDS_PER_MILLISECOND);
        gpuIndices.add(static_cast<juce::int64>(sample.frameIndex));
    }
    root.setProperty("gpu_frame_ms", gpuSamples);
    root.setProperty("gpu_frame_indices", gpuIndices);
    const bool completeGpuSamples = options.gpuTiming && orderedGpu.size() == results.completedFrames;
    root.setProperty("gpu_frame_ns", completeGpuSamples ? juce::var(gpuNanoseconds) : juce::var());
    auto const capabilities = jsonObject();
    capabilities.getDynamicObject()->setProperty("gpu_timestamps", completeGpuSamples);
    root.setProperty("capabilities", capabilities);
    auto const timing = jsonObject();
    timing.getDynamicObject()->setProperty("cpu", "submit-and-encode");
    timing.getDynamicObject()->setProperty("gpu", completeGpuSamples ? "per-frame-timestamps" : "unsupported");
    root.setProperty("timing_method", timing);
    juce::Array<juce::var> targets;
    const auto appendTarget
        = [&targets](const juce::String& name, const int width, const int height, const juce::String& format) {
              auto const target = jsonObject();
              target.getDynamicObject()->setProperty("name", name);
              target.getDynamicObject()->setProperty("width", width);
              target.getDynamicObject()->setProperty("height", height);
              target.getDynamicObject()->setProperty("format", format);
              targets.add(target);
          };
    appendTarget("output", options.width, options.height, "RGBA8");
    appendTarget("accumulation", options.width, options.height, root.getProperty("accumulation_format").toString());
    int width = options.width;
    int height = options.height;
    for (std::size_t index = 0; index < SPECTROGRAM_BLOOM_LEVEL_COUNT; ++index) {
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
        appendTarget(juce::String("bloom-") + juce::String(static_cast<int>(index)), width, height, "RGBA16F");
        if (index == 1) {
            appendTarget("solid-field", width, height, "RGBA16F");
        }
    }
    root.setProperty("targets", targets);
    const juce::File output(juce::String::fromUTF8(options.report.c_str()));
    if (!output.replaceWithText(juce::JSON::toString(report))) {
        throw std::runtime_error("Cannot write JSON report");
    }
    fmt::print(
        "Completed {} frames in {:.3f} ms ({:.2f} frames/s), {} asynchronous GPU samples\nReport: {}\n",
        results.completedFrames,
        results.completedMilliseconds,
        static_cast<double>(results.completedFrames) * MILLISECONDS_PER_SECOND / results.completedMilliseconds,
        results.gpuFrames.size(),
        options.report
    );
}
/// Parses and loads before GUI initialization, then runs the real context and serializes completed results.
int run(const std::span<char*> arguments)
{
    if (std::ranges::any_of(
            arguments,
            [](const char* argument) {
                return std::string_view(argument) == "--help" || std::string_view(argument) == "-h";
            }
        ))
    {
        printHelp();
        return 0;
    }
    try {
        const auto options = parseOptions(arguments);
        requireMeasurementBuild();
        const auto workload = loadWorkload(options);
        const juce::ScopedJuceInitialiser_GUI juce;
        BenchmarkHost host(options, workload);
        juce::MessageManager::getInstance()->runDispatchLoop();
        writeReport(options, workload, host.result());
        return 0;
    } catch (const std::exception& error) {
        fmt::print(stderr, "ImageControl renderer benchmark: {}\n", error.what());
        return 1;
    }
}
}  // namespace
}  // namespace esgrove::image_control::benchmark

/// Passes process-owned argument strings to the benchmark without initializing native GUI for --help.
int main(const int argc, char** argv)
{
    return esgrove::image_control::benchmark::run(std::span(argv, static_cast<std::size_t>(argc)).subspan(1));
}
