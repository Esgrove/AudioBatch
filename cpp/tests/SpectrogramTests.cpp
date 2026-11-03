/// Independent spectrogram expectations for calibrated tones, centered timing, stereo powers, and mel support.
/// Covers arbitrary streaming chunks, bounded pairwise merging, EOF padding, and strict portable codec validation.

#include "analysis/AudioAnalysisTypes.h"
#include "analysis/SpectrogramBuilder.h"
#include "analysis/SpectrogramData.h"
#include "TestSuites.h"

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace audiobatch::tests
{
namespace
{
constexpr int SAMPLE_RATE {48'000};
constexpr int WINDOW {4096};
constexpr int HOP {1024};
constexpr int ROWS {512};

/// Synthesizes a bin-centered analytic sinusoid rather than depending on another analysis implementation.
juce::AudioBuffer<float> tone(const int channels, const int frames, const int bin, const double amplitude = 0.5)
{
    juce::AudioBuffer<float> samples(channels, frames);
    for (int frame = 0; frame < frames; ++frame) {
        const auto value = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * bin * frame / WINDOW));
        for (int channel = 0; channel < channels; ++channel) {
            samples.setSample(channel, frame, value);
        }
    }
    return samples;
}

/// Builds the same PCM in arbitrary ranges, including a harmless empty push between ranges.
std::shared_ptr<const SpectrogramData> build(const juce::AudioBuffer<float>& samples, const int chunk = 997)
{
    SpectrogramBuilder builder(samples.getNumChannels(), SAMPLE_RATE);
    for (int first = 0; first < samples.getNumSamples(); first += chunk) {
        builder.push(samples, first, 0);
        builder.push(samples, first, std::min(chunk, samples.getNumSamples() - first));
    }
    return builder.finish();
}

/// Computes the expected row independently using the published HTK mel definition.
int expectedRow(const double frequency)
{
    const auto mel = 2595.0 * std::log10(1.0 + frequency / 700.0);
    const auto maximumMel = 2595.0 * std::log10(1.0 + (SAMPLE_RATE * 0.5) / 700.0);
    return std::clamp(static_cast<int>(std::floor(mel * ROWS / maximumMel)), 0, ROWS - 1);
}

/// Returns a row byte with explicit bounds established by callers' test geometry assertions.
std::uint8_t intensity(const SpectrogramData& data, const std::size_t column, const int row)
{
    return data.intensities[column * ROWS + static_cast<std::size_t>(row)];
}

/// Presents a memory block to the decoder without copying or assuming native alignment.
std::shared_ptr<const SpectrogramData> decode(const juce::MemoryBlock& blob)
{
    return deserializeSpectrogram(std::span(static_cast<const std::byte*>(blob.getData()), blob.getSize()));
}
}  // namespace

/// Pins the DSP and byte-level contract independently of cache and GUI integration.
class SpectrogramTests final : public juce::UnitTest
{
public:
    /// Names the suite for explicit GoogleTest registration.
    SpectrogramTests() : juce::UnitTest("Spectrogram", "AudioBatch") {}

    void runTest() override;

private:
    void runCodecTests();
    void runMergeTests();
    void runStreamingTests();
    void runToneTests();
};

/// Runs focused, device-free checks of the optional analysis artifact.
void SpectrogramTests::runTest()
{
    runToneTests();
    runStreamingTests();
    runMergeTests();
    runCodecTests();
}

