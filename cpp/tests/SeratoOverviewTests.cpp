/// Synthetic tests for bounded Overview parsing, explicit-endian numeric serialization,
/// and the colouring math that decodes the palette indices and spreads them over the visible waveform.
/// No extracted music payload or sibling repository is required by this portable suite.

#include "serato/SeratoOverview.h"
#include "serato/SeratoWaveformColouring.h"
#include "TestSuites.h"
#include <initializer_list>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Palette index of pure red in the 6x6x6 colour cube.
constexpr std::uint8_t redIndex {180};
/// Palette index of pure green in the 6x6x6 colour cube.
constexpr std::uint8_t greenIndex {30};

/// Returns a slice whose every cell holds the same palette index.
std::array<std::uint8_t, serato::Overview::bandCount> uniformSlice(const std::uint8_t paletteIndex)
{
    std::array<std::uint8_t, serato::Overview::bandCount> slice {};
    slice.fill(paletteIndex);
    return slice;
}

/// Returns an overview with one uniform slice per palette index.
serato::Overview overviewOf(const std::initializer_list<std::uint8_t> paletteIndices)
{
    serato::Overview overview;
    for (const auto paletteIndex : paletteIndices) {
        overview.slices.push_back(uniformSlice(paletteIndex));
    }
    return overview;
}

/// Constructs synthetic slices with distinct band bytes and the supported payload header.
std::vector<std::uint8_t> overviewPayload(std::size_t count)
{
    std::vector<std::uint8_t> bytes(2 + count * serato::Overview::bandCount);
    bytes[0] = 1;
    bytes[1] = 5;
    for (std::size_t index = 2; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((index - 2) % 256);
    }
    return bytes;
}

/// Exposes a cache block to the span-based deserializer without copying it.
std::span<const std::uint8_t> cacheBytes(const juce::MemoryBlock& block)
{
    return {static_cast<const std::uint8_t*>(block.getData()), block.getSize()};
}
}  // namespace

/// Checks ownership, strict payload boundaries and portable cache validation.
class SeratoOverviewTests final : public juce::UnitTest
{
public:
    /// Registers the suite with JUCE's explicit factory runner.
    SeratoOverviewTests() : juce::UnitTest("Serato Overview", "AudioBatch") {}

