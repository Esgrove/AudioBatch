/// Pins read-only public metadata and Serato readers to independently decoded real container expectations.
/// Each fixture retains its own exact timing, palette cells and cache roundtrip.

#include "analysis/AnalysisCache.h"
#include "metadata/AudioMetadata.h"
#include "serato/SeratoOverview.h"
#include "serato/SeratoTagCache.h"
#include "serato/SeratoTagReader.h"
#include "serato/SeratoTags.h"
#include "serato/SeratoTagTypes.h"
#include "serato/SeratoWaveformColouring.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Finds a unified TagLib property without changing its spelling or value.
juce::String property(const AudioMetadata& metadata, const juce::String& key)
{
    for (const auto& entry : metadata.properties) {
        if (entry.key == key && !entry.values.isEmpty()) {
            return entry.values[0];
        }
    }
    return {};
}

/// Adds opaque alpha to a fixed packed RGB expectation.
juce::uint32 opaqueArgb(const juce::var& rgb)
{
    return static_cast<juce::uint32>(static_cast<juce::int64>(rgb)) | 0xff000000U;
}

/// Checks the independently decoded records rather than comparing one format with another.
class RealMetadataTests final : public juce::UnitTest
{
public:
    /// Registers the real fixture suite independently of synthetic parser tests.
    RealMetadataTests() : UnitTest("Real metadata", "Metadata") {}

    void runTest() override;

private:
    void checkCache(
        const juce::File& file,
        const std::vector<serato::TagReadResult>& results,
        const juce::var& expected
    );
    void checkMarkers(const serato::Markers& value, const juce::var& expected);
    void checkOverview(const serato::Overview& value, const juce::var& expected);
    void checkTag(const serato::TagReadResult& result, const juce::var& expected);
    void exactReal(double actual, double expected);
};

/// Reads every fixture without saving and compares its bytes before and after the public reader calls.
void RealMetadataTests::runTest()
{
    beginTest("Shared raw-container oracle");
    const auto expectations = juce::JSON::parse(fixtureFile("metadata/expected.json"));
    expect(expectations.isArray(), "Shared raw-container oracle must be readable");
    if (!expectations.isArray()) {
        return;
    }
    expectEquals(expectations.size(), 9);
    for (const auto& expected : *expectations.getArray()) {
        beginTest(expected["file"].toString());
        const auto file = fixtureFile("metadata/" + expected["file"].toString());
        expect(file.existsAsFile());
        juce::MemoryBlock originalBytes;
        expect(file.loadFileAsData(originalBytes));
        AudioMetadata metadata;
        expect(readMetadata(file, metadata));
        for (const auto* key : {"TITLE", "ARTIST", "ALBUM", "GENRE", "COMMENT", "BPM", "INITIALKEY"}) {
            const auto oracleKey
                = juce::String(key) == "INITIALKEY" ? juce::String("key") : juce::String(key).toLowerCase();
            expectEquals(property(metadata, key), expected[juce::Identifier(oracleKey)].toString(), key);
        }
        expectEquals(
            property(metadata, "DATE"), expected["year"].isVoid() ? juce::String() : expected["year"].toString()
        );
        const auto results = serato::readTags(file, serato::allTagKinds);
        expectEquals(static_cast<int>(results.size()), static_cast<int>(serato::allTagKinds.size()));
        for (const auto& result : results) {
            checkTag(result, expected);
            if (result.value) {
                const auto roundtrip = serato::deserializeTag(result.kind, serato::serializeTag(result));
                expect(roundtrip.has_value(), "Cache roundtrip");
                if (roundtrip) {
                    checkTag(*roundtrip, expected);
                }
            }
        }
        checkCache(file, results, expected);
        juce::MemoryBlock finalBytes;
        expect(file.loadFileAsData(finalBytes));
        expect(finalBytes == originalBytes, "Readers leave fixtures unchanged");
    }
}

/// Persists all six outcomes through the production SQLite cache, including expected absent MP3 GEOB kinds.
void RealMetadataTests::checkCache(
    const juce::File& file,
    const std::vector<serato::TagReadResult>& results,
    const juce::var& expected
)
{
    const auto database
        = juce::File::getCurrentWorkingDirectory().getChildFile("real-metadata-" + juce::Uuid().toString() + ".db");
    const utils::FileIdentity identity(file);
    {
        AnalysisCache cache(database);
        expect(cache.open());
        for (const auto& result : results) {
            expect(serato::storeCachedTag(cache, identity, result));
        }
    }
    {
        AnalysisCache cache(database);
        expect(cache.open());
        for (const auto& result : results) {
            const auto cached = serato::getCachedTag(cache, identity, result.kind);
            expect(cached.has_value());
            if (cached) {
                checkTag(*cached, expected);
            }
        }
    }
    expect(database.deleteFile());
}