/// Checks physical amplitude, independent mel positions, channel powers, and endpoint calibration.
void SpectrogramTests::runToneTests()
{
    beginTest("HTK mel conversion and the fixed decibel profile have independent anchors");
    expectWithinAbsoluteError(frequencyToMel(0.0), 0.0, 1.0e-12);
    expectWithinAbsoluteError(frequencyToMel(1000.0), 999.9855371396244, 1.0e-9);
    expectWithinAbsoluteError(melToFrequency(999.9855371396244), 1000.0, 1.0e-9);
    expectWithinAbsoluteError(spectrogramDecibels(0), -120.0, 1.0e-12);
    expectWithinAbsoluteError(spectrogramDecibels(255), 24.0, 1.0e-12);

    beginTest("Bin-centered sinusoid amplitudes land at their analytic mel rows and source levels");
    for (const int bin : {1, 32, 128, 700, 1800}) {
        const auto data = build(tone(1, 5 * HOP, bin));
        expect(data != nullptr);
        if (!data) {
            continue;
        }
        const auto row = expectedRow(static_cast<double>(bin) * SAMPLE_RATE / WINDOW);
        expectWithinAbsoluteError(spectrogramDecibels(intensity(*data, 2, row)), 20.0 * std::log10(0.5), 0.29);
        expectEquals(data->sampleRate, SAMPLE_RATE);
        expectEquals(data->columnCount(), std::size_t {5});
        expect(isValidSpectrogramData(*data));
    }

    beginTest("Antiphase stereo preserves power instead of cancelling a PCM downmix");
    auto stereo = tone(2, 5 * HOP, 128);
    stereo.applyGain(1, 0, stereo.getNumSamples(), -1.0f);
    const auto antiphase = build(stereo);
    const auto mono = build(tone(1, 5 * HOP, 128));
    expect(antiphase && mono);
    if (antiphase && mono) {
        expect(antiphase->intensities == mono->intensities);
    }

    beginTest("A silent second channel lowers mean power by 3.01 dB");
    stereo.clear(1, 0, stereo.getNumSamples());
    const auto oneChannel = build(stereo);
    expect(oneChannel != nullptr);
    if (oneChannel) {
        expectWithinAbsoluteError(
            spectrogramDecibels(intensity(*oneChannel, 2, expectedRow(1500.0))), 10.0 * std::log10(0.125), 0.29
        );
    }

    beginTest("DC and Nyquist use the real endpoint factor, and the fixed ceiling clips over-range tones");
    for (const bool nyquist : {false, true}) {
        juce::AudioBuffer<float> samples(1, 5 * HOP);
        for (int frame = 0; frame < samples.getNumSamples(); ++frame) {
            samples.setSample(0, frame, nyquist && frame % 2 != 0 ? -0.5f : 0.5f);
        }
        const auto data = build(samples);
        expect(data != nullptr);
        if (data) {
            expectWithinAbsoluteError(
                spectrogramDecibels(intensity(*data, 2, nyquist ? ROWS - 1 : 0)), 20.0 * std::log10(0.5), 0.29
            );
            if (!nyquist) {
                // The first several sub-bin mel rows share the DC half-bin support instead of showing holes.
                expect(intensity(*data, 2, 1) > 0);
            }
        }
    }
    const auto clipped = build(tone(1, 5 * HOP, 128, 32.0));
    expect(clipped != nullptr);
    if (clipped) {
        expectEquals(static_cast<int>(intensity(*clipped, 2, expectedRow(1500.0))), 255);
    }
}

