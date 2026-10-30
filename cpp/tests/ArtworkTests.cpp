/// Tests for album artwork extraction and the artwork loader.
/// Covers front cover ordering, the picture size and count limits, bounded thumbnail decoding,
/// rejection of corrupt and oversized images, and dropping results of replaced or changed files.

#include "gui/ArtworkLoader.h"
#include "gui/AudioBatchComponentState.h"
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

/// One delivered artwork result.
struct ArtworkCompletionEntry {
    /// File version requested by the worker.
    utils::FileIdentity identity;
    /// Generation delivered by the worker.
    std::uint64_t generation {0};
    /// Thumbnail, invalid for a completed no-art result.
    juce::Image image;
};

/// Collects the completions of an artwork loader so the test thread can wait for them.
struct CompletionLog {
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
    std::vector<ArtworkCompletionEntry> snapshot()
    {
        const std::scoped_lock lock(mutex);
        return entries;
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<ArtworkCompletionEntry> entries;
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
        testInvalidation();
        testNoArtState();
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

        beginTest("An invalid front cover falls back to another bounded embedded picture");
        const auto fallback = directory.copyFixture("sine_stereo_44100_16.flac", "fallback.flac");
        expect(addFlacPictures(
            fallback,
            {{back, TagLib::FLAC::Picture::BackCover}, {juce::MemoryBlock("bad", 3), TagLib::FLAC::Picture::FrontCover}}
        ));
        const auto fallbackImage = ArtworkLoader::loadArtworkThumbnail(fallback, {});
        expect(fallbackImage.isValid());
        expect(fallbackImage.getPixelAt(2, 2) == juce::Colours::blue);
        expect(!ArtworkLoader::loadArtworkThumbnail(file, [] { return true; }).isValid());
        expect(readArtworkCandidates(file, {.maximumCount = 0}).empty());
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

        const juce::Image jpegSource(juce::Image::RGB, 12, 6, true);
        juce::JPEGImageFormat jpegFormat;
        juce::MemoryOutputStream jpegStream;
        expect(jpegFormat.writeImageToStream(jpegSource, jpegStream));
        const auto jpeg = ArtworkLoader::decodeThumbnail(jpegStream.getData(), jpegStream.getDataSize());
        expectEquals(jpeg.getWidth(), 12);
        expectEquals(jpeg.getHeight(), 6);

        expect(!ArtworkLoader::decodeThumbnail(nullptr, 0).isValid());
        // An encoded-byte limit is checked before accessing even the signature.
        expect(!ArtworkLoader::decodeThumbnail(small.getData(), ArtworkReadLimits {}.maximumBytes + 1).isValid());
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
            const auto latestGeneration = loader.request(first);
            expect(secondGeneration > firstGeneration);
            expect(latestGeneration > secondGeneration);
            {
                const std::scoped_lock lock(gateMutex);
                released = true;
            }
            gateCondition.notify_all();

            // The replaced first request never completes, which the count after joining the worker confirms.
            expect(log.waitFor(1));
            const auto entries = log.snapshot();
            const auto& entry = entries.front();
            expect(entry.generation == latestGeneration);
            expect(entry.identity.path == first.getFullPathName());
            expect(entry.image.isValid());
            expect(loader.canAdopt(entry.identity, latestGeneration, first));
            expect(!loader.canAdopt(entry.identity, firstGeneration, first));
            expect(!loader.canAdopt(entry.identity, latestGeneration, second));

            expect(first.setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::hours(1)));
            expect(!loader.canAdopt(entry.identity, latestGeneration, first));

