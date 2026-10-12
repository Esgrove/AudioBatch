/// Synthetic tests for bounded Overview parsing and explicit-endian numeric serialization.
/// No extracted music payload or sibling repository is required by this portable suite.

#include "SeratoOverview.h"
#include "TestSuites.h"

#include <algorithm>

namespace audiobatch::tests
{
namespace
{
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
    SeratoOverviewTests() : juce::UnitTest("Serato Overview", "AudioBatch") { }

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
    }
};

std::unique_ptr<juce::UnitTest> createSeratoOverviewTests()
{
    return std::make_unique<SeratoOverviewTests>();
}
}  // namespace audiobatch::tests