/// Checks centered source timing, final partial windows, chunk independence, and artifact-only rejection.
void SpectrogramTests::runStreamingTests()
{
    beginTest("Zero frames have no artifact, and one frame has one centered zero-padded spectrum");
    SpectrogramBuilder empty(1, SAMPLE_RATE);
    expect(empty.finish() == nullptr);
    expect(empty.finish() == nullptr);
    juce::AudioBuffer<float> single(1, 1);
    single.setSample(0, 0, 1.0f);
    const auto shortData = build(single);
    expect(shortData != nullptr);
    if (shortData) {
        expectEquals(shortData->columnCount(), std::size_t {1});
        expectEquals(shortData->lengthInSamples, std::int64_t {1});
        expect(intensity(*shortData, 0, expectedRow(1500.0)) > 0);
    }

    beginTest("Silence is floor-valued at every row, including arbitrary partial EOF");
    for (const int frames : {HOP - 1, HOP, HOP + 1, WINDOW - 1, WINDOW, WINDOW + 1}) {
        juce::AudioBuffer<float> silence(2, frames);
        silence.clear();
        const auto data = build(silence, 17);
        expect(data != nullptr);
        if (data) {
            expectEquals(data->columnCount(), 1 + (static_cast<std::size_t>(frames) - 1) / HOP);
            expectEquals(data->lengthInSamples, static_cast<std::int64_t>(frames));
            expect(std::ranges::all_of(data->intensities, [](const auto value) { return value == 0; }));
        }
    }

    beginTest("An impulse at a source hop center follows the explicit symmetric Hann and leading padding");
    juce::AudioBuffer<float> impulse(1, 6 * HOP + 7);
    impulse.clear();
    impulse.setSample(0, 2 * HOP, 1.0f);
    const auto centered = build(impulse);
    expect(centered != nullptr);
    if (centered) {
        const auto row = expectedRow(1500.0);
        expectEquals(static_cast<int>(intensity(*centered, 0, row)), 0);
        expect(intensity(*centered, 2, row) > intensity(*centered, 1, row));
        expect(intensity(*centered, 2, row) > intensity(*centered, 3, row));
        expectEquals(static_cast<int>(intensity(*centered, 4, row)), 0);
        expectWithinAbsoluteError(
            spectrogramDecibels(intensity(*centered, 2, row)), 20.0 * std::log10(2.0 / 2047.5), 0.29
        );
    }

    beginTest("EOF impulse is retained and one-sample pushes equal a single buffer push");
    impulse.setSample(0, impulse.getNumSamples() - 1, 0.75f);
    const auto whole = build(impulse, impulse.getNumSamples());
    const auto singles = build(impulse, 1);
    const auto uneven = build(impulse, 317);
    expect(whole && singles && uneven);
    if (whole && singles && uneven) {
        expect(whole->intensities == singles->intensities);
        expect(whole->intensities == uneven->intensities);
        expect(intensity(*whole, whole->columnCount() - 1, expectedRow(1500.0)) > 0);
    }
    SpectrogramBuilder finished(1, SAMPLE_RATE);
    finished.push(single, 0, 1);
    const auto firstResult = finished.finish();
    finished.push(single, 0, 1);
    expect(firstResult == finished.finish());

    beginTest("Unsupported formats, invalid ranges, NaNs, and infinities invalidate only the artifact");
    for (const int channels : {0, 3}) {
        SpectrogramBuilder unsupported(channels, SAMPLE_RATE);
        expect(unsupported.finish() == nullptr);
    }
    SpectrogramBuilder invalidRate(1, 0);
    invalidRate.push(single, 0, 1);
    expect(invalidRate.finish() == nullptr);
    for (const auto value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        single.setSample(0, 0, value);
        expect(build(single) == nullptr);
        expect(!std::isfinite(single.getSample(0, 0)));
    }
    for (const std::array<int, 2> range : {std::array {-1, 1}, std::array {0, 2}, std::array {0, -1}}) {
        SpectrogramBuilder invalidRange(1, SAMPLE_RATE);
        invalidRange.push(single, range[0], range[1]);
        expect(invalidRange.finish() == nullptr);
    }
}

/// Reuses small PCM blocks across the first two merge boundaries without allocating a long source file.
void SpectrogramTests::runMergeTests()
{
    beginTest("The exact column bound is unmerged, and each subsequent boundary doubles canonical stride");
    juce::AudioBuffer<float> block(1, HOP);
    block.clear();
    SpectrogramBuilder builder(1, SAMPLE_RATE);
    // A center-zero impulse must survive every subsequent pairwise maximum reduction.
    block.setSample(0, 0, 1.0f);
    builder.push(block, 0, HOP);
    block.clear();
    for (int hop = 1; hop < 4096; ++hop) {
        builder.push(block, 0, HOP);
    }
    // A separate builder pins the exact bound without completing the longer stream prematurely.
    SpectrogramBuilder exactBuilder(1, SAMPLE_RATE);
    juce::AudioBuffer<float> repeated(1, 64 * HOP);
    repeated.clear();
    for (int chunk = 0; chunk < 64; ++chunk) {
        exactBuilder.push(repeated, 0, repeated.getNumSamples());
    }
    const auto exact = exactBuilder.finish();
    expect(exact != nullptr);
    if (exact) {
        expectEquals(exact->columnCount(), std::size_t {4096});
        expectEquals(exact->samplesPerColumn, std::int64_t {HOP});
    }
    for (int hop = 4096; hop < 8192; ++hop) {
        builder.push(block, 0, HOP);
    }
    block.setSample(0, 0, 1.0f);
    builder.push(block, 0, 1);
    const auto merged = builder.finish();
    expect(merged != nullptr);
    if (merged) {
        expectEquals(merged->samplesPerColumn, 4 * std::int64_t {HOP});
        expectEquals(merged->columnCount(), std::size_t {2049});
        expectEquals(merged->lengthInSamples, std::int64_t {8192 * HOP + 1});
        expect(merged->intensities.size() <= std::size_t {4096} * ROWS);
        expect(intensity(*merged, 0, expectedRow(1500.0)) > 0);
        expect(intensity(*merged, merged->columnCount() - 1, expectedRow(1500.0)) > 0);
        expect(isValidSpectrogramData(*merged));
    }
}

