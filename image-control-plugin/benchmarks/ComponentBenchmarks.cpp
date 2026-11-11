/// Measures isolated imager, analyzer, particle construction, and software component rendering workloads.
/// These CPU probes use no attached GL context or desktop peer and do not represent full callback costs.
#include "analysis/SpatialAnalyzer.h"
#include "benchmarks/BenchmarkSupport.h"
#include "dsp/MultibandImager.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"
#include <fmt/base.h>
#include <string_view>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace esgrove::image_control::benchmark
{
namespace
{
/// An analyzer frame copied during untimed capture and replayed by the particle-only probe.
struct CapturedSpectralFrame {
    SpectralFrameInfo info;
    std::vector<SpectralPoint> points;
};
}  // namespace

/// Measures centered, panned, and recovery imager variants, including two input copies per block.
void timeImager(
    const Options& options,
    const BandPans& pans,
    const std::string_view name,
    const BandWidths& widths,
    const BandRecoveries& recoveries
)
{
    constexpr double sampleRate {48'000.0};
    MultibandImager imager;
    imager.setGains(MID_GAINS, SIDE_GAINS, OUTPUT_GAIN);
    imager.setPans(pans);
    imager.setWidths(widths);
    imager.setRecoveries(recoveries);
    imager.prepare(sampleRate, 2, DEFAULT_CROSSOVERS);
    const auto left = makeNoise(MAXIMUM_BLOCK_SIZE, 1);
    const auto right = makeNoise(MAXIMUM_BLOCK_SIZE, 2);
    juce::AudioBuffer<float> block(2, MAXIMUM_BLOCK_SIZE);
    // Initialize storage off-clock so the checksum is defined even when no blocks are processed.
    block.clear();
    const auto process = [&] {
        block.copyFrom(0, 0, left.data(), MAXIMUM_BLOCK_SIZE);
        block.copyFrom(1, 0, right.data(), MAXIMUM_BLOCK_SIZE);
        imager.process(block);
    };
    const int warmupBlocks = imager.getLatencySamples() / MAXIMUM_BLOCK_SIZE + REFRESH_RATE;
    for (int index = 0; index < warmupBlocks; ++index) {
        process();
    }
    const int blocks = static_cast<int>(std::ceil(options.componentAudioSeconds * sampleRate / MAXIMUM_BLOCK_SIZE));
    const double audioSeconds = static_cast<double>(blocks * MAXIMUM_BLOCK_SIZE) / sampleRate;
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
    fmt::print(
        "imager mode={} rate=48000 block={} copy=included audio_s/rep={:.3f} "
        "ms/audio_s[min/p50/max]={:.3f}/{:.3f}/{:.3f} check={:.6f}\n",
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
    constexpr int setterBatchSize {1024};
    constexpr double sampleRate {48'000.0};
    for (const bool changing : {false, true}) {
        MultibandImager imager;
        imager.setWidths(NARROW_WIDTHS);
        imager.setRecoveries(ALL_RECOVERY);
        imager.prepare(sampleRate, 2, DEFAULT_CROSSOVERS);
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
            for (int call = 0; call < setterBatchSize; ++call) {
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
                measurements.push_back(Microseconds(Clock::now() - start).count() / setterBatchSize);
                checksum += observe();
            }
        }
        imager.release();
        const auto result = distribution(measurements);
        fmt::print(
            "imager setters=width+recover targets={} pairs/batch={} n={} "
            "us/pair[p50/p95/p99/max]={:.6f}/{:.6f}/{:.6f}/{:.6f} process=excluded check={:.6f}\n",
            changing ? "changing" : "steady",
            setterBatchSize,
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
    constexpr double sampleRate {48'000.0};
    SpatialAnalyzer analyzer;
    analyzer.prepare(sampleRate);
    SpectrogramParticleBuilder particles;
    analyzer.setFrameCallback([&particles](
                                  const std::span<const SpectralPoint> points, const SpectralFrameInfo& frame
                              ) { particles.addFrame(points, frame); });
    const auto samplesPerRefresh = static_cast<std::size_t>(sampleRate) / REFRESH_RATE;
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
    fmt::print(
        "analysis+particles rate=48000 refresh_hz={} n={} us[p50/p95/p99/max]={:.3f}/{:.3f}/{:.3f}/{:.3f} "
        "particles={}\n",
        REFRESH_RATE,
        measurements.size(),
        result.p50,
        result.p95,
        result.p99,
        result.max,
        particleCount
    );
}

/// Times the analyzer with a constant-time observing callback, excluding particle construction and frame copies.
void timeSpectrogramAnalyzerOnly(const Options& options)
{
    constexpr double sampleRate {48'000.0};
    SpatialAnalyzer analyzer;
    analyzer.prepare(sampleRate);
    std::size_t frameCount = 0;
    std::size_t pointCount = 0;
    double checksum = 0.0;
    analyzer.setFrameCallback([&](const std::span<const SpectralPoint> points, const SpectralFrameInfo& frame) {
        ++frameCount;
        pointCount += points.size();
        checksum += frame.frameSeconds;
        if (!points.empty()) {
            checksum += static_cast<double>(points.front().level);
        }
    });
    const auto samplesPerRefresh = static_cast<std::size_t>(sampleRate) / REFRESH_RATE;
    const auto left = makeNoise(samplesPerRefresh, 3);
    const auto right = makeNoise(samplesPerRefresh, 4);
    for (int refresh = 0; refresh < REFRESH_RATE; ++refresh) {
        analyzer.pushSamples(left, right);
    }
    frameCount = 0;
    pointCount = 0;
    checksum = 0.0;
    std::vector<double> measurements;
    measurements.reserve(static_cast<std::size_t>(options.refreshes) * static_cast<std::size_t>(options.repetitions));
    for (int repetition = 0; repetition < options.repetitions; ++repetition) {
        for (int refresh = 0; refresh < options.refreshes; ++refresh) {
            const auto start = Clock::now();
            analyzer.pushSamples(left, right);
            measurements.push_back(Microseconds(Clock::now() - start).count());
        }
    }
    const auto result = distribution(measurements);
    fmt::print(
        "analyzer-only rate=48000 refresh_hz={} n={} us[p50/p95/p99/max]={:.3f}/{:.3f}/{:.3f}/{:.3f} "
        "frames={} points={} check={:.6f}\n",
        REFRESH_RATE,
        measurements.size(),
        result.p50,
        result.p95,
        result.p99,
        result.max,
        frameCount,
        pointCount,
        checksum
    );
}

/// Replays owned frames captured from the warmed real analyzer, preserving tier identity and simultaneous deadlines.
/// Seed frames retain the analyzer's since-clear clock, and each replay cycle advances it by the capture duration.
/// Analysis, frame copies, capture storage, and result checksums are outside the particle-only clock pair.
void timeSpectrogramParticlesOnly(const Options& options)
{
    constexpr double sampleRate {48'000.0};
    constexpr auto samplesPerRefresh = static_cast<std::size_t>(sampleRate) / REFRESH_RATE;
    // 128 refreshes contain 51,200 samples, a multiple of every analyzer hop at 48 kHz.
    constexpr std::size_t captureRefreshes {128};
    constexpr double captureSeconds = static_cast<double>(captureRefreshes * samplesPerRefresh) / sampleRate;
    std::array<std::vector<CapturedSpectralFrame>, captureRefreshes> captured;
    SpectrogramParticleBuilder particles;
    {
        SpatialAnalyzer analyzer;
        analyzer.prepare(sampleRate);
        analyzer.setFrameCallback([&particles](
                                      const std::span<const SpectralPoint> points, const SpectralFrameInfo& frame
                                  ) { particles.addFrame(points, frame); });
        const auto left = makeNoise(samplesPerRefresh, 3);
        const auto right = makeNoise(samplesPerRefresh, 4);
        // Seed all tier peaks and the reference clock through the actual warmup endpoints before replay begins.
        for (int refresh = 0; refresh < REFRESH_RATE; ++refresh) {
            particles.clear();
            analyzer.pushSamples(left, right);
        }
        std::size_t captureIndex = 0;
        analyzer.setFrameCallback([&](const std::span<const SpectralPoint> points, const SpectralFrameInfo& frame) {
            captured[captureIndex].push_back({
                .info = frame,
                .points = std::vector<SpectralPoint>(points.begin(), points.end()),
            });
        });
        for (; captureIndex < captured.size(); ++captureIndex) {
            analyzer.pushSamples(left, right);
        }
    }
    const auto replay = [&](const std::size_t refresh) {
        particles.clear();
        const std::size_t cycle = refresh / captured.size();
        const double cycleOffset = static_cast<double>(cycle) * captureSeconds;
        for (const auto& frame : captured[refresh % captured.size()]) {
            auto info = frame.info;
            info.endSeconds += cycleOffset;
            particles.addFrame(frame.points, info);
        }
    };
    std::size_t refreshIndex = 0;
    for (; refreshIndex < captured.size(); ++refreshIndex) {
        replay(refreshIndex);
    }
    std::vector<double> measurements;
    measurements.reserve(static_cast<std::size_t>(options.refreshes) * static_cast<std::size_t>(options.repetitions));
    std::size_t particleCount = 0;
    double checksum = 0.0;
    for (int repetition = 0; repetition < options.repetitions; ++repetition) {
        for (int refresh = 0; refresh < options.refreshes; ++refresh) {
            const auto start = Clock::now();
            replay(refreshIndex);
            measurements.push_back(Microseconds(Clock::now() - start).count());
            ++refreshIndex;
            particleCount += particles.particles().size();
            checksum += static_cast<double>(particles.referenceLevel());
            if (!particles.particles().empty()) {
                checksum += static_cast<double>(particles.particles().front().red);
            }
        }
    }
    const auto result = distribution(measurements);
    fmt::print(
        "particles-only rate=48000 refresh_hz={} captured_refreshes={} n={} "
        "us[p50/p95/p99/max]={:.3f}/{:.3f}/{:.3f}/{:.3f} particles={} check={:.6f}\n",
        REFRESH_RATE,
        captured.size(),
        measurements.size(),
        result.p50,
        result.p95,
        result.p99,
        result.max,
        particleCount,
        checksum
    );
}

/// Times CPU software overlay painting with a settled pointer state and no editor, desktop peer, or GL context.
void timeSpectrogramOverlays(const Options& options, const OverlayHover hover)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), processor.getHistory());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    if (hover != OverlayHover::none) {
        const auto plot = view.getPlotBounds();
        const double frequency = hover == OverlayHover::band
            ? std::sqrt(static_cast<double>(DEFAULT_CROSSOVERS[0]) * static_cast<double>(DEFAULT_CROSSOVERS[1]))
            : static_cast<double>(DEFAULT_CROSSOVERS[1]);
        const juce::Point<float> position(
            plot.getX()
                + static_cast<float>(frequencyToShare(frequency, FrequencyScale::logarithmic)) * plot.getWidth(),
            plot.getCentreY()
        );
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent event(
            juce::Desktop::getInstance().getMainMouseSource(),
            position,
            {},
            1.0F,
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            &view,
            &view,
            now,
            position,
            now,
            0,
            false
        );
        view.mouseMove(event);
        if (hover == OverlayHover::band && view.getHoveredBand() != std::optional<std::size_t> {1}) {
            throw std::runtime_error("Benchmark band hover missed its target");
        }
    }
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
    const char* hoverName = "none";
    if (hover == OverlayHover::band) {
        hoverName = "band-response";
    } else if (hover == OverlayHover::crossover) {
        hoverName = "crossover";
    }
    fmt::print(
        "overlay software={}x{} refresh_hz={} n={} us[p50/p95/p99/max]={:.3f}/{:.3f}/{:.3f}/{:.3f} hover={}\n",
        VIEW_WIDTH,
        VIEW_HEIGHT,
        REFRESH_RATE,
        measurements.size(),
        result.p50,
        result.p95,
        result.p99,
        result.max,
        hoverName
    );
}

/// Times four display-history updates and their software paint at a simulated, unpaced editor refresh cadence.
/// Input generation, canvas clearing, setup/layout, and observable history/pixel checksums are outside the clock pair.
void timeBandCorrelationMeters(const Options& options, const bool changing, const double persistenceSeconds)
{
    constexpr int meterWidth {VIEW_WIDTH / static_cast<int>(BAND_COUNT)};
    constexpr double refreshSeconds {1.0 / REFRESH_RATE};
    constexpr std::array<float, BAND_COUNT> steadyValues {-0.8F, -0.2F, 0.3F, 0.9F};
    const int warmupRefreshes = static_cast<int>(std::ceil(MAX_PERSISTENCE_SECONDS * REFRESH_RATE)) + 1;
    std::array<std::array<float, BAND_COUNT>, REFRESH_RATE> readings {};
    for (std::size_t refresh = 0; refresh < readings.size(); ++refresh) {
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            const double phase = static_cast<double>(refresh) / REFRESH_RATE
                + static_cast<double>(band) / static_cast<double>(BAND_COUNT);
            readings[refresh][band] = changing
                ? static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * phase))
                : steadyValues[band];
        }
    }
    juce::Component row;
    row.setSize(VIEW_WIDTH, BAND_CORRELATION_HEIGHT);
    std::array<std::unique_ptr<BandCorrelationMeter>, BAND_COUNT> meters;
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        meters[band] = std::make_unique<BandCorrelationMeter>(bandName(band));
        meters[band]->setBounds(static_cast<int>(band) * meterWidth, 0, meterWidth, BAND_CORRELATION_HEIGHT);
        row.addAndMakeVisible(*meters[band]);
    }
    juce::Image canvas(juce::Image::ARGB, VIEW_WIDTH, BAND_CORRELATION_HEIGHT, true, juce::SoftwareImageType {});
    juce::Graphics graphics(canvas);
    const auto updateAndPaint = [&](const std::array<float, BAND_COUNT>& values) {
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            meters[band]->setCorrelation(values[band], refreshSeconds, persistenceSeconds);
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
                    static_cast<int>(band) * meterWidth + refresh % meterWidth, refresh % BAND_CORRELATION_HEIGHT
                );
                pixelChecksum += pixel.getARGB();
            }
        }
    }
    const auto result = distribution(measurements);
    fmt::print(
        "correlation update+paint meters={} readings={} persistence_s={:.3f} software={}x{} refresh_hz={} "
        "warmup_s/rep={:.3f} n={} us[p50/p95/p99/max]={:.3f}/{:.3f}/{:.3f}/{:.3f} check={:.6f} pixels={}\n",
        BAND_COUNT,
        changing ? "changing" : "steady",
        persistenceSeconds,
        VIEW_WIDTH,
        BAND_CORRELATION_HEIGHT,
        REFRESH_RATE,
        static_cast<double>(warmupRefreshes) * refreshSeconds,
        measurements.size(),
        result.p50,
        result.p95,
        result.p99,
        result.max,
        checksum,
        pixelChecksum
    );
}
}  // namespace esgrove::image_control::benchmark