            expectEquals(loader.request(directory.getChildFile("missing.flac")), std::uint64_t {0});
            loader.cancel();
            expect(!loader.canAdopt(entry.identity, secondGeneration, second));
        }
        expectEquals(static_cast<int>(log.snapshot().size()), 1);

        beginTest("A completed no-art result cannot be adopted after disable, clear or forced refresh");
        CompletionLog noArtLog;
        ArtworkLoader noArtLoader(
            [&noArtLog](utils::FileIdentity identity, const std::uint64_t generation, juce::Image image) {
                noArtLog.add(std::move(identity), generation, std::move(image));
            },
            [](const juce::File&, const utils::CancellationCheck&) { return juce::Image(); }
        );
        const auto noArtGeneration = noArtLoader.request(second);
        expect(noArtLog.waitFor(1));
        const auto noArtEntries = noArtLog.snapshot();
        if (!noArtEntries.empty()) {
            const auto& entry = noArtEntries.front();
            expect(!entry.image.isValid());
            expect(noArtLoader.canAdopt(entry.identity, noArtGeneration, second));
            noArtLoader.cancel();
            expect(!noArtLoader.canAdopt(entry.identity, noArtGeneration, second));
            expect(noArtLoader.request(second) > noArtGeneration);
            expect(!noArtLoader.canAdopt(entry.identity, noArtGeneration, second));
        }
    }

    /// Exercises cancellation and same-path replacement while the active decoder is gated, without sleeps.
    void testInvalidation()
    {
        beginTest("Disable, release and shutdown invalidate active and pending work, including same-path replacements");
        const TemporaryDirectory directory;
        const auto file = directory.copyFixture("sine_stereo_44100_16.flac", "gated.flac");
        for (const bool replaceFile : {false, true}) {
            std::mutex mutex;
            std::condition_variable condition;
            bool started = false;
            bool released = false;
            bool sawCancellation = false;
            bool decoded = false;
            CompletionLog log;
            ArtworkLoader loader(
                [&log](utils::FileIdentity identity, const std::uint64_t generation, juce::Image image) {
                    log.add(std::move(identity), generation, std::move(image));
                },
                [&](const juce::File&, const utils::CancellationCheck& isCancelled) {
                    std::unique_lock lock(mutex);
                    started = true;
                    condition.notify_all();
                    condition.wait(lock, [&released] { return released; });
                    sawCancellation = isCancelled();
                    decoded = true;
                    condition.notify_all();
                    return juce::Image();
                }
            );
            const auto generation = loader.request(file);
            {
                std::unique_lock lock(mutex);
                expect(condition.wait_for(lock, std::chrono::seconds(10), [&started] { return started; }));
            }
            const utils::FileIdentity identity(file);
            if (replaceFile) {
                expect(file.setLastModificationTime(file.getLastModificationTime() + juce::RelativeTime::hours(1)));
            } else {
                static_cast<void>(loader.request(file));
                loader.cancel();
            }
            expect(!loader.canAdopt(identity, generation, file));
            {
                const std::scoped_lock lock(mutex);
                released = true;
            }
            condition.notify_all();
            {
                std::unique_lock lock(mutex);
                expect(condition.wait_for(lock, std::chrono::seconds(10), [&decoded] { return decoded; }));
                // Observe the cancellation before shutdown itself changes the generation.
                expect(sawCancellation);
            }
            loader.cancelAndWait();
            expect(log.snapshot().empty());
            expectEquals(loader.request(file), std::uint64_t {0});
            loader.cancelAndWait();
        }
    }

    /// Checks the non-rendering state shared by all panel refresh and invalidation paths.
    void testNoArtState()
    {
        beginTest("Completed no-art results and pending requests are cached by identity until explicitly invalidated");
        const TemporaryDirectory directory;
        const auto file = directory.copyFixture("sine_stereo_44100_16.flac", "no-art.flac");
        ArtworkState state;
        state.file = file;
        expect(state.needsRequest());
        state.requested.emplace(file);
        expect(!state.needsRequest());
        state.completed = true;
        expect(!state.image.isValid());
        expect(!state.needsRequest());
        expect(file.setLastModificationTime(file.getLastModificationTime() + juce::RelativeTime::hours(1)));
        expect(state.needsRequest());
        state.requested.emplace(file);
        expect(!state.needsRequest());
        state.invalidate();
        expect(state.file == file);
        expect(state.needsRequest());
        expect(!state.completed);
        expect(!state.image.isValid());
    }
};

/// Creates the artwork suite for TestMain.cpp.
std::unique_ptr<juce::UnitTest> createArtworkTests()
{
    return std::make_unique<ArtworkTests>();
}
}  // namespace audiobatch::tests