/// Checks exact profile bytes, independent geometry rejection, and malformed or future cache envelopes.
void SpectrogramTests::runCodecTests()
{
    beginTest("The little-endian SPEC envelope round trips geometry and unmodified column-major bytes");
    SpectrogramData source;
    source.sampleRate = SAMPLE_RATE;
    source.channelCount = 2;
    source.lengthInSamples = HOP + 1;
    source.intensities.resize(std::size_t {2} * ROWS);
    for (std::size_t index = 0; index < source.intensities.size(); ++index) {
        source.intensities[index] = static_cast<std::uint8_t>(index % 256);
    }
    const auto blob = serializeSpectrogram(source);
    expectEquals(blob.getSize(), std::size_t {72 + 2 * ROWS});
    const auto restored = decode(blob);
    expect(restored != nullptr);
    if (restored) {
        expect(restored->intensities == source.intensities);
        expectEquals(restored->sampleRate, SAMPLE_RATE);
        expectEquals(restored->channelCount, 2);
        expectEquals(restored->lengthInSamples, std::int64_t {HOP + 1});
        expectEquals(restored->samplesPerColumn, std::int64_t {HOP});
    }
    const auto bytes = std::span(static_cast<const std::byte*>(blob.getData()), blob.getSize());
    expect(
        bytes[0] == std::byte {'S'} && bytes[1] == std::byte {'P'} && bytes[2] == std::byte {'E'}
        && bytes[3] == std::byte {'C'}
    );
    expect(bytes[28] == std::byte {0x80} && bytes[29] == std::byte {0xbb});
    expect(bytes[40] == std::byte {1} && bytes[41] == std::byte {4});

    beginTest("Every fixed profile field and geometry field is checked, with exact payload consumption");
    expect(decode({}) == nullptr);
    for (std::size_t size = 0; size < 72; ++size) {
        expect(deserializeSpectrogram(bytes.first(size)) == nullptr);
    }
    for (const std::size_t offset : {0U, 4U, 8U, 12U, 16U, 20U, 24U, 32U, 36U, 40U, 48U, 56U, 64U}) {
        auto corrupt = blob;
        corrupt[offset] = static_cast<char>(static_cast<unsigned char>(corrupt[offset]) ^ 0x01U);
        expect(decode(corrupt) == nullptr);
    }
    auto zeroRate = blob;
    for (std::size_t offset = 28; offset < 32; ++offset) {
        zeroRate[offset] = 0;
    }
    expect(decode(zeroRate) == nullptr);
    auto enormous = blob;
    for (std::size_t offset = 36; offset < 56; ++offset) {
        enormous[offset] = static_cast<char>(0xff);
    }
    expect(decode(enormous) == nullptr);
    auto truncated = blob;
    truncated.setSize(blob.getSize() - 1);
    expect(decode(truncated) == nullptr);
    auto extended = blob;
    extended.append("x", 1);
    expect(decode(extended) == nullptr);

    beginTest("Invalid and noncanonical data cannot be serialized or matched to a source record");
    AudioAnalysisRecord record;
    record.channels = 2;
    record.sampleRate = SAMPLE_RATE;
    record.lengthInSamples = HOP + 1;
    expect(spectrogramMatches(source, record));
    ++record.lengthInSamples;
    expect(!spectrogramMatches(source, record));
    for (const int invalidKind : {0, 1, 2, 3, 4, 5}) {
        auto invalid = source;
        switch (invalidKind) {
            case 0:
                invalid.sampleRate = 0;
                break;
            case 1:
                invalid.channelCount = 3;
                break;
            case 2:
                invalid.lengthInSamples = 0;
                break;
            case 3:
                invalid.samplesPerColumn *= 2;
                break;
            case 4:
                invalid.intensities.pop_back();
                break;
            case 5:
                invalid.intensities.resize(std::size_t {4097} * ROWS);
                break;
            default:
                break;
        }
        expect(!isValidSpectrogramData(invalid));
        expect(serializeSpectrogram(invalid).isEmpty());
    }
}

/// Creates the explicitly registered spectrogram contract suite.
std::unique_ptr<juce::UnitTest> createSpectrogramTests()
{
    return std::make_unique<SpectrogramTests>();
}
}  // namespace audiobatch::tests