    /// Exercises synthetic payloads only, including the exact allocation limit.
    void runTest() override
    {
        beginTest("Complete nonstandard counts are retained and input ownership is independent");
        for (const std::size_t count : {1U, 3U, 240U, 257U}) {
            auto payload = overviewPayload(count);
            const auto overview = serato::Overview::parse(payload);
            expect(overview.has_value());
            if (!overview) {
                continue;
            }
            expectEquals(overview->slices.size(), count);
            expectEquals(static_cast<int>(overview->slices.front()[15]), 15);
            const auto expected = overview->slices;
            std::ranges::fill(payload, std::uint8_t {0});
            expect(overview->slices == expected);
        }

        beginTest("Short payloads, unknown headers and partial tails are rejected");
        for (const std::size_t size : {0U, 1U, 2U, 3U, 17U, 19U, 33U}) {
            auto payload = overviewPayload(2);
            payload.resize(size);
            expect(!serato::Overview::parse(payload));
        }
        for (const std::size_t position : {0U, 1U}) {
            auto payload = overviewPayload(1);
            payload[position] = 0xff;
            expect(!serato::Overview::parse(payload));
        }

        beginTest("Zero bands are valid and the maximum complete slice count is bounded");
        {
            auto payload = overviewPayload(1);
            std::ranges::fill(std::span(payload).subspan(2), std::uint8_t {0});
            const auto silent = serato::Overview::parse(payload);
            expect(silent.has_value());
            if (silent) {
                expect(std::ranges::all_of(silent->slices.front(), [](auto value) { return value == 0; }));
            }
            const auto maximum = serato::Overview::parse(overviewPayload(serato::Overview::maxSlices));
            expect(maximum.has_value());
            if (maximum) {
                expectEquals(maximum->slices.size(), serato::Overview::maxSlices);
                const auto restored = serato::Overview::deserialize(cacheBytes(maximum->serialize()));
                expect(restored.has_value());
                if (restored) {
                    expect(restored->slices == maximum->slices);
                }
            }
            expect(!serato::Overview::parse(overviewPayload(serato::Overview::maxSlices + 1)));
        }

        beginTest("Cache header has explicit magic, kind, version and big-endian count");
        {
            const auto overview = serato::Overview::parse(overviewPayload(257));
            expect(overview.has_value());
            if (overview) {
                const auto block = overview->serialize();
                const auto bytes = cacheBytes(block);
                const std::array<std::uint8_t, 16> expectedHeader {
                    'S',
                    'O',
                    'V',
                    'R',
                    0,
                    0,
                    0,
                    1,
                    0,
                    0,
                    0,
                    1,
                    0,
                    0,
                    1,
                    1,
                };
                expectEquals(bytes.size(), std::size_t {16 + 257 * 16});
                expect(std::ranges::equal(bytes.first(16), expectedHeader));
                const auto restored = serato::Overview::deserialize(bytes);
                expect(restored.has_value());
                if (restored) {
                    expect(restored->slices == overview->slices);
                }

                beginTest("Corrupt cache fields, truncated bands and extra bytes are rejected");
                for (const std::size_t offset : {0U, 7U, 11U, 12U, 15U}) {
                    auto corrupt = block;
                    const auto changed = static_cast<std::uint8_t>(cacheBytes(corrupt)[offset] ^ 0xffU);
                    corrupt.copyFrom(&changed, static_cast<int>(offset), 1);
                    expect(!serato::Overview::deserialize(cacheBytes(corrupt)));
                }
                for (const auto length : {std::size_t {0}, std::size_t {15}, block.getSize() - 1, block.getSize() + 1})
                {
                    auto corrupt = block;
                    corrupt.setSize(length, true);
                    expect(!serato::Overview::deserialize(cacheBytes(corrupt)));
                }
                auto zeroCount = block;
                const std::array<std::uint8_t, 4> zeroBytes {};
                zeroCount.copyFrom(zeroBytes.data(), 12, zeroBytes.size());
                expect(!serato::Overview::deserialize(cacheBytes(zeroCount)));
            }
        }

        beginTest("Invalid public slice vectors cannot produce cache entries");
        {
            serato::Overview overview;
            expectEquals(overview.serialize().getSize(), std::size_t {0});
            overview.slices.resize(serato::Overview::maxSlices + 1);
            expectEquals(overview.serialize().getSize(), std::size_t {0});
        }

        runColouringTests();
    }

private:
    /// Checks that a colour is opaque and has the given components, allowing one step of rounding.
    void expectColour(const juce::Colour colour, const int red, const int green, const int blue)
    {
        const auto description = colour.toDisplayString(true);
        expectEquals(static_cast<int>(colour.getAlpha()), 255, description);
        expect(std::abs(static_cast<int>(colour.getRed()) - red) <= 1, description);
        expect(std::abs(static_cast<int>(colour.getGreen()) - green) <= 1, description);
        expect(std::abs(static_cast<int>(colour.getBlue()) - blue) <= 1, description);
    }