/// Checks each production kind against the common fixed expectation document.
void RealMetadataTests::checkTag(const serato::TagReadResult& result, const juce::var& expected)
{
    const auto absent = (result.kind == serato::TagKind::analysis && expected["analysis"].isVoid())
        || (result.kind == serato::TagKind::beatGrid && expected["beatgrid"].isVoid());
    expect(
        result.status() == (absent ? serato::ReadStatus::absent : serato::ReadStatus::present),
        utils::format("Kind {}", static_cast<int>(result.kind))
    );
    if (!result.value) {
        return;
    }
    switch (result.kind) {
        case serato::TagKind::analysis: {
            const auto& value = std::get<serato::Analysis>(*result.value);
            expectEquals(static_cast<int>(value.majorVersion), static_cast<int>(expected["analysis"][0]));
            expectEquals(static_cast<int>(value.minorVersion), static_cast<int>(expected["analysis"][1]));
            break;
        }
        case serato::TagKind::autotags: {
            const auto& value = std::get<serato::Autotags>(*result.value);
            exactReal(value.bpm, static_cast<double>(expected["autotags"][0]));
            exactReal(value.autoGain, static_cast<double>(expected["autotags"][1]));
            exactReal(value.gain, static_cast<double>(expected["autotags"][2]));
            break;
        }
        case serato::TagKind::beatGrid: {
            const auto& value = std::get<serato::BeatGrid>(*result.value);
            const auto& grid = expected["beatgrid"];
            const auto& anchors = *grid.getArray();
            expectEquals(static_cast<int>(value.markers.size()), anchors.size());
            for (std::size_t index = 0;
                 index < std::min(value.markers.size(), static_cast<std::size_t>(anchors.size()));
                 ++index)
            {
                const auto& anchor = anchors[static_cast<int>(index)];
                exactReal(value.markers[index].positionSeconds, static_cast<double>(anchor["position_seconds"]));
                expectEquals(
                    static_cast<juce::int64>(value.markers[index].beatsUntilNext),
                    static_cast<juce::int64>(anchor["beats_until_next"])
                );
                expect(value.markers[index].bpm.has_value() == !anchor["bpm"].isVoid());
                const auto bpm = value.markers[index].bpm;
                if (bpm) {
                    exactReal(*bpm, static_cast<double>(anchor["bpm"]));
                }
            }
            break;
        }
        case serato::TagKind::markers:
            checkMarkers(std::get<serato::Markers>(*result.value), expected);
            break;
        case serato::TagKind::overview:
            checkOverview(std::get<serato::Overview>(*result.value), expected["overview"]);
            break;
        case serato::TagKind::trackInfo: {
            const auto& value = std::get<serato::TrackInfo>(*result.value);
            expect(value.bpm.has_value());
            if (value.bpm) {
                exactReal(*value.bpm, expected["bpm"].toString().getDoubleValue());
            }
            expectEquals(value.key, expected["key"].toString());
            expectEquals(value.album, expected["album"].toString());
            expectEquals(value.genre, expected["genre"].toString());
            expectEquals(value.comment, expected["comment"].toString());
            expectEquals(value.year, expected["year"].isVoid() ? juce::String() : expected["year"].toString());
            break;
        }
    }
}

