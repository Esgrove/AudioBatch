/// Measures public processor callback tails, paced crossover changes, and concurrent editor analysis workloads.
/// Shared noise, workload configuration, and quantile helpers also serve the isolated component probes.
#include "analysis/StereoSampleFifo.h"
#include "benchmarks/BenchmarkSupport.h"
#include "dsp/MultibandImager.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/SpectrogramRenderer.h"
#include "ui/SpectrogramWorker.h"
#include <fmt/base.h>
#include <string_view>

#include <juce_opengl/juce_opengl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace esgrove::image_control::benchmark
{
namespace
{
constexpr std::array<double, 4> SAMPLE_RATES {44'100.0, 48'000.0, 96'000.0, 192'000.0};
constexpr std::array<int, 4> FIXED_BLOCK_SIZES {32, 64, 256, 512};
/// Uneven blocks cross convolution partition boundaries, including a one-sample callback.
constexpr std::array<int, 8> IRREGULAR_BLOCK_SIZES {1, 31, 97, 255, 17, 509, 64, 257};
constexpr float NOISE_AMPLITUDE {0.3F};
constexpr double MICROSECONDS_PER_SECOND {1.0e6};
constexpr double MILLISECONDS_PER_SECOND {1000.0};
constexpr double TAIL_PERCENTILE_95 {0.95};
constexpr double TAIL_PERCENTILE_99 {0.99};
constexpr int WORKER_PRIMING_MILLISECONDS {20};
constexpr BandRecoveries PARTIAL_RECOVERY {0.0F, 0.75F, 0.0F, 0.5F};
/// Wall-clock spacing leaves several design-thread polls between requests.
constexpr double CROSSOVER_CHANGE_SECONDS {0.08};
/// Includes delayed publication and a partition crossfade, not just the requesting callback.
constexpr double POST_REQUEST_SECONDS {0.06};
constexpr std::array<Crossovers, 2> AUTOMATED_CROSSOVERS {
    Crossovers {160.0F, 900.0F, 7200.0F},
    Crossovers {70.0F, 350.0F, 3200.0F},
};
constexpr std::array<Mode, 10> MODES {
    Mode::neutral,
    Mode::centred,
    Mode::panned,
    Mode::delta,
    Mode::bypass,
    Mode::hostBypass,
    Mode::mono,
    Mode::narrowed,
    Mode::recoveryPartial,
    Mode::recoveryAll,
};

/// A zero fixed size selects the irregular block sequence.
struct BlockPattern {
    /// Keeps the output machine-readable while identifying the variable-size sequence.
    [[nodiscard]] std::string name() const
    {
        return fixedSize > 0 ? std::to_string(fixedSize) : "irregular";
    }

    /// Returns the next host block size without allocating inside the measurement loop.
    [[nodiscard]] int at(const std::size_t index) const
    {
        return fixedSize > 0 ? fixedSize : IRREGULAR_BLOCK_SIZES[index % IRREGULAR_BLOCK_SIZES.size()];
    }

    int fixedSize {0};
};

/// Callback durations are paired with their own block's deadline before percentiles are computed.
struct Samples {
    /// Records only callback execution, excluding input copies, automation setters, and pacing sleeps.
    void add(const double elapsed, const int blockSize, const double sampleRate)
    {
        const double deadline = static_cast<double>(blockSize) / sampleRate * MICROSECONDS_PER_SECOND;
        microseconds.push_back(elapsed);
        deadlineRatios.push_back(elapsed / deadline);
        frames += static_cast<std::uint64_t>(blockSize);
        totalMicroseconds += elapsed;
        if (elapsed > deadline) {
            ++overDeadline;
        }
    }

    /// Reserves storage before the measured loop so recording results cannot reallocate.
    void reserve(const std::size_t count)
    {
        microseconds.reserve(count);
        deadlineRatios.reserve(count);
    }

    std::vector<double> microseconds;
    std::vector<double> deadlineRatios;
    std::uint64_t frames {0};
    double totalMicroseconds {0.0};
    std::size_t overDeadline {0};
};

/// Accumulates callback observations across warmed repetitions, excluding setup and scheduling work.
struct CallbackResults {
    Samples samples;
    Samples postRequest;
    std::vector<double> repetitionP99;
    double checksum {0.0};
    double maximumStartLateness {0.0};
    std::size_t requests {0};
    std::size_t lateStarts {0};
    bool truncated {false};
};

/// Keeps each repetition's audio timeline independent of the workload's pooled observations.
struct CallbackSchedule {
    Clock::time_point start {Clock::now()};
    Clock::time_point nextRefresh {start};
    Clock::time_point lastRequest {start - std::chrono::seconds(1)};
    double audioSeconds {0.0};
    double nextChangeSeconds {0.0};
    double nextDisplayAudioSeconds {0.0};
};

/// Owns the unattached GL context before the renderer, and stops the FIFO's only consumer before either dies.
/// The production renderer bounds its pending particle queue even though no GPU consumes it here.
class AnalysisLoad
{
public:
    /// Starts the same worker as the editor without constructing an editor or a desktop peer.
    AnalysisLoad(ImageControlProcessor& processor, const double sampleRate) :
        renderer(context),
        worker(processor.getAnalyzerFifo(), renderer)
    {
        SpectrogramAnalysisSettings settings;
        settings.sampleRate = sampleRate;
        worker.setSettings(settings);
        worker.start();
    }

    /// Joins analysis before the processor releases its DSP and FIFO.
    ~AnalysisLoad()
    {
        worker.stop();
    }

    AnalysisLoad(const AnalysisLoad&) = delete;
    AnalysisLoad(AnalysisLoad&&) = delete;
    AnalysisLoad& operator=(const AnalysisLoad&) = delete;
    AnalysisLoad& operator=(AnalysisLoad&&) = delete;

    /// Observes actual submitted particles so a run can disclose missing concurrent analysis.
    [[nodiscard]] std::size_t pendingParticles()
    {
        return renderer.pendingParticleCount();
    }

    /// Hands off the CPU side of an editor refresh. No OpenGL calls or context attachment occur.
    void refresh()
    {
        renderer.submitFrame(
            1.0 / REFRESH_RATE,
            DEFAULT_PERSISTENCE_SECONDS,
            juce::Rectangle<float>(0.0F, 0.0F, static_cast<float>(VIEW_WIDTH), static_cast<float>(VIEW_HEIGHT)),
            SpectrogramRenderMode::particles
        );
        worker.wake();
    }

private:
    juce::OpenGLContext context;
    SpectrogramRenderer renderer;
    SpectrogramWorker worker;
};

/// Uses the real parameter interface outside callback timing, not raw atomics or a DSP-only surrogate.
void setParameter(ImageControlProcessor& processor, const juce::String& parameterId, const float value)
{
    // Non-owning parameter observer, owned by the processor's tree throughout this synchronous call.
    auto* parameter = processor.getParameters().getParameter(parameterId);
    if (parameter == nullptr) {
        throw std::runtime_error("Missing benchmark parameter: " + parameterId.toStdString());
    }
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

/// Applies one stable workload through host parameters and the editor's correlation control.
void configureProcessor(ImageControlProcessor& processor, const Mode mode, const bool meters)
{
    const auto& widths = modeWidths(mode);
    const auto& recoveries = modeRecoveries(mode);
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const bool neutral = mode == Mode::neutral;
        setParameter(processor, widthParameterId(band), widths[band]);
        setParameter(processor, recoverParameterId(band), recoveries[band] * PERCENT_PER_UNIT);
        setParameter(
            processor, midGainParameterId(band), neutral ? 0.0F : juce::Decibels::gainToDecibels(MID_GAINS[band])
        );
        setParameter(
            processor, sideGainParameterId(band), neutral ? 0.0F : juce::Decibels::gainToDecibels(SIDE_GAINS[band])
        );
        const bool pan = mode != Mode::neutral && mode != Mode::centred;
        setParameter(processor, panParameterId(band), pan ? PANS[band] * PAN_MAX_PERCENT : 0.0F);
        const bool solo = (mode == Mode::solo || mode == Mode::soloDelta) && band == 1;
        setParameter(processor, soloParameterId(band), solo ? 1.0F : 0.0F);
    }
    setParameter(processor, OUTPUT_GAIN_ID, mode == Mode::neutral ? 0.0F : juce::Decibels::gainToDecibels(OUTPUT_GAIN));
    setParameter(processor, BYPASS_ID, mode == Mode::bypass ? 1.0F : 0.0F);
    setParameter(processor, DELTA_ID, mode == Mode::delta || mode == Mode::soloDelta ? 1.0F : 0.0F);
    setParameter(processor, MONO_ID, mode == Mode::mono ? 1.0F : 0.0F);
    processor.setCorrelationEnabled(meters);
}

/// Prints pooled callback tails and per-block deadline ratios, plus variation across repetition p99s.
void printCallbacks(
    const Samples& samples,
    const double sampleRate,
    const std::span<const double> repetitionP99,
    const double checksum
)
{
    const auto times = distribution(samples.microseconds);
    const auto ratios = distribution(samples.deadlineRatios);
    const auto repeats = distribution(std::vector<double>(repetitionP99.begin(), repetitionP99.end()));
    const double audioSeconds = static_cast<double>(samples.frames) / sampleRate;
    fmt::print(
        "n={} us[p50/p95/p99/max]={:.3f}/{:.3f}/{:.3f}/{:.3f} "
        "deadline[x]={:.4f}/{:.4f}/{:.4f}/{:.4f} over={}/{} callback_ms/audio_s={:.3f} rep_p99[min/max]={:.3f}/{:.3f} "
        "check={:.6f}\n",
        samples.microseconds.size(),
        times.p50,
        times.p95,
        times.p99,
        times.max,
        ratios.p50,
        ratios.p95,
        ratios.p99,
        ratios.max,
        samples.overDeadline,
        samples.microseconds.size(),
        audioSeconds > 0.0 ? samples.totalMicroseconds / MICROSECONDS_PER_MILLISECOND / audioSeconds : 0.0,
        repetitionP99.empty() ? 0.0 : *std::ranges::min_element(repetitionP99),
        repeats.max,
        checksum
    );
}

/// Converts audio intervals to the monotonic clock without quantizing them to whole milliseconds.
Clock::duration durationFromSeconds(const double seconds)
{
    return std::chrono::duration_cast<Clock::duration>(Seconds(seconds));
}

/// Warms history and gain settling, then primes the optional worker before measuring concurrent callbacks.
/// The optional worker is owned by timeProcessor and remains valid throughout this synchronous call.
template<typename Prepare, typename Callback, typename Consume, typename Refresh>
std::size_t warmProcessor(
    const int warmupFrames,
    const int repetition,
    const OptionalRef<AnalysisLoad> load,
    const Prepare& prepare,
    const Callback& callback,
    const Consume& consume,
    const Refresh& refresh
)
{
    int warmedFrames = 0;
    while (warmedFrames < warmupFrames) {
        warmedFrames += prepare(0);
        callback();
        consume(false);
        refresh();
    }
    if (!load) {
        return 0;
    }
    const auto prime = [&] {
        prepare(MAXIMUM_BLOCK_SIZE);
        callback();
        refresh();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    const auto primingDeadline = Clock::now() + std::chrono::milliseconds(WORKER_PRIMING_MILLISECONDS);
    while (Clock::now() < primingDeadline) {
        prime();
    }
    if (repetition == 0) {
        const auto startupDeadline = Clock::now() + std::chrono::seconds(1);
        while (load->get().pendingParticles() == 0 && Clock::now() < startupDeadline) {
            prime();
        }
        return load->get().pendingParticles();
    }
    return 0;
}

/// Paces callback starts and submits real parameter requests outside the measured callback interval.
void paceProcessor(
    ImageControlProcessor& processor,
    CallbackSchedule& schedule,
    CallbackResults& results,
    const double blockSeconds
)
{
    const auto target = schedule.start + durationFromSeconds(schedule.audioSeconds);
    std::this_thread::sleep_until(target);
    const double lateness = Microseconds(Clock::now() - target).count();
    results.maximumStartLateness = std::max(results.maximumStartLateness, lateness);
    if (lateness > blockSeconds * MICROSECONDS_PER_SECOND) {
        ++results.lateStarts;
    }
    if (schedule.audioSeconds >= schedule.nextChangeSeconds) {
        const auto& crossovers = AUTOMATED_CROSSOVERS[results.requests % AUTOMATED_CROSSOVERS.size()];
        for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
            setParameter(processor, crossoverParameterId(crossover), crossovers[crossover]);
        }
        ++results.requests;
        schedule.nextChangeSeconds += CROSSOVER_CHANGE_SECONDS;
        schedule.lastRequest = Clock::now();
    }
}

/// Runs one warmed repetition, preserving pooled tails and the real-time post-request window.
template<typename Prepare, typename Callback, typename Consume, typename Refresh>
void measureProcessor(
    const Options& options,
    const double sampleRate,
    const bool automate,
    const std::size_t maximumCallbacks,
    const Clock::time_point workloadDeadline,
    CallbackResults& results,
    const Prepare& prepare,
    const Callback& callback,
    const Consume& consume,
    const Refresh& refresh
)
{
    std::vector<double> currentTimes;
    currentTimes.reserve(maximumCallbacks);
    CallbackSchedule schedule;
    std::size_t measured = 0;
    while (automate ? schedule.audioSeconds < options.pacedSeconds : measured < maximumCallbacks) {
        if (Clock::now() >= workloadDeadline) {
            results.truncated = true;
            break;
        }
        const int size = prepare(schedule, results);
        const auto before = Clock::now();
        callback();
        const auto after = Clock::now();
        const double elapsed = Microseconds(after - before).count();
        results.samples.add(elapsed, size, sampleRate);
        currentTimes.push_back(elapsed);
        if (automate && Seconds(before - schedule.lastRequest).count() <= POST_REQUEST_SECONDS) {
            results.postRequest.add(elapsed, size, sampleRate);
        }
        consume(true);
        schedule.audioSeconds += static_cast<double>(size) / sampleRate;
        ++measured;
        // Flood runs wake at simulated display intervals. Paced runs use wall-clock display intervals.
        const bool displayDue
            = automate ? after >= schedule.nextRefresh : schedule.audioSeconds >= schedule.nextDisplayAudioSeconds;
        if (displayDue) {
            refresh();
            schedule.nextRefresh = after + durationFromSeconds(1.0 / REFRESH_RATE);
            schedule.nextDisplayAudioSeconds = schedule.audioSeconds + 1.0 / REFRESH_RATE;
        }
    }
    results.repetitionP99.push_back(distribution(std::move(currentTimes)).p99);
}

/// Reports pooled observations and discloses truncated work or unconfirmed concurrent analysis.
void printProcessor(
    const Options& options,
    const double sampleRate,
    const BlockPattern pattern,
    const Mode mode,
    const bool meters,
    const bool concurrentAnalysis,
    const bool automate,
    const std::size_t particles,
    const CallbackResults& results
)
{
    const std::string_view fifo = concurrentAnalysis ? "worker-flood" : "drain";
    fmt::print(
        "callback kind={} rate={:.0f} block={} mode={} meter={} fifo={} particles={} ",
        automate ? "paced-crossovers" : "unpaced",
        sampleRate,
        pattern.name(),
        modeName(mode),
        meters ? "on" : "off",
        concurrentAnalysis && automate ? "worker-paced" : fifo,
        particles
    );
    printCallbacks(results.samples, sampleRate, results.repetitionP99, results.checksum);
    if (automate) {
        fmt::print(
            "  post-request-window={:.0f}ms requests={} start_late_blocks={} max_start_late_us={:.3f} ",
            POST_REQUEST_SECONDS * MILLISECONDS_PER_SECOND,
            results.requests,
            results.lateStarts,
            results.maximumStartLateness
        );
        printCallbacks(results.postRequest, sampleRate, {}, 0.0);
    }
    if (results.truncated) {
        fmt::print("  WARNING: {}s workload limit. Counts/repetitions incomplete.\n", options.maximumWorkloadSeconds);
    }
    if (concurrentAnalysis && particles == 0) {
        fmt::print("  WARNING: no particles observed. Concurrent analysis was not confirmed for this workload.\n");
    }
}

/// Creates the optional concurrent analyzer, whose lifetime is limited to one measured processor workload.
std::unique_ptr<AnalysisLoad> createAnalysisLoad(
    ImageControlProcessor& processor,
    const double sampleRate,
    const bool concurrentAnalysis
)
{
    return concurrentAnalysis ? std::make_unique<AnalysisLoad>(processor, sampleRate) : nullptr;
}

/// Measures the public callback. A draining reference always leaves FIFO space. Worker runs may drop under overload.
void timeProcessor(
    const Options& options,
    const double sampleRate,
    const BlockPattern pattern,
    const Mode mode,
    const bool meters,
    const bool concurrentAnalysis,
    const bool automate
)
{
    ImageControlProcessor processor;
    processor.setRateAndBufferSizeDetails(sampleRate, MAXIMUM_BLOCK_SIZE);
    processor.setNonRealtime(false);
    configureProcessor(processor, mode, meters);
    processor.prepareToPlay(sampleRate, MAXIMUM_BLOCK_SIZE);
    auto load = createAnalysisLoad(processor, sampleRate, concurrentAnalysis);
    const OptionalRef<AnalysisLoad> analysisLoad = load ? OptionalRef<AnalysisLoad>(*load) : std::nullopt;
    const auto left = makeNoise(MAXIMUM_BLOCK_SIZE, 1);
    const auto right = makeNoise(MAXIMUM_BLOCK_SIZE, 2);
    std::array<float, MAXIMUM_BLOCK_SIZE> discardedLeft {};
    std::array<float, MAXIMUM_BLOCK_SIZE> discardedRight {};
    juce::AudioBuffer<float> block(2, MAXIMUM_BLOCK_SIZE);
    juce::MidiBuffer midi;
    CallbackResults results;
    const int minimumBlock = std::max(pattern.fixedSize, 1);
    const auto maximumCallbacks = automate
        ? static_cast<std::size_t>(std::ceil(options.pacedSeconds * sampleRate / minimumBlock)) + 1
        : static_cast<std::size_t>(options.callbacks);
    const auto capacity = maximumCallbacks * static_cast<std::size_t>(options.repetitions);
    results.samples.reserve(capacity);
    results.postRequest.reserve(automate ? capacity : 0);
    results.repetitionP99.reserve(static_cast<std::size_t>(options.repetitions));
    std::size_t blockIndex = 0;
    std::size_t particles = 0;
    const auto workloadDeadline = Clock::now() + std::chrono::seconds(options.maximumWorkloadSeconds);
    const auto prepareBlock = [&](const int fixedSize) {
        const int size = fixedSize > 0 ? fixedSize : pattern.at(blockIndex++);
        block.setSize(2, size, false, false, true);
        block.copyFrom(0, 0, left.data(), size);
        block.copyFrom(1, 0, right.data(), size);
        return size;
    };
    const auto prepareMeasured = [&](CallbackSchedule& schedule, CallbackResults& observations) {
        if (automate) {
            paceProcessor(processor, schedule, observations, static_cast<double>(pattern.at(blockIndex)) / sampleRate);
        }
        return prepareBlock(0);
    };
    const auto callback = [&] {
        if (mode == Mode::hostBypass) {
            processor.processBlockBypassed(block, midi);
        } else {
            processor.processBlock(block, midi);
        }
    };
    const auto consume = [&](const bool measured) {
        if (load == nullptr) {
            static_cast<void>(processor.getAnalyzerFifo().pop(discardedLeft, discardedRight));
        }
        results.checksum += measured ? static_cast<double>(block.getSample(0, block.getNumSamples() - 1)) : 0.0;
    };
    const auto refresh = [&] {
        if (load != nullptr) {
            load->refresh();
        }
    };
    for (int repetition = 0; repetition < options.repetitions; ++repetition) {
        if (Clock::now() >= workloadDeadline) {
            results.truncated = true;
            break;
        }
        const int warmupFrames = processor.getLatencySamples()
            + static_cast<int>(std::ceil(sampleRate * GAIN_SMOOTHING_SECONDS)) + MAXIMUM_BLOCK_SIZE;
        particles = std::max(
            particles, warmProcessor(warmupFrames, repetition, analysisLoad, prepareBlock, callback, consume, refresh)
        );
        measureProcessor(
            options,
            sampleRate,
            automate,
            maximumCallbacks,
            workloadDeadline,
            results,
            prepareMeasured,
            callback,
            consume,
            refresh
        );
        for (const float correlation : processor.getBandCorrelations()) {
            if (std::isfinite(correlation)) {
                results.checksum += static_cast<double>(correlation);
            }
        }
    }
    if (load != nullptr) {
        particles = std::max(particles, load->pendingParticles());
    }
    load.reset();
    processor.releaseResources();
    printProcessor(options, sampleRate, pattern, mode, meters, concurrentAnalysis, automate, particles, results);
}

/// Compares meters and FIFO consumers without changing the enclosing mode/rate/block workload.
void timeProcessorVariants(
    const Options& options,
    const double sampleRate,
    const int blockSize,
    const Mode mode,
    const bool automate
)
{
    for (const bool meters : {false, true}) {
        for (const bool worker : {false, true}) {
            if (!worker || options.worker) {
                timeProcessor(options, sampleRate, {.fixedSize = blockSize}, mode, meters, worker, automate);
            }
        }
    }
}
}  // namespace

/// Returns fixed-seed noise. The standard-library distribution is repeatable within the same toolchain.
std::vector<float> makeNoise(const std::size_t samples, const unsigned seed)
{
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> distribution(-NOISE_AMPLITUDE, NOISE_AMPLITUDE);
    std::vector<float> noise(samples);
    for (auto& sample : noise) {
        sample = distribution(generator);
    }
    return noise;
}

/// Describes the parameter configuration independently of the sample-rate/block-size matrix.
std::string_view modeName(const Mode mode)
{
    switch (mode) {
        case Mode::neutral:
            return "neutral";
        case Mode::centred:
            return "centred";
        case Mode::panned:
            return "panned";
        case Mode::delta:
            return "delta";
        case Mode::bypass:
            return "bypass-param";
        case Mode::hostBypass:
            return "bypass-host";
        case Mode::mono:
            return "mono";
        case Mode::narrowed:
            return "narrow-recovery-off";
        case Mode::recoveryPartial:
            return "narrow-recovery-partial";
        case Mode::recoveryAll:
            return "narrow-recovery-all";
        case Mode::solo:
            return "solo";
        case Mode::soloDelta:
            return "solo-delta";
    }
    throw std::logic_error("Unknown processing mode");
}

/// Keeps baseline workloads at unity width and narrows only the recovery-comparison modes.
const BandWidths& modeWidths(const Mode mode)
{
    if (mode == Mode::narrowed || mode == Mode::recoveryPartial || mode == Mode::recoveryAll) {
        return NARROW_WIDTHS;
    }
    return UNITY_WIDTHS;
}

/// Disables recovery explicitly, including for neutral and the narrowed recovery-off reference.
const BandRecoveries& modeRecoveries(const Mode mode)
{
    if (mode == Mode::recoveryPartial) {
        return PARTIAL_RECOVERY;
    }
    return mode == Mode::recoveryAll ? ALL_RECOVERY : RECOVERY_OFF;
}

/// Sorts a copy after measurement, leaving the hot recording loop constant-time.
Distribution distribution(std::vector<double> values)
{
    if (values.empty()) {
        return {};
    }
    std::ranges::sort(values);
    const auto percentile = [&values](const double fraction) {
        const auto rank = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size())));
        return values[std::max(std::size_t {1}, rank) - 1];
    };
    return {
        .p50 = percentile(0.5),
        .p95 = percentile(TAIL_PERCENTILE_95),
        .p99 = percentile(TAIL_PERCENTILE_99),
        .max = values.back(),
    };
}

