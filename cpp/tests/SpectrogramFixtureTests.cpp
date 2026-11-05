/// Exercises production file analysis against shared NumPy spectrogram fixtures.
/// Checks independent mel anchors, source geometry, calibrated tonal powers, and interior noise cutoff contrast.
/// Sparse linear FFT probes are compared only where they establish a tonal row maximum, not arbitrary noise rows.

#include "analysis/AudioAnalysis.h"
#include "analysis/SpectrogramData.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/StringFormat.h"

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

namespace audiobatch::tests
{
namespace
{
constexpr int FFT_SIZE {4096};
constexpr int HOP_SIZE {1024};
constexpr int MEL_ROWS {512};
constexpr double MEL_TOLERANCE {1.0e-8};
constexpr double CUTOFF_CONTRAST_DECIBELS {40.0};
constexpr std::array SAMPLE_RATES {44'100, 48'000, 96'000};

/// Distinguishes numeric JSON values from absent fields and numeric-looking strings.
bool isNumber(const juce::var& value)
{
    return (value.isInt() || value.isInt64() || value.isDouble()) && std::isfinite(static_cast<double>(value));
}

/// Validates the source kind and any tonal parameters before peak-bin selection.
bool validSourceExpectation(const juce::var& source)
{
    if (!source.isObject() || !source["kind"].isString()) {
        return false;
    }
    const auto kind = source["kind"].toString();
    if (kind == "tone" || kind == "antiphase") {
        const auto& tones = source["tone_bins"];
        const auto& amplitudes = source["amplitudes"];
        if (!tones.isArray() || tones.size() == 0 || !amplitudes.isArray() || amplitudes.size() != tones.size()) {
            return false;
        }
        for (int index = 0; index < tones.size(); ++index) {
            if (!isNumber(tones[index]) || static_cast<int>(tones[index]) <= 0
                || static_cast<int>(tones[index]) >= FFT_SIZE / 2 || !isNumber(amplitudes[index])
                || static_cast<double>(amplitudes[index]) <= 0.0)
            {
                return false;
            }
        }
    }
    return kind == "silence" || kind == "tone" || kind == "antiphase" || kind == "dc" || kind == "nyquist"
        || kind == "periodic_noise";
}

/// Validates centered frame positions and their sparse nonnegative power arrays.
bool validFrameExpectations(const juce::Array<juce::var>& frames, const int probeCount, const juce::int64 sampleCount)
{
    for (const auto& frame : frames) {
        const auto& powers = frame["powers"];
        if (!frame.isObject() || !isNumber(frame["center_sample"]) || !powers.isArray() || powers.size() != probeCount)
        {
            return false;
        }
        const auto center = static_cast<juce::int64>(frame["center_sample"]);
        if (center < 0 || center >= sampleCount || center % HOP_SIZE != 0) {
            return false;
        }
        for (const auto& power : *powers.getArray()) {
            if (!isNumber(power) || static_cast<double>(power) < 0.0) {
                return false;
            }
        }
    }
    return true;
}

/// Validates every array and indexed value before the fixture checks dereference them.
bool validFileExpectation(const juce::var& expected)
{
    if (!expected.isObject() || !isNumber(expected["sample_rate"]) || !isNumber(expected["channels"])
        || !isNumber(expected["sample_count"]) || !isNumber(expected["frame_count"]))
    {
        return false;
    }
    const auto rate = static_cast<int>(expected["sample_rate"]);
    const auto channels = static_cast<int>(expected["channels"]);
    const auto count = static_cast<juce::int64>(expected["sample_count"]);
    const auto& bins = expected["probe_bins"];
    const auto& frames = expected["frames"];
    const auto& source = expected["source"];
    if (std::ranges::find(SAMPLE_RATES, rate) == SAMPLE_RATES.end() || channels < 1 || channels > 2 || count < FFT_SIZE
        || count > juce::int64 {4096} * HOP_SIZE || !bins.isArray() || bins.size() == 0 || !frames.isArray()
        || frames.size() == 0 || !source.isObject() || !source["kind"].isString()
        || static_cast<juce::int64>(expected["frame_count"]) != 1 + (count - 1) / HOP_SIZE)
    {
        return false;
    }
    int previousBin = -1;
    for (const auto& bin : *bins.getArray()) {
        if (!isNumber(bin) || static_cast<int>(bin) <= previousBin || static_cast<int>(bin) > FFT_SIZE / 2) {
            return false;
        }
        previousBin = static_cast<int>(bin);
    }
    return validFrameExpectations(*frames.getArray(), bins.size(), count) && validSourceExpectation(source);
}

/// Confirms that an interior tonal probe exceeds both independently measured Hann neighbors.
/// Real endpoint bins are exempt because their power normalization differs from adjacent complex bins.
void expectLocalPeak(
    juce::UnitTest& test,
    const juce::Array<juce::var>& probes,
    const juce::var& powers,
    const int bin,
    const double power
)
{
    if (bin <= 0 || bin >= FFT_SIZE / 2) {
        return;
    }
    for (const auto neighbor : {bin - 1, bin + 1}) {
        const auto neighborProbe = probes.indexOf(juce::var(neighbor));
        test.expect(neighborProbe >= 0);
        if (neighborProbe >= 0) {
            test.expect(power > static_cast<double>(powers[neighborProbe]), "Expected a local tone maximum");
        }
    }
}

/// Locates a source frequency by the published logarithmic mel coordinate, without production conversion helpers.
int referenceRow(const double frequency, const int sampleRate)
{
    const auto position = MEL_ROWS * std::log1p(frequency / 700.0) / std::log1p(sampleRate * 0.5 / 700.0);
    return std::clamp(static_cast<int>(std::floor(position)), 0, MEL_ROWS - 1);
}

/// Reads one column-major display byte after source geometry has been checked.
int rowByte(const SpectrogramData& data, const std::size_t column, const int row)
{
    return data.intensities[column * MEL_ROWS + static_cast<std::size_t>(row)];
}

/// Quantizes independent linear power using the documented fixed display range and nearest-byte rounding.
int referenceByte(const double power)
{
    const auto decibels = std::clamp(10.0 * std::log10(std::max(power, 1.0e-12)), -120.0, 24.0);
    return static_cast<int>(std::floor((decibels + 120.0) * 255.0 / 144.0 + 0.5));
}

/// Reports physical row levels without calling the production display conversion.
double rowDecibels(const SpectrogramData& data, const std::size_t column, const int row)
{
    return -120.0 + static_cast<double>(rowByte(data, column, row)) * 144.0 / 255.0;
}
}  // namespace

/// Compares the real decoding and analysis pipeline with independently generated shared fixtures.
class SpectrogramFixtureTests final : public juce::UnitTest
{
public:
    /// Gives the explicitly registered suite a stable filter name.
    SpectrogramFixtureTests() : juce::UnitTest("Spectrogram fixtures", "AudioBatch") {}