    /// Checks the palette decoding of one slice and the gradient built across the visible part of the track.
    void runColouringTests()
    {
        const auto fallback = juce::Colours::blue.withAlpha(0.25f);

        beginTest("Palette indices decode through the zero-based 6x6x6 colour cube");
        expectColour(serato::colourForOverviewSlice(uniformSlice(2), fallback), 0, 0, 102);
        expectColour(serato::colourForOverviewSlice(uniformSlice(43), fallback), 51, 51, 51);
        expectColour(serato::colourForOverviewSlice(uniformSlice(100), fallback), 102, 204, 204);
        expectColour(serato::colourForOverviewSlice(uniformSlice(redIndex), fallback), 255, 0, 0);
        expectColour(serato::colourForOverviewSlice(uniformSlice(greenIndex), fallback), 0, 255, 0);
        expectColour(serato::colourForOverviewSlice(uniformSlice(215), fallback), 255, 255, 255);

        beginTest("Background and reserved cells are skipped, and a slice without colour uses the opaque fallback");
        {
            auto slice = uniformSlice(0);
            slice[1] = 1;
            slice[2] = 216;
            slice[3] = 255;
            slice[15] = 215;
            expectColour(serato::colourForOverviewSlice(slice, fallback), 255, 255, 255);
            slice[15] = 0;
            expectColour(serato::colourForOverviewSlice(slice, fallback), 0, 0, 255);
        }

        beginTest("A slice colour is the mean of its coloured cells, whatever their rows");
        {
            auto slice = uniformSlice(redIndex);
            std::ranges::fill(std::span(slice).first(8), greenIndex);
            const auto mixed = serato::colourForOverviewSlice(slice, fallback);
            expectColour(mixed, 128, 128, 0);
            std::ranges::reverse(slice);
            expect(serato::colourForOverviewSlice(slice, fallback) == mixed);
            // Background cells do not darken the mean of the remaining green cells.
            std::ranges::fill(std::span(slice).first(8), std::uint8_t {0});
            expectColour(serato::colourForOverviewSlice(slice, fallback), 0, 255, 0);
        }

        beginTest("The gradient spans the waveform and its ends sample the visible times");
        {
            const auto overview = overviewOf({redIndex, greenIndex});
            const juce::Rectangle bounds(10, 0, 4, 20);
            const auto gradient = serato::makeOverviewGradient(overview, {0.0, 2.0}, 2.0, bounds, fallback);
            expect(gradient.has_value());
            if (gradient) {
                expectWithinAbsoluteError(gradient->point1.x, 10.0f, 0.001f);
                expectWithinAbsoluteError(gradient->point2.x, 14.0f, 0.001f);
                expect(!gradient->isRadial);
                expectEquals(gradient->getNumColours(), 6);
                expectColour(gradient->getColour(0), 255, 0, 0);
                expectColour(gradient->getColour(gradient->getNumColours() - 1), 0, 255, 0);
            }
        }

        beginTest("At close zoom the colour blends between the centres of adjacent slices");
        {
            const auto overview = overviewOf({redIndex, greenIndex});
            // Halfway between the slice centres at 0.5 s and 1.5 s.
            const auto gradient
                = serato::makeOverviewGradient(overview, {0.9, 1.1}, 2.0, juce::Rectangle(0, 0, 1, 20), fallback);
            expect(gradient.has_value());
            if (gradient) {
                expectEquals(gradient->getNumColours(), 3);
                expectWithinAbsoluteError(gradient->getColourPosition(1), 0.5, 0.001);
                expectColour(gradient->getColour(1), 128, 128, 0);
            }
        }

        beginTest("When a pixel spans several slices, each slice counts by its overlap");
        {
            const auto overview = overviewOf({redIndex, greenIndex, greenIndex, greenIndex});
            // The single pixel covers half of the red slice and one and a half green slices.
            const auto gradient
                = serato::makeOverviewGradient(overview, {0.25, 1.25}, 2.0, juce::Rectangle(0, 0, 1, 20), fallback);
            expect(gradient.has_value());
            if (gradient) {
                expectEquals(gradient->getNumColours(), 3);
                expectColour(gradient->getColour(1), 64, 191, 0);
            }
        }

        beginTest("Invalid overviews, lengths, ranges and bounds produce no gradient");
        {
            const auto overview = overviewOf({redIndex});
            const juce::Rectangle bounds(0, 0, 10, 10);
            const auto infinity = std::numeric_limits<double>::infinity();
            const serato::Overview emptyOverview;
            expect(!serato::makeOverviewGradient(emptyOverview, {0.0, 1.0}, 1.0, bounds, fallback));
            expect(!serato::makeOverviewGradient(overview, {0.0, 1.0}, 1.0, {}, fallback));
            expect(!serato::makeOverviewGradient(overview, {0.0, 1.0}, 0.0, bounds, fallback));
            expect(!serato::makeOverviewGradient(overview, {0.0, 1.0}, infinity, bounds, fallback));
            expect(!serato::makeOverviewGradient(overview, {0.5, 0.5}, 1.0, bounds, fallback));
            expect(!serato::makeOverviewGradient(overview, {0.0, infinity}, 1.0, bounds, fallback));
        }

        beginTest("The number of gradient stops is bounded on a very wide waveform");
        {
            const auto gradient = serato::makeOverviewGradient(
                overviewOf({redIndex, greenIndex}), {0.0, 1.0}, 1.0, juce::Rectangle(0, 0, 100'000, 10), fallback
            );
            expect(gradient.has_value());
            if (gradient) {
                expectEquals(gradient->getNumColours(), serato::maxWaveformGradientStops);
            }
        }
    }
};

std::unique_ptr<juce::UnitTest> createSeratoOverviewTests()
{
    return std::make_unique<SeratoOverviewTests>();
}
}  // namespace audiobatch::tests
