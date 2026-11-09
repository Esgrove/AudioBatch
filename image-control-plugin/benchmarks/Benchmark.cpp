/// Measures callback tails, paced crossovers, concurrent analysis, imager throughput, and software display rendering.
/// Uses no window or GL context.
/// Observations are not pass/fail thresholds or improvement evidence without a baseline.

#include "analysis/SpatialAnalyzer.h"
#include "analysis/StereoSampleFifo.h"
#include "dsp/MultibandImager.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"
#include "ui/SpectrogramRenderer.h"
#include "ui/SpectrogramWorker.h"
#include <string_view>
#include <system_error>

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <print>
#include <random>
#include <ratio>
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
/// Monotonic wall clock used for callback durations and paced automation.
using Clock = std::chrono::steady_clock;
/// Floating-point seconds keep audio and wall-clock intervals in the same unit.
using Seconds = std::chrono::duration<double>;
/// Callback and display work are reported in microseconds.
using Microseconds = std::chrono::duration<double, std::micro>;

constexpr std::array<double, 4> SAMPLE_RATES {44'100.0, 48'000.0, 96'000.0, 192'000.0};
constexpr std::array<int, 4> FIXED_BLOCK_SIZES {32, 64, 256, 512};
/// Uneven blocks cross convolution partition boundaries, including a one-sample callback.
constexpr std::array<int, 8> IRREGULAR_BLOCK_SIZES {1, 31, 97, 255, 17, 509, 64, 257};
constexpr int MAXIMUM_BLOCK_SIZE {512};
constexpr int REFRESH_RATE {120};
constexpr int VIEW_WIDTH {1000};
constexpr int VIEW_HEIGHT {420};
constexpr float NOISE_AMPLITUDE {0.3F};
constexpr double MICROSECONDS_PER_SECOND {1.0e6};
constexpr double MILLISECONDS_PER_SECOND {1000.0};
constexpr double MICROSECONDS_PER_MILLISECOND {1000.0};
constexpr double TAIL_PERCENTILE_95 {0.95};
constexpr double TAIL_PERCENTILE_99 {0.99};
constexpr int WORKER_PRIMING_MILLISECONDS {20};
constexpr int MAXIMUM_REPETITIONS {10};
constexpr double QUICK_PACED_SECONDS {0.16};
/// Wall-clock spacing leaves several design-thread polls between requests.
constexpr double CROSSOVER_CHANGE_SECONDS {0.08};
/// Includes delayed publication and a partition crossfade, not just the requesting callback.
constexpr double POST_REQUEST_SECONDS {0.06};
constexpr std::array<Crossovers, 2> AUTOMATED_CROSSOVERS {
    Crossovers {160.0F, 900.0F, 7200.0F},
    Crossovers {70.0F, 350.0F, 3200.0F},
};
constexpr BandGains MID_GAINS {0.9F, 1.0F, 1.1F, 0.8F};
constexpr BandGains SIDE_GAINS {0.5F, 1.2F, 1.4F, 0.7F};
constexpr BandPans PANS {0.0F, -0.3F, 0.2F, 0.5F};
constexpr BandPans CENTRED_PANS {};
constexpr BandWidths UNITY_WIDTHS {1.0F, 1.0F, 1.0F, 1.0F};
constexpr BandWidths NARROW_WIDTHS {0.0F, 0.25F, 0.5F, 0.75F};
constexpr BandRecoveries RECOVERY_OFF {};
constexpr BandRecoveries PARTIAL_RECOVERY {0.0F, 0.75F, 0.0F, 0.5F};
constexpr BandRecoveries ALL_RECOVERY {1.0F, 0.75F, 0.5F, 1.0F};
constexpr float OUTPUT_GAIN {0.9F};
/// The complete callback modes, including a host bypass independent of the bypass parameter.
enum class Mode : std::uint8_t {
    neutral,
    centred,
    panned,
    delta,
    bypass,
    hostBypass,
    mono,
    narrowed,
    recoveryPartial,
    recoveryAll,
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

/// Work limits can be reduced without removing any sample-rate, block-size, or processing-mode coverage.
struct Options {
    int callbacks {512};
    int repetitions {3};
    int refreshes {600};
    int maximumWorkloadSeconds {10};
    double componentAudioSeconds {15.0};
    double pacedSeconds {0.32};
    std::string label {"unlabelled"};
    bool components {true};
    bool processor {true};
    bool worker {true};
};

/// A zero fixed size selects the irregular block sequence.
struct BlockPattern {
    int fixedSize {0};

