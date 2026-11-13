/// Parses benchmark workload options and reports reproducibility context before running the CPU probes.
/// Observations are not pass/fail thresholds or improvement evidence without a baseline.
#include "benchmarks/BenchmarkSupport.h"
#include "Parameters.h"
#include <fmt/base.h>
#include <string_view>
#include <system_error>

#include <juce_events/juce_events.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace esgrove::image_control::benchmark
{
namespace
{
constexpr int MAXIMUM_REPETITIONS {10};
constexpr double QUICK_PACED_SECONDS {0.16};
/// Short-run defaults retain repeated observations without changing the selected workload matrix.
constexpr int QUICK_CALLBACKS {128};
constexpr int QUICK_REPETITIONS {2};
/// Explicit callback counts stay within the documented bounded workload range.
constexpr int MINIMUM_CALLBACKS {32};
constexpr int MAXIMUM_CALLBACKS {16'384};
/// Back-to-back clock reads estimate measurement overhead separately from callback work.
constexpr int CLOCK_OVERHEAD_SAMPLES {1024};

/// Rejects unoptimized or non-native measurements before JUCE initialization or workload execution.
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

/// Prints reproducibility context without pretending runtime detection reveals all compiler/linker flags.
void printIdentity(const Options& options)
{
    fmt::print(
        "ImageControl performance observations | label={} | {}\n",
        options.label,
        juce::Time::getCurrentTime().toISO8601(true).toStdString()
    );
    fmt::print(
        "OS={} CPU={} vendor={} physical/logical={}/{} pointer_bits={}\n",
        juce::SystemStats::getOperatingSystemName().toStdString(),
        juce::SystemStats::getCpuModel().toStdString(),
        juce::SystemStats::getCpuVendor().toStdString(),
        juce::SystemStats::getNumPhysicalCpus(),
        juce::SystemStats::getNumCpus(),
        sizeof(void*) * 8
    );
#ifdef __clang__
    fmt::print("compiler=Clang {}\n", __clang_version__);
#elifdef _MSC_VER
    fmt::print("compiler=MSVC {} full={}\n", _MSC_VER, _MSC_FULL_VER);
#endif
    fmt::print(
        "C++={} JUCE={} JUCE_DEBUG={}\n",
        static_cast<long long>(__cplusplus),
        juce::SystemStats::getJUCEVersion().toStdString(),
#if JUCE_DEBUG
        true
#else
        false
#endif
    );
#ifdef _LIBCPP_VERSION
    fmt::print("stdlib=libc++ {}\n", _LIBCPP_VERSION);
#endif
#ifdef _LIBCPP_HARDENING_MODE
    fmt::print("libc++ hardening mode={}\n", _LIBCPP_HARDENING_MODE);
#endif
#ifdef _MSVC_STL_VERSION
    fmt::print("stdlib=MSVC STL {}\n", _MSVC_STL_VERSION);
#endif
#ifdef _MSVC_STL_HARDENING
    fmt::print("MSVC STL hardening={}\n", _MSVC_STL_HARDENING);
#endif
#ifdef __AVX2__
    fmt::print("compiled ISA: AVX2\n");
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    fmt::print("compiled ISA: NEON\n");
#endif
#ifdef __FMA__
    fmt::print("compiled ISA: FMA\n");
#endif
    fmt::print(
        "build_type={} optimized_build={} native_cpu_enabled={} NDEBUG=1\n",
        IMAGE_CONTROL_BENCHMARK_BUILD_TYPE,
        IMAGE_CONTROL_BENCHMARK_OPTIMIZED_BUILD != 0,
        IMAGE_CONTROL_BENCHMARK_NATIVE_CPU != 0
    );
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    fmt::print(
        "binary={} bytes={} modified={}\n",
        executable.getFullPathName().toStdString(),
        executable.getSize(),
        executable.getLastModificationTime().toISO8601(true).toStdString()
    );
    fmt::print(
        "repetitions={} callbacks/steady_rep={} paced_s/rep={:.3f} refreshes/component_rep={} worker={} focused={}\n",
        options.repetitions,
        options.callbacks,
        options.pacedSeconds,
        options.refreshes,
        options.worker,
        options.focused
    );
    fmt::print(
        R"(Warmup: callback latency + gain settling + one maximum block per repetition. Components: one second or more.
Input: independent fixed-seed stereo noise, amplitude +/-.3. Irregular blocks: 1,31,97,255,17,509,64,257.
Callback timing excludes copying, parameter setters, checksums, FIFO pop, worker wake, and sleep.
Original modes explicitly set Width 1 and Recover Off. Narrowed rows share Widths 0/.25/.5/.75.
Recovery variants select Off, two bands, or all bands, each with meter/worker and moving-crossover coverage.
Focused callback rows hold 48 kHz, meters off, drained FIFO, and settled nonneutral gains/pans at 32/64 samples.
Solo rows select band 1 through host parameters, with separate solo, delta, and combined solo+delta observations.
Analyzer-only observes frames with a constant-time callback. Particles-only replays warmed, owned analyzer frames.
Overlay rows retain the unhovered reference and add settled band-response and crossover hover painting.
Isolated setter rows time batched Width+Recover target pairs, excluding process, copies, and checksums.
Correlation rows include four live/trend/range history updates and full-row software painting at simulated 120 Hz.
Correlation warmup fills maximum Persistence history per repetition, including --quick and shorter selected windows.
Correlation timing excludes reading generation, canvas clearing, checksums, construction, and layout. No audio DSP is run.
Clock overhead is not subtracted.
deadline[x] is callback time / that block's audio duration, NOT an OS scheduling or dropout guarantee.
Unpaced worker-flood is bounded-queue overload, not a faithful display cadence. Paced rows wake at 120 Hz.
Post-request windows include possible design publication/crossfades. Public API exposes no completion count.
No GPU drawing, desktop compositor, audio-device scheduling, or full-editor refresh/layout cost is measured.
Compare identical hardware, power/thermal state, background load, toolchain, flags, and hardening.
Record source revision with --label. No baseline comparison or arbitrary performance pass/fail limit is applied.
p99 needs more than --quick for stable tail evidence.)"
        "\n"
    );
    fmt::print(
        "Safety limit: {} seconds of callback/warmup work per row. Truncated rows retain actual counts and warn.\n",
        options.maximumWorkloadSeconds
    );
    std::vector<double> clockOverhead;
    clockOverhead.reserve(CLOCK_OVERHEAD_SAMPLES);
    for (int index = 0; index < CLOCK_OVERHEAD_SAMPLES; ++index) {
        const auto before = Clock::now();
        clockOverhead.push_back(Microseconds(Clock::now() - before).count());
    }
    const auto overhead = distribution(clockOverhead);
    fmt::print("empty clock pair us[p50/p99/max]={:.3f}/{:.3f}/{:.3f}\n", overhead.p50, overhead.p99, overhead.max);
}

/// Validates bounded integer overrides rather than silently accepting typos or impractical run sizes.
int boundedInteger(const std::string_view text, const int minimum, const int maximum)
{
    int result = 0;
    const auto* first = std::to_address(text.begin());
    const auto* end = std::to_address(text.end());
    const auto parsed = std::from_chars(first, end, result);
    if (parsed.ec != std::errc {} || parsed.ptr != end || result < minimum || result > maximum) {
        throw std::invalid_argument("Integer option is outside its documented range");
    }
    return result;
}

/// Parses workload controls. Quick defaults apply before explicit overrides, independent of argument ordering.
Options parseOptions(const std::span<const std::string_view> arguments)
{
    Options options;
    if (std::ranges::contains(arguments, "--quick")) {
        options.callbacks = QUICK_CALLBACKS;
        options.repetitions = QUICK_REPETITIONS;
        options.refreshes = REFRESH_RATE;
        options.componentAudioSeconds = 2.0;
        options.pacedSeconds = QUICK_PACED_SECONDS;
    }
    bool processorOnly = false;
    bool componentsOnly = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        const auto value = [&] -> std::string_view {
            if (++index >= arguments.size()) {
                throw std::invalid_argument("Missing value for " + std::string(argument));
            }
            return arguments[index];
        };
        if (argument == "--quick") {
            continue;
        }
        if (argument == "--callbacks") {
            options.callbacks = boundedInteger(value(), MINIMUM_CALLBACKS, MAXIMUM_CALLBACKS);
        } else if (argument == "--repetitions") {
            options.repetitions = boundedInteger(value(), 1, MAXIMUM_REPETITIONS);
        } else if (argument == "--label") {
            options.label = value();
        } else if (argument == "--focused") {
            options.focused = true;
        } else if (argument == "--refresh-settings-only") {
            options.refreshSettingsOnly = true;
        } else if (argument == "--no-worker") {
            options.worker = false;
        } else if (argument == "--processor-only") {
            processorOnly = true;
            options.components = false;
        } else if (argument == "--components-only") {
            componentsOnly = true;
            options.processor = false;
        } else {
            throw std::invalid_argument("Unknown option: " + std::string(argument));
        }
    }
    if (processorOnly && componentsOnly) {
        throw std::invalid_argument("--processor-only and --components-only are mutually exclusive");
    }
    return options;
}

/// Lists workload selection and reproducibility options without initializing JUCE or touching the desktop.
void printHelp()
{
    fmt::print(
        R"(Usage: ImageControlBenchmark [--quick] [--callbacks N] [--repetitions N] [--label TEXT]
                             [--processor-only | --components-only] [--no-worker] [--focused]
                             [--refresh-settings-only]
  --quick            Shorter runs, retaining the selected coverage (not stable tail evidence)
  --focused          Only small steady solo/delta callbacks and spectrogram component probes
  --callbacks N      Measured callbacks per steady repetition, 32..16384 (default 512, quick 128)
  --repetitions N    Warmed repetitions, 1..10 (default 3, quick 2)
  --label TEXT       Record revision/build/baseline identity in the output
  --processor-only  Skip isolated component probes
  --components-only Skip callback probes and paced automation
  --no-worker       Skip concurrent analysis rows, retaining the always-drained FIFO reference
  --refresh-settings-only
                    Only peerless editor settings refresh, with the worker stopped
  -h, --help        Show this help and exit
Measurements require Release with -DAUDIOBATCH_NATIVE_CPU=ON. CI builds support --help only.
Keep hardware, background load, compiler flags, and hardening identical across comparisons.)"
        "\n"
    );
}
}  // namespace
}  // namespace esgrove::image_control::benchmark

