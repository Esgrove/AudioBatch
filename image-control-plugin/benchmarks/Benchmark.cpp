/// Times ImageControl's hot paths: the audio processing of the multiband imager,
/// and the CPU side of one display refresh of the spatial spectrogram: its analysis, particles, and overlay paint.
/// The particles are drawn on the GPU, which needs an OpenGL context and does not run here.
/// Build it in Release with the tests, and run it once with and once
/// without IMAGE_CONTROL_HARDENING to measure what the standard library hardening costs.

#include "analysis/SpatialAnalyzer.h"
#include "dsp/MultibandImager.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/SpatialSpectrogramView.h"

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <cstddef>
#include <print>
#include <random>
#include <ratio>
#include <span>
#include <vector>

namespace esgrove::image_control::benchmark
{
namespace
{
/// Sample rate of the benchmark audio.
constexpr double SAMPLE_RATE {48'000.0};
/// Seconds of stereo audio the imager processes.
constexpr int AUDIO_SECONDS {60};
/// Samples per processed block, a common host block size.
constexpr int BLOCK_SIZE {512};
/// Number of display refreshes the spectrogram runs.
constexpr int REFRESHES {600};
/// Display refresh rate the spectrogram runs at.
constexpr int REFRESH_RATE {120};
/// Width of the painted spectrogram, in pixels.
constexpr int VIEW_WIDTH {1000};
/// Height of the painted spectrogram, in pixels.
constexpr int VIEW_HEIGHT {420};
/// Amplitude of the noise.
constexpr float NOISE_AMPLITUDE {0.3F};

/// Mid gains of the bands, off unity so the imager does not take its neutral pass-through path.
constexpr BandGains MID_GAINS {0.9F, 1.0F, 1.1F, 0.8F};
/// Side gains of the bands.
constexpr BandGains SIDE_GAINS {0.5F, 1.2F, 1.4F, 0.7F};
/// Pans of the bands, off centre so the imager pans each band separately.
constexpr BandPans PANS {0.0F, -0.3F, 0.2F, 0.5F};
/// Pans of the bands at the centre, where the imager joins the summed mid and side once.
constexpr BandPans CENTRED_PANS {};
/// Output gain.
constexpr float OUTPUT_GAIN {0.9F};

/// Wall-clock time in milliseconds.
using Milliseconds = std::chrono::duration<double, std::milli>;

/// Returns uniform noise from a fixed seed, so every run processes the same audio.
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

/// Returns the milliseconds the imager takes per second of stereo audio with the given band pans,
/// with gains that keep it off its neutral pass-through path.
double timeImager(const BandPans& pans)
{
    MultibandImager imager;
    imager.setGains(MID_GAINS, SIDE_GAINS, OUTPUT_GAIN);
    imager.setPans(pans);
    imager.prepare(SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    const auto left = makeNoise(static_cast<std::size_t>(BLOCK_SIZE), 1);
    const auto right = makeNoise(static_cast<std::size_t>(BLOCK_SIZE), 2);
    juce::AudioBuffer<float> block(2, BLOCK_SIZE);
    const int blocks = AUDIO_SECONDS * static_cast<int>(SAMPLE_RATE) / BLOCK_SIZE;

    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < blocks; ++index) {
        block.copyFrom(0, 0, left.data(), BLOCK_SIZE);
        block.copyFrom(1, 0, right.data(), BLOCK_SIZE);
        imager.process(block);
    }
    const Milliseconds elapsed = std::chrono::steady_clock::now() - start;
    imager.release();
    return elapsed.count() / AUDIO_SECONDS;
}

/// Returns the milliseconds the CPU side of one spectrogram refresh takes: analyzing a refresh's worth of audio,
/// turning its bins into particles, taking the render batch, and painting the overlays.
double timeSpectrogram(ImageControlProcessor& processor)
{
    SpatialAnalyzer analyzer;
    analyzer.prepare(SAMPLE_RATE);
    SpatialSpectrogramView view(processor.getParameters());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    analyzer.setFrameCallback([&view](const std::span<const SpectralPoint> points, const double frameSeconds) {
        view.addFrame(points, frameSeconds);
    });
    const auto samplesPerRefresh = static_cast<std::size_t>(SAMPLE_RATE) / REFRESH_RATE;
    const auto left = makeNoise(samplesPerRefresh, 3);
    const auto right = makeNoise(samplesPerRefresh, 4);
    const juce::Image canvas(juce::Image::ARGB, VIEW_WIDTH, VIEW_HEIGHT, true);
    juce::Graphics graphics(canvas);

    const auto start = std::chrono::steady_clock::now();
    for (int refresh = 0; refresh < REFRESHES; ++refresh) {
        analyzer.pushSamples(left, right);
        view.advance(1.0 / REFRESH_RATE);
        const auto batch = view.takeRenderBatch();
        juce::ignoreUnused(batch);
        view.paintEntireComponent(graphics, false);
    }
    const Milliseconds elapsed = std::chrono::steady_clock::now() - start;
    return elapsed.count() / REFRESHES;
}
}  // namespace
}  // namespace esgrove::image_control::benchmark

/// Runs the imager with centred and panned bands and the spectrogram, and prints their timings.
int main()
{
    const juce::ScopedJuceInitialiser_GUI juce;
    esgrove::image_control::ImageControlProcessor processor;
    const double imagerMilliseconds
        = esgrove::image_control::benchmark::timeImager(esgrove::image_control::benchmark::CENTRED_PANS);
    const double pannedMilliseconds
        = esgrove::image_control::benchmark::timeImager(esgrove::image_control::benchmark::PANS);
    const double spectrogramMilliseconds = esgrove::image_control::benchmark::timeSpectrogram(processor);
    std::println("Imager: {:.3f} ms per second of stereo audio", imagerMilliseconds);
    std::println("Imager with panned bands: {:.3f} ms per second of stereo audio", pannedMilliseconds);
    std::println("Spectrogram CPU side: {:.3f} ms per refresh", spectrogramMilliseconds);
    return 0;
}