    void runTest() override;

private:
    void checkMel(const juce::var& expectations);
    void checkNoiseCutoffs(const std::map<juce::String, std::shared_ptr<const SpectrogramData>>& spectra);
    void checkPowers(const SpectrogramData& data, const juce::var& expected);
};

/// Loads the required oracle defensively, then analyzes every listed source through the production entry point.
void SpectrogramFixtureTests::runTest()
{
    beginTest("The shared NumPy dataset is required and has the expected contract");
    const auto oracle = fixtureFile("spectrogram_expected/expected.json");
    expect(oracle.existsAsFile(), "Missing shared spectrogram expectations");
    const auto expectations = juce::JSON::parse(oracle);
    expect(expectations.isObject(), "Spectrogram expectations must be a JSON object");
    if (!expectations.isObject()) {
        return;
    }
    expectEquals(static_cast<int>(expectations["schema_version"]), 1);
    const auto& contract = expectations["contract"];
    expectEquals(static_cast<int>(contract["fft_size"]), FFT_SIZE);
    expectEquals(static_cast<int>(contract["hop_size"]), HOP_SIZE);
    expectEquals(static_cast<int>(contract["mel_rows"]), MEL_ROWS);
    expectEquals(contract["window"].toString(), juce::String("symmetric_hann"));
    checkMel(expectations["mel"]);

    const auto& files = expectations["files"];
    expect(files.isObject(), "The oracle must contain a files object");
    if (!files.isObject()) {
        return;
    }
    const auto& properties = files.getDynamicObject()->getProperties();
    expectEquals(properties.size(), 20);
    for (const auto rate : SAMPLE_RATES) {
        for (const auto* kind : {"silence", "tone", "antiphase", "noise_16000hz", "noise_18000hz", "noise_full"}) {
            const auto fixtureName = utils::format("{}_{}.wav", kind, rate);
            expect(files.hasProperty(juce::Identifier(fixtureName)), utils::format("Missing fixture {}", fixtureName));
        }
    }
    for (const auto* fixtureName : {"dc_48000.wav", "nyquist_48000.wav"}) {
        expect(files.hasProperty(juce::Identifier(fixtureName)), utils::format("Missing fixture {}", fixtureName));
    }

    std::map<juce::String, std::shared_ptr<const SpectrogramData>> spectra;
    for (int index = 0; index < properties.size(); ++index) {
        const auto fixtureName = properties.getName(index).toString();
        const auto& expected = properties.getValueAt(index);
        beginTest(utils::format("Source geometry and independent powers: {}", fixtureName));
        const auto valid = validFileExpectation(expected);
        expect(valid, "Malformed fixture description");
        if (!valid) {
            continue;
        }
        const auto file = fixtureFile(utils::format("spectrogram_expected/{}", fixtureName));
        expect(file.existsAsFile());
        if (!file.existsAsFile()) {
            continue;
        }
        const auto record = analyzeFile(file);
        expect(!record.error.has_value(), record.errorMessage);
        expectEquals(record.sampleRate, static_cast<int>(expected["sample_rate"]));
        expectEquals(record.channels, static_cast<int>(expected["channels"]));
        expectEquals(
            record.lengthInSamples, static_cast<std::int64_t>(static_cast<juce::int64>(expected["sample_count"]))
        );
        expect(record.spectrogram != nullptr, "Production file analysis must produce its source spectrogram");
        if (!record.spectrogram) {
            continue;
        }
        const auto& data = *record.spectrogram;
        const auto validGeometry = isValidSpectrogramData(data) && spectrogramMatches(data, record)
            && data.sampleRate == static_cast<int>(expected["sample_rate"])
            && data.channelCount == static_cast<int>(expected["channels"])
            && data.lengthInSamples == static_cast<juce::int64>(expected["sample_count"])
            && data.samplesPerColumn == HOP_SIZE
            && data.columnCount() == static_cast<std::size_t>(static_cast<int>(expected["frame_count"]))
            && data.intensities.size() == data.columnCount() * MEL_ROWS;
        expect(validGeometry, "Source-rate geometry, centered hop count, and complete row storage must agree");
        if (!validGeometry) {
            continue;
        }
        checkPowers(data, expected);
        spectra.emplace(fixtureName, record.spectrogram);
    }
    checkNoiseCutoffs(spectra);
}

/// Uses stored NumPy mel values directly, including high-rate Nyquist and selected boundary anchors.
void SpectrogramFixtureTests::checkMel(const juce::var& expectations)
{
    beginTest("Shared mel landmarks and boundaries match to 1e-8");
    expect(expectations.isArray());
    if (!expectations.isArray()) {
        return;
    }
    expectEquals(expectations.size(), static_cast<int>(SAMPLE_RATES.size()));
    for (const auto& expected : *expectations.getArray()) {
        const auto& landmarks = expected["landmarks"];
        const auto& boundaries = expected["boundaries"];
        expect(expected.isObject() && landmarks.isArray() && boundaries.isArray());
        if (!expected.isObject() || !landmarks.isArray() || !boundaries.isArray()) {
            continue;
        }
        expectEquals(landmarks.size(), 8);
        expectEquals(boundaries.size(), 7);
        for (const auto& landmark : *landmarks.getArray()) {
            const auto valid
                = isNumber(landmark["hz"]) && isNumber(landmark["mel"]) && isNumber(landmark["roundtrip_hz"]);
            expect(valid, "Malformed mel landmark");
            if (valid) {
                expectWithinAbsoluteError(
                    frequencyToMel(static_cast<double>(landmark["hz"])),
                    static_cast<double>(landmark["mel"]),
                    MEL_TOLERANCE
                );
                expectWithinAbsoluteError(
                    melToFrequency(static_cast<double>(landmark["mel"])),
                    static_cast<double>(landmark["roundtrip_hz"]),
                    MEL_TOLERANCE
                );
            }
        }
        for (const auto& boundary : *boundaries.getArray()) {
            const auto valid = isNumber(boundary["hz"]) && isNumber(boundary["mel"]);
            expect(valid, "Malformed mel boundary");
            if (valid) {
                expectWithinAbsoluteError(
                    frequencyToMel(static_cast<double>(boundary["hz"])),
                    static_cast<double>(boundary["mel"]),
                    MEL_TOLERANCE
                );
                expectWithinAbsoluteError(
                    melToFrequency(static_cast<double>(boundary["mel"])),
                    static_cast<double>(boundary["hz"]),
                    MEL_TOLERANCE
                );
            }
        }
    }
}

/// Checks silence everywhere and tonal local maxima only in fully supported interior Hann windows.
void SpectrogramFixtureTests::checkPowers(const SpectrogramData& data, const juce::var& expected)
{
    const auto& source = expected["source"];
    const auto kind = source["kind"].toString();
    if (kind == "silence") {
        expect(std::ranges::all_of(data.intensities, [](const auto value) { return value == 0; }));
        return;
    }
    if (kind == "periodic_noise") {
        // Sparse noise probes cannot predict a row maximum over other, unrecorded FFT bins.
        return;
    }
    juce::Array<int> peakBins;
    if (kind == "dc" || kind == "nyquist") {
        peakBins.add(kind == "dc" ? 0 : FFT_SIZE / 2);
    } else {
        for (const auto& bin : *source["tone_bins"].getArray()) {
            peakBins.add(static_cast<int>(bin));
        }
    }
    const auto& probes = *expected["probe_bins"].getArray();
    int checkedPeaks = 0;
    for (const auto& frame : *expected["frames"].getArray()) {
        const auto center = static_cast<juce::int64>(frame["center_sample"]);
        if (center < FFT_SIZE / 2 || center + FFT_SIZE / 2 > data.lengthInSamples) {
            continue;
        }
        const auto column = static_cast<std::size_t>(center / HOP_SIZE);
        for (const auto bin : peakBins) {
            const auto probe = probes.indexOf(juce::var(bin));
            expect(probe >= 0, "The oracle must include every source peak bin");
            if (probe < 0) {
                continue;
            }
            const auto power = static_cast<double>(frame["powers"][probe]);
            expect(power > 0.0, "Antiphase channels must retain their independent nonzero powers");
            expectLocalPeak(*this, probes, frame["powers"], bin, power);
            const auto frequency = static_cast<double>(bin) * data.sampleRate / FFT_SIZE;
            const auto row = referenceRow(frequency, data.sampleRate);
            // A peak's own frequency lies inside this mel interval, so its half-bin cell overlaps the row.
            // Other Hann main-lobe bins cannot exceed a bin-centered tonal peak.
            // Endpoint neighbor normalization differs by less than one byte for these constant/alternating signals.
            // One byte is 144/255 dB, covering nearest-byte boundaries and float FFT roundoff versus NumPy doubles.
            const auto difference = std::abs(rowByte(data, column, row) - referenceByte(power));
            expect(
                difference <= 1, utils::format("Peak bin {} at center {} differs by {} bytes", bin, center, difference)
            );
            ++checkedPeaks;
        }
    }
    expectEquals(checkedPeaks, peakBins.size());
}

/// Resolves cutoff contrast well away from transitions and file edges, at every fixture sample rate.
void SpectrogramFixtureTests::checkNoiseCutoffs(
    const std::map<juce::String, std::shared_ptr<const SpectrogramData>>& spectra
)
{
    for (const auto rate : SAMPLE_RATES) {
        for (const auto cutoff : {16'000, 18'000}) {
            beginTest(utils::format("Interior {} Hz noise cutoff at {} Hz source rate", cutoff, rate));
            const auto filtered = spectra.find(utils::format("noise_{}hz_{}.wav", cutoff, rate));
            const auto full = spectra.find(utils::format("noise_full_{}.wav", rate));
            expect(
                filtered != spectra.end() && full != spectra.end(), "Both filtered and full-band fixtures are required"
            );
            if (filtered == spectra.end() || full == spectra.end()) {
                continue;
            }
            const auto& filteredData = *filtered->second;
            const auto& fullData = *full->second;
            const auto belowRow = referenceRow(cutoff - 1000.0, rate);
            const auto aboveRow = referenceRow(cutoff + 1000.0, rate);
            const auto middle = (filteredData.lengthInSamples / 2) / HOP_SIZE;
            for (const auto offset : {-1, 0, 1}) {
                const auto center = (middle + offset) * HOP_SIZE;
                const auto interior = center >= FFT_SIZE / 2
                    && center + FFT_SIZE / 2 <= std::min(filteredData.lengthInSamples, fullData.lengthInSamples);
                expect(interior, "Cutoff checks require a full source window without EOF broad-band leakage");
                if (!interior) {
                    continue;
                }
                const auto column = static_cast<std::size_t>(center / HOP_SIZE);
                const auto below = rowDecibels(filteredData, column, belowRow);
                const auto above = rowDecibels(filteredData, column, aboveRow);
                const auto fullAbove = rowDecibels(fullData, column, aboveRow);
                expect(
                    below - above > CUTOFF_CONTRAST_DECIBELS,
                    utils::format("Below/above cutoff contrast at center {} is {} dB", center, below - above)
                );
                expect(
                    fullAbove - above > CUTOFF_CONTRAST_DECIBELS,
                    utils::format("Full/filtered high-row contrast at center {} is {} dB", center, fullAbove - above)
                );
            }
        }
    }
}

/// Creates the explicitly registered shared-fixture spectrogram suite.
std::unique_ptr<juce::UnitTest> createSpectrogramFixtureTests()
{
    return std::make_unique<SpectrogramFixtureTests>();
}
}  // namespace audiobatch::tests