/// Covers every steady mode/rate/block combination and paced shortest-fixed/irregular crossover requests.
void timeProcessorMatrix(const Options& options)
{
    fmt::print(
        "\nComplete processor callback matrix (stereo input, normal thread priority, no physical audio device):\n"
    );
    for (const double sampleRate : SAMPLE_RATES) {
        for (const int blockSize : std::array<int, 5> {32, 64, 256, 512, 0}) {
            for (const Mode mode : MODES) {
                timeProcessorVariants(options, sampleRate, blockSize, mode, false);
            }
        }
    }
    fmt::print(
        "\nPaced real-time crossover requests (original panned and recovery modes, shortest fixed/irregular):\n"
    );
    for (const double sampleRate : SAMPLE_RATES) {
        for (const int blockSize : {FIXED_BLOCK_SIZES.front(), 0}) {
            for (const Mode mode : {Mode::panned, Mode::narrowed, Mode::recoveryPartial, Mode::recoveryAll}) {
                timeProcessorVariants(options, sampleRate, blockSize, mode, true);
            }
        }
    }
}

/// Isolates settled nonneutral gains/pans and solo/delta at small callbacks without another rate/meter/worker matrix.
void timeFocusedProcessor(const Options& options)
{
    constexpr double sampleRate {48'000.0};
    fmt::print("\nFocused steady small callbacks (settled nonneutral gains/pans, meters off, drained FIFO):\n");
    for (const int blockSize : {32, 64}) {
        for (const Mode mode : {Mode::panned, Mode::solo, Mode::delta, Mode::soloDelta}) {
            timeProcessor(options, sampleRate, {.fixedSize = blockSize}, mode, false, false, false);
        }
    }
}
}  // namespace esgrove::image_control::benchmark