/// Pins record order, slot indices, signed loop boundaries and explicitly stored lock flags.
void RealMetadataTests::checkMarkers(const serato::Markers& value, const juce::var& expected)
{
    const auto& cues = expected["cues"];
    const auto& loops = expected["loops"];
    expectEquals(static_cast<int>(value.cues.size()), cues.size());
    expectEquals(static_cast<int>(value.loops.size()), loops.size());
    expect(value.colour.has_value());
    if (value.colour) {
        expectEquals(
            static_cast<juce::int64>(value.colour->getARGB()),
            static_cast<juce::int64>(opaqueArgb(expected["track_rgb"]))
        );
    }
    expect(value.bpmLocked.has_value());
    if (value.bpmLocked) {
        expect(*value.bpmLocked == static_cast<bool>(expected["bpm_locked"]));
    }
    for (std::size_t index = 0; index < std::min(value.cues.size(), static_cast<std::size_t>(cues.size())); ++index) {
        const auto& cue = value.cues[index];
        const auto& record = cues[static_cast<int>(index)];
        expectEquals(static_cast<int>(cue.index), static_cast<int>(record["index"]));
        expectEquals(
            static_cast<juce::int64>(cue.positionMilliseconds), static_cast<juce::int64>(record["position_ms"])
        );
        expectEquals(cue.name, record["name"].toString());
        expectEquals(
            static_cast<juce::int64>(cue.colour.getARGB()), static_cast<juce::int64>(opaqueArgb(record["rgb"]))
        );
    }
    for (std::size_t index = 0; index < std::min(value.loops.size(), static_cast<std::size_t>(loops.size())); ++index) {
        const auto& loop = value.loops[index];
        const auto& record = loops[static_cast<int>(index)];
        expectEquals(static_cast<int>(loop.index), static_cast<int>(record["index"]));
        expectEquals(static_cast<juce::int64>(loop.startMilliseconds), static_cast<juce::int64>(record["start_ms"]));
        expectEquals(static_cast<juce::int64>(loop.endMilliseconds), static_cast<juce::int64>(record["end_ms"]));
        expectEquals(loop.name, record["name"].toString());
        expect(loop.locked == static_cast<bool>(record["locked"]));
        expectEquals(
            static_cast<juce::int64>(loop.colour.getARGB()), static_cast<juce::int64>(opaqueArgb(record["rgb"]))
        );
    }
}

/// Hashes raw cells in stored order and checks the independent palette oracle including fallback.
void RealMetadataTests::checkOverview(const serato::Overview& value, const juce::var& expected)
{
    expectEquals(static_cast<int>(value.slices.size()), static_cast<int>(expected["slices"]));
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto& slice : value.slices) {
        for (const auto cell : slice) {
            hash = (hash ^ cell) * 0x100000001b3ULL;
        }
    }
    expectEquals(juce::String::toHexString(static_cast<juce::int64>(hash)), expected["fnv1a64"].toString());
    const juce::Colour fallback(17, 34, 51);
    const auto& samples = expected["samples"];
    for (const auto& sample : *samples.getArray()) {
        const auto index = static_cast<std::size_t>(static_cast<int>(sample["index"]));
        expect(index < value.slices.size(), "Sample lies within the parsed raster");
        if (index >= value.slices.size()) {
            continue;
        }
        const auto& slice = value.slices[index];
        for (std::size_t cell = 0; cell < slice.size(); ++cell) {
            expectEquals(static_cast<int>(slice[cell]), static_cast<int>(sample["cells"][static_cast<int>(cell)]));
        }
        const auto colour = serato::colourForOverviewSlice(slice, fallback);
        const auto oracle = sample["rgb"].isVoid() ? fallback : juce::Colour(opaqueArgb(sample["rgb"]));
        expectEquals(static_cast<juce::int64>(colour.getARGB()), static_cast<juce::int64>(oracle.getARGB()));
        serato::Overview isolated;
        isolated.slices.push_back(slice);
        const auto gradient = serato::makeOverviewGradient(isolated, {0.0, 1.0}, 1.0, {0, 0, 1, 1}, fallback);
        expect(gradient.has_value(), "Final waveform gradient");
        if (gradient) {
            expect(gradient->getColourAtPosition(0.5) == oracle, "Exact final waveform RGB");
        }
    }
}
/// Requires binary64 identity for stored grid positions and terminal tempos, never a timing tolerance.
void RealMetadataTests::exactReal(const double actual, const double expected)
{
    expect(
        std::bit_cast<std::uint64_t>(actual) == std::bit_cast<std::uint64_t>(expected),
        utils::format(
            "Actual bits {} expected {}",
            juce::String::toHexString(std::bit_cast<juce::int64>(actual)),
            juce::String::toHexString(std::bit_cast<juce::int64>(expected))
        )
    );
}
}  // namespace

/// Creates the read-only real container metadata suite.
std::unique_ptr<juce::UnitTest> createRealMetadataTests()
{
    return std::make_unique<RealMetadataTests>();
}
}  // namespace audiobatch::tests