/// Runs bounded performance probes and reports invalid options without entering a GUI event loop.
int main(const int argc, char* argv[])
{
    try {
        std::vector<std::string_view> arguments;
        for (const auto* argument : std::span(argv, static_cast<std::size_t>(argc)).subspan(1)) {
            arguments.emplace_back(argument);
        }
        if (std::ranges::contains(arguments, "--help") || std::ranges::contains(arguments, "-h")) {
            esgrove::image_control::benchmark::printHelp();
            return 0;
        }
        const auto options = esgrove::image_control::benchmark::parseOptions(arguments);
        esgrove::image_control::benchmark::requireMeasurementBuild();
        const juce::ScopedJuceInitialiser_GUI juce;
        const auto start = esgrove::image_control::benchmark::Clock::now();
        esgrove::image_control::benchmark::printIdentity(options);
        if (options.refreshSettingsOnly) {
            esgrove::image_control::benchmark::timeEditorViewSettings(options);
            return 0;
        }
        if (options.processor) {
            if (!options.focused) {
                esgrove::image_control::benchmark::timeProcessorMatrix(options);
            }
            esgrove::image_control::benchmark::timeFocusedProcessor(options);
        }
        if (options.components && !options.focused) {
            fmt::print("\nIsolated components (these are not complete callback costs):\n");
            esgrove::image_control::benchmark::timeImager(
                options, esgrove::image_control::benchmark::CENTRED_PANS, "centred"
            );
            esgrove::image_control::benchmark::timeImager(options, esgrove::image_control::benchmark::PANS, "panned");
            for (const auto mode :
                 {
                     esgrove::image_control::benchmark::Mode::narrowed,
                     esgrove::image_control::benchmark::Mode::recoveryPartial,
                     esgrove::image_control::benchmark::Mode::recoveryAll,
                 })
            {
                esgrove::image_control::benchmark::timeImager(
                    options,
                    esgrove::image_control::benchmark::PANS,
                    esgrove::image_control::benchmark::modeName(mode),
                    esgrove::image_control::benchmark::modeWidths(mode),
                    esgrove::image_control::benchmark::modeRecoveries(mode)
                );
            }
            esgrove::image_control::benchmark::timeImagerSetters(options);
            esgrove::image_control::benchmark::timeSpectrogramAnalysis(options);
            esgrove::image_control::benchmark::timeSpectrogramOverlays(options);
            for (const bool changing : {false, true}) {
                for (const double persistenceSeconds :
                     {
                         esgrove::image_control::MIN_PERSISTENCE_SECONDS,
                         esgrove::image_control::DEFAULT_PERSISTENCE_SECONDS,
                         esgrove::image_control::MAX_PERSISTENCE_SECONDS,
                     })
                {
                    esgrove::image_control::benchmark::timeBandCorrelationMeters(options, changing, persistenceSeconds);
                }
            }
        }
        if (options.components) {
            fmt::print("\nFocused spectrogram components (no GPU drawing or full-editor refresh):\n");
            if (options.focused) {
                esgrove::image_control::benchmark::timeSpectrogramAnalysis(options);
                esgrove::image_control::benchmark::timeSpectrogramOverlays(options);
            }
            esgrove::image_control::benchmark::timeSpectrogramAnalyzerOnly(options);
            esgrove::image_control::benchmark::timeSpectrogramParticlesOnly(options);
            esgrove::image_control::benchmark::timeSpectrogramOverlays(
                options, esgrove::image_control::benchmark::OverlayHover::band
            );
            esgrove::image_control::benchmark::timeSpectrogramOverlays(
                options, esgrove::image_control::benchmark::OverlayHover::crossover
            );
        }
        fmt::print(
            "Elapsed wall seconds: {:.3f}\n",
            esgrove::image_control::benchmark::Seconds(esgrove::image_control::benchmark::Clock::now() - start).count()
        );
        return 0;
    } catch (const std::exception& error) {
        fmt::print(stderr, "ImageControlBenchmark: {}. Use --help for options.\n", error.what());
        return 1;
    }
}
