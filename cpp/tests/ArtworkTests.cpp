/// Tests for album artwork extraction and the artwork loader.
/// Covers front cover ordering, the picture size and count limits, bounded thumbnail decoding,
/// rejection of corrupt and oversized images, and dropping results of replaced or changed files.

#include "gui/ArtworkLoader.h"
#include "metadata/AudioMetadata.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include <condition_variable>

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <flacfile.h>
#include <flacpicture.h>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Encodes a solid colour image of the given size as PNG.
juce::MemoryBlock encodePng(const int width, const int height, const juce::Colour colour)
{
    juce::Image image(juce::Image::ARGB, width, height, true);
    image.clear(image.getBounds(), colour);
    juce::MemoryOutputStream stream;
    juce::PNGImageFormat format;
    static_cast<void>(format.writeImageToStream(image, stream));
    return stream.getMemoryBlock();
}

/// Embeds the pictures into a FLAC file as native picture blocks.
bool addFlacPictures(
    const juce::File& file,
    const std::vector<std::pair<juce::MemoryBlock, TagLib::FLAC::Picture::Type>>& pictures
)
{
    TagLib::FLAC::File tagged(utils::tagLibFileName(file.getFullPathName()), false);
    for (const auto& [data, type] : pictures) {
        auto picture = std::make_unique<TagLib::FLAC::Picture>();
        picture->setMimeType("image/png");
        picture->setType(type);
        picture->setData(
            TagLib::ByteVector(static_cast<const char*>(data.getData()), static_cast<unsigned int>(data.getSize()))
        );
        tagged.addPicture(picture.release());
    }
    return tagged.save();
}

/// Collects the completions of an artwork loader so the test thread can wait for them.
struct CompletionLog {
    /// One delivered result.
    struct Entry {
        utils::FileIdentity identity;
        std::uint64_t generation {0};
        juce::Image image;
    };

    /// Records a completion from the worker thread.
    void add(utils::FileIdentity identity, const std::uint64_t generation, juce::Image image)
    {
        {
            const std::scoped_lock lock(mutex);
            entries.push_back({.identity = std::move(identity), .generation = generation, .image = std::move(image)});
        }
        condition.notify_all();
    }

    /// Waits until the given number of completions arrived, or the timeout passed.
    bool waitFor(const std::size_t count)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(10), [this, count] { return entries.size() >= count; });
    }

    /// Returns a copy of the completions so far.
    std::vector<Entry> snapshot()
    {
        const std::scoped_lock lock(mutex);
        return entries;
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<Entry> entries;
};
}  // namespace

/// Checks artwork reading and decoding on temporary fixture copies, without a GUI.
class ArtworkTests final : public juce::UnitTest
{
public:
    ArtworkTests() : juce::UnitTest("Artwork", "AudioBatch") {}

    /// Runs the extraction, decoding, and loader checks.
    void runTest() override
    {
        testCandidates();
        testDecoding();
        testLoader();
    }

private:
    /// Checks that front covers come first and that the limits skip large pictures and cap the count.
    void testCandidates()
    {
        beginTest("Artwork candidates put front covers first and respect the size and count limits");
        const TemporaryDirectory directory;
        const auto file = directory.copyFixture("sine_stereo_44100_16.flac", "artwork.flac");
        expect(readArtworkCandidates(file).empty());

        const auto back = encodePng(8, 8, juce::Colours::blue);
        const auto front = encodePng(16, 16, juce::Colours::red);
        const auto large = encodePng(512, 512, juce::Colours::green);
        expect(addFlacPictures(
            file,
            {
                {back, TagLib::FLAC::Picture::BackCover},
                {large, TagLib::FLAC::Picture::Artist},
                {front, TagLib::FLAC::Picture::FrontCover},
            }
        ));

        const auto all = readArtworkCandidates(file);
        expectEquals(static_cast<int>(all.size()), 3);
        expectEquals(all.front().pictureType, juce::String("Front Cover"));
        expect(all.front().data == front);

        const auto limited = readArtworkCandidates(file, {.maximumBytes = back.getSize(), .maximumCount = 4});
        expectEquals(static_cast<int>(limited.size()), 1);
        expect(limited.front().data == back);

        expectEquals(static_cast<int>(readArtworkCandidates(file, {.maximumCount = 1}).size()), 1);
        expect(readArtworkCandidates(directory.getChildFile("missing.flac")).empty());

        const auto thumbnail = ArtworkLoader::loadArtworkThumbnail(file, {});
        expect(thumbnail.isValid());
        expectEquals(thumbnail.getWidth(), 16);
        expect(thumbnail.getPixelAt(4, 4) == juce::Colours::red);
    }

