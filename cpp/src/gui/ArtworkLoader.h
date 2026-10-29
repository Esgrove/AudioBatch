/// Bounded asynchronous loading of the embedded album artwork shown beside the preview.
/// ArtworkLoader reads the pictures of one file on its own worker, independent of the Serato tag reads,
/// and decodes the first usable one into a small thumbnail.
/// Like SeratoPreviewLoader, it keeps at most one active read and one replaceable pending request,
/// and a request replaced or cancelled before it finishes never completes.

#pragma once

#include "utils/FileUtils.h"
#include "utils/ThreadUtils.h"
#include <condition_variable>
#include <stop_token>

#include <JuceHeader.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace audiobatch
{
/// Largest width or height of a decoded thumbnail, twice the 48 px it is drawn at for high density displays.
constexpr int artworkThumbnailSize {96};

/// Largest width or height a source image may declare, so a hostile header cannot make the decoder allocate gigabytes.
constexpr int artworkMaximumSourceDimension {4096};

/// One artwork request, with the file identity it was made for.
struct ArtworkRequest {
    /// The file to read the pictures of.
    juce::File file;
    /// Size and modification time of the file when it was requested.
    utils::FileIdentity identity;
    /// Number of the request, so a result for an older selection is recognised and dropped.
    std::uint64_t generation {0};
};

/// Reads and decodes the artwork of the selected file on a worker thread.
class ArtworkLoader final
{
public:
    /// Receives the requested identity and generation, with an invalid image when the file has no usable artwork.
    /// Runs on the worker, so it must only post the result to the message thread.
    using Completion = std::function<void(utils::FileIdentity, std::uint64_t, juce::Image)>;
    /// Returns the thumbnail of a file, or an invalid image, checking the cancellation between pictures.
    using Decoder = std::function<juce::Image(const juce::File&, const utils::CancellationCheck&)>;

    explicit ArtworkLoader(Completion onCompletion, Decoder decoder = loadArtworkThumbnail);

    ~ArtworkLoader();

    ArtworkLoader(const ArtworkLoader&) = delete;
    ArtworkLoader& operator=(const ArtworkLoader&) = delete;
    ArtworkLoader(ArtworkLoader&&) = delete;
    ArtworkLoader& operator=(ArtworkLoader&&) = delete;

    static juce::Image loadArtworkThumbnail(const juce::File& file, const utils::CancellationCheck& isCancelled);
    static juce::Image decodeThumbnail(const void* data, std::size_t size);

    std::uint64_t request(const juce::File& file);

    void cancel();
    void cancelAndWait();

    [[nodiscard]] bool canAdopt(
        const utils::FileIdentity& identity,
        std::uint64_t resultGeneration,
        const juce::File& selectedFile
    ) const;

private:
    void run(const std::stop_token& stopToken);
    void process(const ArtworkRequest& active, const std::stop_token& stopToken) const;

    [[nodiscard]] bool isCurrentGeneration(std::uint64_t resultGeneration) const;

    /// Receives the thumbnail of each current request on the worker thread.
    Completion completion;
    /// Reads and decodes the artwork, replaceable in tests.
    Decoder decode;

    /// Counts the requests and cancellations, so a result of an older request is never delivered.
    std::atomic<std::uint64_t> generation {0};
    /// True once the loader stopped for good.
    std::atomic<bool> stopped {false};
    /// Wakes the worker when a request arrives or the loader stops.
    std::condition_variable_any condition;
    /// Guards the pending request.
    std::mutex mutex;
    /// The latest request the worker has not started yet, which a newer request replaces.
    std::optional<ArtworkRequest> pending;

    /// Starts only after every member it accesses has been initialized.
    std::jthread worker;
};
}  // namespace audiobatch