    /// Returns the next host block size without allocating inside the measurement loop.
    [[nodiscard]] int at(const std::size_t index) const
    {
        return fixedSize > 0 ? fixedSize : IRREGULAR_BLOCK_SIZES[index % IRREGULAR_BLOCK_SIZES.size()];
    }

    /// Keeps the output machine-readable while identifying the variable-size sequence.
    [[nodiscard]] std::string name() const
    {
        return fixedSize > 0 ? std::to_string(fixedSize) : "irregular";
    }
};

/// Callback durations are paired with their own block's deadline before percentiles are computed.
struct Samples {
    std::vector<double> microseconds;
    std::vector<double> deadlineRatios;
    std::uint64_t frames {0};
    double totalMicroseconds {0.0};
    std::size_t overDeadline {0};

    /// Reserves storage before the measured loop so recording results cannot reallocate.
    void reserve(const std::size_t count)
    {
        microseconds.reserve(count);
        deadlineRatios.reserve(count);
    }

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
};

/// Quantiles use nearest rank, and max remains an observed sample rather than an extrapolated bound.
struct Distribution {
    double p50 {0.0};
    double p95 {0.0};
    double p99 {0.0};
    double max {0.0};
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
    }
    throw std::logic_error("Unknown processing mode");
}

/// Keeps every original workload at unity width, with narrowing only in the added recovery comparisons.
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
        setParameter(processor, recoverParameterId(band), recoveries[band] * RECOVER_MAX_PERCENT);
        setParameter(
            processor, midGainParameterId(band), neutral ? 0.0F : juce::Decibels::gainToDecibels(MID_GAINS[band])
        );
        setParameter(
            processor, sideGainParameterId(band), neutral ? 0.0F : juce::Decibels::gainToDecibels(SIDE_GAINS[band])
        );
        const bool pan = mode != Mode::neutral && mode != Mode::centred;
        setParameter(processor, panParameterId(band), pan ? PANS[band] * PAN_MAX_PERCENT : 0.0F);
    }
    setParameter(processor, OUTPUT_GAIN_ID, mode == Mode::neutral ? 0.0F : juce::Decibels::gainToDecibels(OUTPUT_GAIN));
    setParameter(processor, BYPASS_ID, mode == Mode::bypass ? 1.0F : 0.0F);
    setParameter(processor, DELTA_ID, mode == Mode::delta ? 1.0F : 0.0F);
    setParameter(processor, MONO_ID, mode == Mode::mono ? 1.0F : 0.0F);
    processor.setCorrelationEnabled(meters);
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
    std::println(
        "n={} us[p50/p95/p99/max]={:.3f}/{:.3f}/{:.3f}/{:.3f} "
        "deadline[x]={:.4f}/{:.4f}/{:.4f}/{:.4f} over={}/{} callback_ms/audio_s={:.3f} rep_p99[min/max]={:.3f}/{:.3f} "
        "check={:.6f}",
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

/// Converts audio intervals to the monotonic clock without quantizing them to whole milliseconds.
Clock::duration durationFromSeconds(const double seconds)
{
    return std::chrono::duration_cast<Clock::duration>(Seconds(seconds));
}

/// Warms history and gain settling, then primes the optional worker before measuring concurrent callbacks.
/// The nullable worker observer is owned by timeProcessor and remains valid throughout this synchronous call.
template<typename Prepare, typename Callback, typename Consume, typename Refresh>
std::size_t warmProcessor(
    const int warmupFrames,
    const int repetition,
    AnalysisLoad* load,
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
    if (load == nullptr) {
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
        while (load->pendingParticles() == 0 && Clock::now() < startupDeadline) {
            prime();
        }
        return load->pendingParticles();
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
    std::print(
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
        std::print(
            "  post-request-window={:.0f}ms requests={} start_late_blocks={} max_start_late_us={:.3f} ",
            POST_REQUEST_SECONDS * MILLISECONDS_PER_SECOND,
            results.requests,
            results.lateStarts,
            results.maximumStartLateness
        );
        printCallbacks(results.postRequest, sampleRate, {}, 0.0);
    }
    if (results.truncated) {
        std::println("  WARNING: {}s workload limit. Counts/repetitions incomplete.", options.maximumWorkloadSeconds);
    }
    if (concurrentAnalysis && particles == 0) {
        std::println("  WARNING: no particles observed. Concurrent analysis was not confirmed for this workload.");
    }
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
    auto load = concurrentAnalysis ? std::make_unique<AnalysisLoad>(processor, sampleRate) : nullptr;
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
            particles, warmProcessor(warmupFrames, repetition, load.get(), prepareBlock, callback, consume, refresh)
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

/// Covers every steady mode/rate/block combination and paced shortest-fixed/irregular crossover requests.
void timeProcessorMatrix(const Options& options)
{
    std::println(
        "\nComplete processor callback matrix (stereo input, normal thread priority, no physical audio device):"
    );
    for (const double sampleRate : SAMPLE_RATES) {
        for (const int blockSize : std::array<int, 5> {32, 64, 256, 512, 0}) {
            for (const Mode mode : MODES) {
                timeProcessorVariants(options, sampleRate, blockSize, mode, false);
            }
        }
    }
    std::println(
        "\nPaced real-time crossover requests (original panned and recovery modes, shortest fixed/irregular):"
    );
    for (const double sampleRate : SAMPLE_RATES) {
        for (const int blockSize : {FIXED_BLOCK_SIZES.front(), 0}) {
            for (const Mode mode : {Mode::panned, Mode::narrowed, Mode::recoveryPartial, Mode::recoveryAll}) {
                timeProcessorVariants(options, sampleRate, blockSize, mode, true);
            }
        }
    }
}

/// Retains the original direct imager probe and adds recovery variants, including two input copies per block.
void timeImager(
    const Options& options,
    const BandPans& pans,
    const std::string_view name,
    const BandWidths& widths = UNITY_WIDTHS,
    const BandRecoveries& recoveries = RECOVERY_OFF
)
{
    constexpr double SAMPLE_RATE {48'000.0};
    MultibandImager imager;
    imager.setGains(MID_GAINS, SIDE_GAINS, OUTPUT_GAIN);
    imager.setPans(pans);
    imager.setWidths(widths);
    imager.setRecoveries(recoveries);
    imager.prepare(SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    const auto left = makeNoise(MAXIMUM_BLOCK_SIZE, 1);
    const auto right = makeNoise(MAXIMUM_BLOCK_SIZE, 2);
    juce::AudioBuffer<float> block(2, MAXIMUM_BLOCK_SIZE);
    const auto process = [&] {
        block.copyFrom(0, 0, left.data(), MAXIMUM_BLOCK_SIZE);
        block.copyFrom(1, 0, right.data(), MAXIMUM_BLOCK_SIZE);
        imager.process(block);
    };
    const int warmupBlocks = imager.getLatencySamples() / MAXIMUM_BLOCK_SIZE + REFRESH_RATE;
    for (int index = 0; index < warmupBlocks; ++index) {
        process();
    }
    const int blocks = static_cast<int>(std::ceil(options.componentAudioSeconds * SAMPLE_RATE / MAXIMUM_BLOCK_SIZE));
    const double audioSeconds = static_cast<double>(blocks * MAXIMUM_BLOCK_SIZE) / SAMPLE_RATE;
    std::vector<double> measurements;
    double checksum = 0.0;
    for (int repetition = 0; repetition < options.repetitions; ++repetition) {
        const auto start = Clock::now();
        for (int index = 0; index < blocks; ++index) {
            process();
        }
        const double elapsed = Microseconds(Clock::now() - start).count();
        measurements.push_back(elapsed / MICROSECONDS_PER_MILLISECOND / audioSeconds);
        checksum += static_cast<double>(block.getSample(0, MAXIMUM_BLOCK_SIZE - 1));
    }
    imager.release();
    const auto result = distribution(measurements);
    std::println(
        "imager mode={} rate=48000 block={} copy=included audio_s/rep={:.3f} "
        "ms/audio_s[min/p50/max]={:.3f}/{:.3f}/{:.3f} check={:.6f}",
        name,
        MAXIMUM_BLOCK_SIZE,
        audioSeconds,
        *std::ranges::min_element(measurements),
        result.p50,
        result.max,
        checksum
    );
}

/// Isolates repeated and changing Width/Recover target setters from convolution and the host parameter interface.
/// Each observation batches calls to reduce clock overhead. One untimed block advances ramps and observes output.
void timeImagerSetters(const Options& options)
{
    constexpr int SETTER_BATCH_SIZE {1024};
    constexpr double SAMPLE_RATE {48'000.0};
    for (const bool changing : {false, true}) {
        MultibandImager imager;
        imager.setWidths(NARROW_WIDTHS);
        imager.setRecoveries(ALL_RECOVERY);
        imager.prepare(SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
        const auto left = makeNoise(MAXIMUM_BLOCK_SIZE, 1);
        const auto right = makeNoise(MAXIMUM_BLOCK_SIZE, 2);
        juce::AudioBuffer<float> block(2, MAXIMUM_BLOCK_SIZE);
        const auto observe = [&] {
            block.copyFrom(0, 0, left.data(), MAXIMUM_BLOCK_SIZE);
            block.copyFrom(1, 0, right.data(), MAXIMUM_BLOCK_SIZE);
            imager.process(block);
            return static_cast<double>(block.getSample(0, MAXIMUM_BLOCK_SIZE - 1));
        };
        const auto setTargets = [&] {
            for (int call = 0; call < SETTER_BATCH_SIZE; ++call) {
                const bool alternate = changing && call % 2 == 0;
                imager.setWidths(alternate ? UNITY_WIDTHS : NARROW_WIDTHS);
                imager.setRecoveries(alternate ? RECOVERY_OFF : ALL_RECOVERY);
            }
        };
        const int warmupBlocks = imager.getLatencySamples() / MAXIMUM_BLOCK_SIZE + REFRESH_RATE;
        for (int warmup = 0; warmup < warmupBlocks; ++warmup) {
            setTargets();
            static_cast<void>(observe());
        }
        std::vector<double> measurements;
        measurements.reserve(
            static_cast<std::size_t>(options.refreshes) * static_cast<std::size_t>(options.repetitions)
        );
        double checksum = 0.0;
        for (int repetition = 0; repetition < options.repetitions; ++repetition) {
            for (int batch = 0; batch < options.refreshes; ++batch) {
                const auto start = Clock::now();
                setTargets();
                measurements.push_back(Microseconds(Clock::now() - start).count() / SETTER_BATCH_SIZE);
                checksum += observe();
            }
        }
        imager.release();
        const auto result = distribution(measurements);
        std::println(
            "imager setters=width+recover targets={} pairs/batch={} n={} "
            "us/pair[p50/p95/p99/max]={:.6f}/{:.6f}/{:.6f}/{:.6f} process=excluded check={:.6f}",
            changing ? "changing" : "steady",
            SETTER_BATCH_SIZE,
            measurements.size(),
            result.p50,
            result.p95,
            result.p99,
            result.max,
            checksum
        );
    }
}

/// Times analysis plus particle construction after priming the analyzer's longest history.
void timeSpectrogramAnalysis(const Options& options)
{
    constexpr double SAMPLE_RATE {48'000.0};
    SpatialAnalyzer analyzer;
    analyzer.prepare(SAMPLE_RATE);
    SpectrogramParticleBuilder particles;
    analyzer.setFrameCallback([&particles](const std::span<const SpectralPoint> points, const double frameSeconds) {
        particles.addFrame(points, frameSeconds);
    });
    const auto samplesPerRefresh = static_cast<std::size_t>(SAMPLE_RATE) / REFRESH_RATE;
    const auto left = makeNoise(samplesPerRefresh, 3);
    const auto right = makeNoise(samplesPerRefresh, 4);
    const auto process = [&] {
        particles.clear();
        analyzer.pushSamples(left, right);
    };
    for (int refresh = 0; refresh < REFRESH_RATE; ++refresh) {
        process();
    }
    std::vector<double> measurements;
    measurements.reserve(static_cast<std::size_t>(options.refreshes) * static_cast<std::size_t>(options.repetitions));
    std::size_t particleCount = 0;
    for (int repetition = 0; repetition < options.repetitions; ++repetition) {
        for (int refresh = 0; refresh < options.refreshes; ++refresh) {
            const auto start = Clock::now();
            process();
            measurements.push_back(Microseconds(Clock::now() - start).count());
            particleCount += particles.particles().size();
        }
    }
    const auto result = distribution(measurements);
    std::println(
        "analysis+particles rate=48000 refresh_hz={} n={} us[p50/p95/p99/max]={:.3f}/{:.3f}/{:.3f}/{:.3f} particles={}",
        REFRESH_RATE,
        measurements.size(),
        result.p50,
        result.p95,
        result.p99,
        result.max,
        particleCount
    );
}

/// Times CPU software overlay painting without constructing an editor or attaching a window/GL context.
void timeSpectrogramOverlays(const Options& options)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const juce::Image canvas(juce::Image::ARGB, VIEW_WIDTH, VIEW_HEIGHT, true);
    juce::Graphics graphics(canvas);
    const auto paint = [&] {
        view.advance(1.0 / REFRESH_RATE);
        view.paintEntireComponent(graphics, false);
    };
    for (int refresh = 0; refresh < REFRESH_RATE; ++refresh) {
        paint();
    }
    std::vector<double> measurements;
    measurements.reserve(static_cast<std::size_t>(options.refreshes) * static_cast<std::size_t>(options.repetitions));
    for (int repetition = 0; repetition < options.repetitions; ++repetition) {
        for (int refresh = 0; refresh < options.refreshes; ++refresh) {
            const auto start = Clock::now();
            paint();
            measurements.push_back(Microseconds(Clock::now() - start).count());
        }
    }
    const auto result = distribution(measurements);
    std::println(
        "overlay software={}x{} refresh_hz={} n={} us[p50/p95/p99/max]={:.3f}/{:.3f}/{:.3f}/{:.3f}",
        VIEW_WIDTH,
        VIEW_HEIGHT,
        REFRESH_RATE,
        measurements.size(),
        result.p50,
        result.p95,
        result.p99,
        result.max
    );
}

/// Times four display-history updates and their software paint at a simulated, unpaced editor refresh cadence.
/// Input generation, canvas clearing, setup/layout, and observable history/pixel checksums are outside the clock pair.
void timeBandCorrelationMeters(const Options& options, const bool changing, const double persistenceSeconds)
{
    constexpr int METER_WIDTH {VIEW_WIDTH / static_cast<int>(BAND_COUNT)};
    constexpr double REFRESH_SECONDS {1.0 / REFRESH_RATE};
    constexpr std::array<float, BAND_COUNT> STEADY_VALUES {-0.8F, -0.2F, 0.3F, 0.9F};
    const int warmupRefreshes = static_cast<int>(std::ceil(MAX_PERSISTENCE_SECONDS * REFRESH_RATE)) + 1;
    std::array<std::array<float, BAND_COUNT>, REFRESH_RATE> readings {};
    for (std::size_t refresh = 0; refresh < readings.size(); ++refresh) {
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            const double phase = static_cast<double>(refresh) / REFRESH_RATE
                + static_cast<double>(band) / static_cast<double>(BAND_COUNT);
            readings[refresh][band] = changing
                ? static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * phase))
                : STEADY_VALUES[band];
        }
    }
    juce::Component row;
    row.setSize(VIEW_WIDTH, BAND_CORRELATION_HEIGHT);
    std::array<std::unique_ptr<BandCorrelationMeter>, BAND_COUNT> meters;
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        meters[band] = std::make_unique<BandCorrelationMeter>(bandName(band));
        meters[band]->setBounds(static_cast<int>(band) * METER_WIDTH, 0, METER_WIDTH, BAND_CORRELATION_HEIGHT);
        row.addAndMakeVisible(*meters[band]);
    }
    juce::Image canvas(juce::Image::ARGB, VIEW_WIDTH, BAND_CORRELATION_HEIGHT, true, juce::SoftwareImageType {});
    juce::Graphics graphics(canvas);
    const auto updateAndPaint = [&](const std::array<float, BAND_COUNT>& values) {
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            meters[band]->setCorrelation(values[band], REFRESH_SECONDS, persistenceSeconds);
        }
        row.paintEntireComponent(graphics, false);
    };
    std::vector<double> measurements;
    measurements.reserve(static_cast<std::size_t>(options.refreshes) * static_cast<std::size_t>(options.repetitions));
    double checksum = 0.0;
    std::uint64_t pixelChecksum = 0;
    for (int repetition = 0; repetition < options.repetitions; ++repetition) {
        for (const auto& meter : meters) {
            meter->clearHistory();
        }
        // Fill the maximum retained history even for the shortest selected Persistence and --quick runs.
        for (int refresh = 0; refresh < warmupRefreshes; ++refresh) {
            canvas.clear(canvas.getBounds());
            updateAndPaint(readings[static_cast<std::size_t>(refresh) % readings.size()]);
        }
        for (int refresh = 0; refresh < options.refreshes; ++refresh) {
            const auto& values = readings[static_cast<std::size_t>(warmupRefreshes + refresh) % readings.size()];
            canvas.clear(canvas.getBounds());
            const auto start = Clock::now();
            updateAndPaint(values);
            measurements.push_back(Microseconds(Clock::now() - start).count());
            for (std::size_t band = 0; band < BAND_COUNT; ++band) {
                checksum += static_cast<double>(meters[band]->trendValue().value_or(0.0F));
                if (const auto range = meters[band]->recentRange()) {
                    checksum += static_cast<double>(range->getStart()) + static_cast<double>(range->getEnd());
                }
                const auto pixel = canvas.getPixelAt(
                    static_cast<int>(band) * METER_WIDTH + refresh % METER_WIDTH, refresh % BAND_CORRELATION_HEIGHT
                );
                pixelChecksum += pixel.getARGB();
            }
        }
    }
    const auto result = distribution(measurements);
    std::println(
        "correlation update+paint meters={} readings={} persistence_s={:.3f} software={}x{} refresh_hz={} "
        "warmup_s/rep={:.3f} n={} us[p50/p95/p99/max]={:.3f}/{:.3f}/{:.3f}/{:.3f} check={:.6f} pixels={}",
        BAND_COUNT,
        changing ? "changing" : "steady",
        persistenceSeconds,
        VIEW_WIDTH,
        BAND_CORRELATION_HEIGHT,
        REFRESH_RATE,
        static_cast<double>(warmupRefreshes) * REFRESH_SECONDS,
        measurements.size(),
        result.p50,
        result.p95,
        result.p99,
        result.max,
        checksum,
        pixelChecksum
    );
}

/// Prints reproducibility context without pretending runtime detection reveals all compiler/linker flags.
void printIdentity(const Options& options)
{
    std::println(
        "ImageControl performance observations | label={} | {}",
        options.label,
        juce::Time::getCurrentTime().toISO8601(true).toStdString()
    );
    std::println(
        "OS={} CPU={} vendor={} physical/logical={}/{} pointer_bits={}",
        juce::SystemStats::getOperatingSystemName().toStdString(),
        juce::SystemStats::getCpuModel().toStdString(),
        juce::SystemStats::getCpuVendor().toStdString(),
        juce::SystemStats::getNumPhysicalCpus(),
        juce::SystemStats::getNumCpus(),
        sizeof(void*) * 8
    );
#ifdef __clang__
    std::println("compiler=Clang {}", __clang_version__);
#elifdef _MSC_VER
    std::println("compiler=MSVC {} full={}", _MSC_VER, _MSC_FULL_VER);
#endif
    std::println(
        "C++={} JUCE={} JUCE_DEBUG={}",
        static_cast<long long>(__cplusplus),
        juce::SystemStats::getJUCEVersion().toStdString(),
#if JUCE_DEBUG
        true
#else
        false
#endif
    );
#ifdef _LIBCPP_VERSION
    std::println("stdlib=libc++ {}", _LIBCPP_VERSION);
#endif
#ifdef _LIBCPP_HARDENING_MODE
    std::println("libc++ hardening mode={}", _LIBCPP_HARDENING_MODE);
#endif
#ifdef _MSVC_STL_VERSION
    std::println("stdlib=MSVC STL {}", _MSVC_STL_VERSION);
#endif
#ifdef _MSVC_STL_HARDENING
    std::println("MSVC STL hardening={}", _MSVC_STL_HARDENING);
#endif
#ifdef __AVX2__
    std::println("compiled ISA: AVX2");
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    std::println("compiled ISA: NEON");
#endif
#ifdef __FMA__
    std::println("compiled ISA: FMA");
#endif
#if JUCE_DEBUG || !defined(NDEBUG)
    std::println("WARNING: assertions/Debug are enabled. Use an optimized Release build for performance evidence.");
#else
    std::println("NDEBUG=1. Verify CMAKE_BUILD_TYPE/configuration is Release. Non-Debug alone does not prove Release.");
#endif
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    std::println(
        "binary={} bytes={} modified={}",
        executable.getFullPathName().toStdString(),
        executable.getSize(),
        executable.getLastModificationTime().toISO8601(true).toStdString()
    );
    std::println(
        "repetitions={} callbacks/steady_rep={} paced_s/rep={:.3f} refreshes/component_rep={} worker={}",
        options.repetitions,
        options.callbacks,
        options.pacedSeconds,
        options.refreshes,
        options.worker
    );
    std::println(
        R"(Warmup: callback latency + gain settling + one maximum block per repetition. Components: one second or more.
Input: independent fixed-seed stereo noise, amplitude +/-.3. Irregular blocks: 1,31,97,255,17,509,64,257.
Callback timing excludes copying, parameter setters, checksums, FIFO pop, worker wake, and sleep.
Original modes explicitly set Width 1 and Recover Off. Narrowed rows share Widths 0/.25/.5/.75.
Recovery variants select Off, two bands, or all bands, each with meter/worker and moving-crossover coverage.
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
    );
    std::println(
        "Safety limit: {} seconds of callback/warmup work per row. Truncated rows retain actual counts and warn.",
        options.maximumWorkloadSeconds
    );
    std::vector<double> clockOverhead;
    clockOverhead.reserve(1024);
    for (int index = 0; index < 1024; ++index) {
        const auto before = Clock::now();
        clockOverhead.push_back(Microseconds(Clock::now() - before).count());
    }
    const auto overhead = distribution(clockOverhead);
    std::println("empty clock pair us[p50/p99/max]={:.3f}/{:.3f}/{:.3f}", overhead.p50, overhead.p99, overhead.max);
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
    if (std::ranges::find(arguments, "--quick") != arguments.end()) {
        options.callbacks = 128;
        options.repetitions = 2;
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
            options.callbacks = boundedInteger(value(), 32, 16'384);
        } else if (argument == "--repetitions") {
            options.repetitions = boundedInteger(value(), 1, MAXIMUM_REPETITIONS);
        } else if (argument == "--label") {
            options.label = value();
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
    std::println(R"(Usage: ImageControlBenchmark [--quick] [--callbacks N] [--repetitions N] [--label TEXT]
                             [--processor-only | --components-only] [--no-worker]
  --quick            Shorter runs, retaining the complete coverage matrix (not stable tail evidence)
  --callbacks N      Measured callbacks per steady repetition, 32..16384 (default 512, quick 128)
  --repetitions N    Warmed repetitions, 1..10 (default 3, quick 2)
  --label TEXT       Record revision/build/baseline identity in the output
  --processor-only  Skip isolated component probes
  --components-only Skip callback matrix and paced automation
  --no-worker       Skip concurrent analysis rows, retaining the always-drained FIFO reference
Use Release. Keep hardware, background load, compiler flags, and hardening identical across comparisons.)");
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
        if (std::ranges::find(arguments, "--help") != arguments.end()) {
            esgrove::image_control::benchmark::printHelp();
            return 0;
        }
        const auto options = esgrove::image_control::benchmark::parseOptions(arguments);
        const juce::ScopedJuceInitialiser_GUI juce;
        const auto start = esgrove::image_control::benchmark::Clock::now();
        esgrove::image_control::benchmark::printIdentity(options);
        if (options.processor) {
            esgrove::image_control::benchmark::timeProcessorMatrix(options);
        }
        if (options.components) {
            std::println("\nIsolated components (these are not complete callback costs):");
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
        std::println(
            "Elapsed wall seconds: {:.3f}",
            esgrove::image_control::benchmark::Seconds(esgrove::image_control::benchmark::Clock::now() - start).count()
        );
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "ImageControlBenchmark: {}. Use --help for options.", error.what());
        return 1;
    }
}