    /// Checks that decoding keeps small images, scales large ones, and rejects corrupt and huge headers.
    void testDecoding()
    {
        beginTest("Thumbnails are bounded, and corrupt or oversized images are skipped without decoding");
        const auto small = encodePng(40, 20, juce::Colours::white);
        const auto smallImage = ArtworkLoader::decodeThumbnail(small.getData(), small.getSize());
        expectEquals(smallImage.getWidth(), 40);
        expectEquals(smallImage.getHeight(), 20);

        const auto wide = encodePng(400, 200, juce::Colours::white);
        const auto wideImage = ArtworkLoader::decodeThumbnail(wide.getData(), wide.getSize());
        expectEquals(wideImage.getWidth(), artworkThumbnailSize);
        expectEquals(wideImage.getHeight(), artworkThumbnailSize / 2);

        expect(!ArtworkLoader::decodeThumbnail(nullptr, 0).isValid());
        const juce::MemoryBlock garbage("not an image at all", 19);
        expect(!ArtworkLoader::decodeThumbnail(garbage.getData(), garbage.getSize()).isValid());

        auto huge = encodePng(4, 4, juce::Colours::white);
        const std::span bytes(static_cast<std::uint8_t*>(huge.getData()), huge.getSize());
        // Declares 65536 x 65536 in the IHDR chunk, which the header check must reject before decoding.
        constexpr std::array<std::uint8_t, 8> hugeDimensions {0, 1, 0, 0, 0, 1, 0, 0};
        std::ranges::copy(hugeDimensions, bytes.subspan(16).begin());
        expect(!ArtworkLoader::decodeThumbnail(huge.getData(), huge.getSize()).isValid());

        constexpr std::array<std::uint8_t, 7> truncatedJpeg {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08};
        expect(!ArtworkLoader::decodeThumbnail(truncatedJpeg.data(), truncatedJpeg.size()).isValid());
    }

    /// Checks that a replaced request never completes, the latest one does,
    /// and that adoption is refused for an old generation, another selection, or a changed file.
    void testLoader()
    {
        beginTest("The artwork loader delivers only the latest request and refuses stale adoption");
        const TemporaryDirectory directory;
        const auto first = directory.copyFixture("sine_stereo_44100_16.flac", "first.flac");
        const auto second = directory.copyFixture("sine_stereo_44100_16.flac", "second.flac");

        std::mutex gateMutex;
        std::condition_variable gateCondition;
        bool firstStarted = false;
        bool released = false;
        CompletionLog log;
        {
            ArtworkLoader loader(
                [&log](utils::FileIdentity identity, const std::uint64_t generation, juce::Image image) {
                    log.add(std::move(identity), generation, std::move(image));
                },
                [&](const juce::File& file, const utils::CancellationCheck& isCancelled) {
                    if (file == first) {
                        std::unique_lock lock(gateMutex);
                        firstStarted = true;
                        gateCondition.notify_all();
                        gateCondition.wait(lock, [&released] { return released; });
                    }
                    if (isCancelled()) {
                        return juce::Image();
                    }
                    juce::Image image(juce::Image::ARGB, 4, 4, true);
                    image.clear(image.getBounds(), juce::Colours::orange);
                    return image;
                }
            );

            const auto firstGeneration = loader.request(first);
            expect(firstGeneration != 0);
            {
                std::unique_lock lock(gateMutex);
                expect(gateCondition.wait_for(lock, std::chrono::seconds(10), [&firstStarted] {
                    return firstStarted;
                }));
            }
            const auto secondGeneration = loader.request(second);
            expect(secondGeneration > firstGeneration);
            {
                const std::scoped_lock lock(gateMutex);
                released = true;
            }
            gateCondition.notify_all();

            // The replaced first request never completes, which the count after joining the worker confirms.
            expect(log.waitFor(1));
            const auto entries = log.snapshot();
            const auto& entry = entries.front();
            expect(entry.generation == secondGeneration);
            expect(entry.image.isValid());
            expect(loader.canAdopt(entry.identity, secondGeneration, second));
            expect(!loader.canAdopt(entry.identity, firstGeneration, second));
            expect(!loader.canAdopt(entry.identity, secondGeneration, first));

            expect(second.setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::hours(1)));
            expect(!loader.canAdopt(entry.identity, secondGeneration, second));

            expectEquals(loader.request(directory.getChildFile("missing.flac")), std::uint64_t {0});
            loader.cancel();
            expect(!loader.canAdopt(entry.identity, secondGeneration, second));
        }
        expectEquals(static_cast<int>(log.snapshot().size()), 1);
    }
};

/// Creates the artwork suite for TestMain.cpp.
std::unique_ptr<juce::UnitTest> createArtworkTests()
{
    return std::make_unique<ArtworkTests>();
}
}  // namespace audiobatch::tests
