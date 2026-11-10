/// Implements the single-worker, latest-pending artwork loader and its bounded thumbnail decoding.
/// Image headers are checked before decoding, so oversized or corrupt pictures are skipped instead of decoded.

#include "gui/ArtworkLoader.h"

#include "metadata/AudioMetadata.h"
#include "serato/SeratoBytes.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/ThreadUtils.h"
#include <stop_token>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace audiobatch
{
namespace
{
/// Width and height an image header declares.
struct ImageDimensions {
    // Preserve the width/height positional construction used by the image header decoders.
    /// Declared width in pixels.
    int width {0};
    /// Declared height in pixels.
    int height {0};
};

/// Bytes that open every PNG file.
constexpr std::array<std::uint8_t, 8> PNG_SIGNATURE {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
/// Type of the first PNG chunk, which holds the dimensions.
constexpr std::array<std::uint8_t, 4> PNG_HEADER_CHUNK {'I', 'H', 'D', 'R'};
/// Required byte length of the PNG IHDR data.
constexpr std::uint32_t PNG_HEADER_DATA_SIZE {13};
/// Offset of the first chunk type in a PNG file.
constexpr std::size_t PNG_HEADER_CHUNK_OFFSET {12};
/// Offset of the big-endian width in a PNG file. The height follows it.
constexpr std::size_t PNG_WIDTH_OFFSET {16};
/// Offset of the big-endian height in a PNG file.
constexpr std::size_t PNG_HEIGHT_OFFSET {20};
/// Bytes a PNG needs to hold its signature and dimensions.
constexpr std::size_t PNG_MINIMUM_SIZE {24};

/// Byte that starts every JPEG marker.
constexpr std::uint8_t JPEG_MARKER_PREFIX {0xFF};
/// Start-of-image marker.
constexpr std::uint8_t JPEG_START_OF_IMAGE {0xD8};
/// Temporary marker, which has no length field.
constexpr std::uint8_t JPEG_TEMPORARY {0x01};
/// First restart marker, which has no length field.
constexpr std::uint8_t JPEG_FIRST_RESTART {0xD0};
/// Last restart marker.
constexpr std::uint8_t JPEG_LAST_RESTART {0xD7};
/// First start-of-frame marker.
constexpr std::uint8_t JPEG_FIRST_START_OF_FRAME {0xC0};
/// Last start-of-frame marker.
constexpr std::uint8_t JPEG_LAST_START_OF_FRAME {0xCF};
/// Huffman table marker inside the start-of-frame range.
constexpr std::uint8_t JPEG_HUFFMAN_TABLE {0xC4};
/// Extension marker inside the start-of-frame range.
constexpr std::uint8_t JPEG_EXTENSION {0xC8};
/// Arithmetic coding marker inside the start-of-frame range.
constexpr std::uint8_t JPEG_ARITHMETIC_CODING {0xCC};
/// Bytes of a marker without its segment.
constexpr std::size_t JPEG_MARKER_SIZE {2};
/// Bytes of a marker with its segment length.
constexpr std::size_t JPEG_SEGMENT_HEADER_SIZE {4};
/// Offset of the height from a start-of-frame marker. The width follows it.
constexpr std::size_t JPEG_HEIGHT_OFFSET {5};
/// Offset of the width from a start-of-frame marker.
constexpr std::size_t JPEG_WIDTH_OFFSET {7};
/// Bytes from a start-of-frame marker to the end of its dimensions.
constexpr std::size_t JPEG_FRAME_DIMENSIONS_END {9};

/// Reads a big-endian unsigned value of the given type at an offset the caller checked.
template<std::unsigned_integral Value>
Value readBigEndianAt(const std::span<const std::uint8_t> bytes, const std::size_t offset)
{
    return serato::readBigEndian<Value>(bytes.subspan(offset).first<sizeof(Value)>());
}

/// Returns whether a declared size fits the artwork source limit.
bool isAllowedDimension(const std::int64_t value)
{
    return value > 0 && value <= ARTWORK_MAXIMUM_SOURCE_DIMENSION;
}

/// Reads the dimensions from the IHDR chunk of a PNG image, or nullopt when the header is not a PNG.
std::optional<ImageDimensions> readPngDimensions(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < PNG_MINIMUM_SIZE || !std::ranges::equal(bytes.first<PNG_SIGNATURE.size()>(), PNG_SIGNATURE)
        || readBigEndianAt<std::uint32_t>(bytes, PNG_SIGNATURE.size()) != PNG_HEADER_DATA_SIZE
        || !std::ranges::equal(bytes.subspan(PNG_HEADER_CHUNK_OFFSET, PNG_HEADER_CHUNK.size()), PNG_HEADER_CHUNK))
    {
        return std::nullopt;
    }
    const std::int64_t width = readBigEndianAt<std::uint32_t>(bytes, PNG_WIDTH_OFFSET);
    const std::int64_t height = readBigEndianAt<std::uint32_t>(bytes, PNG_HEIGHT_OFFSET);
    if (!isAllowedDimension(width) || !isAllowedDimension(height)) {
        return ImageDimensions {};
    }
    return ImageDimensions {.width = static_cast<int>(width), .height = static_cast<int>(height)};
}

/// Returns whether a JPEG marker stands alone, without a segment length.
bool isStandaloneJpegMarker(const std::uint8_t marker)
{
    return marker == JPEG_START_OF_IMAGE || marker == JPEG_TEMPORARY
        || (marker >= JPEG_FIRST_RESTART && marker <= JPEG_LAST_RESTART);
}

/// Returns whether a JPEG marker starts a frame header, which holds the dimensions.
bool isJpegStartOfFrame(const std::uint8_t marker)
{
    return marker >= JPEG_FIRST_START_OF_FRAME && marker <= JPEG_LAST_START_OF_FRAME && marker != JPEG_HUFFMAN_TABLE
        && marker != JPEG_EXTENSION && marker != JPEG_ARITHMETIC_CODING;
}

/// Walks the JPEG markers to the first start-of-frame segment and reads its dimensions,
/// or returns nullopt when the data is not a JPEG.
std::optional<ImageDimensions> readJpegDimensions(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < JPEG_SEGMENT_HEADER_SIZE || bytes[0] != JPEG_MARKER_PREFIX || bytes[1] != JPEG_START_OF_IMAGE) {
        return std::nullopt;
    }
    std::size_t position {JPEG_MARKER_SIZE};
    while (position + JPEG_SEGMENT_HEADER_SIZE <= bytes.size()) {
        if (bytes[position] != JPEG_MARKER_PREFIX) {
            return ImageDimensions {};
        }
        const auto marker = bytes[position + 1];
        if (marker == JPEG_MARKER_PREFIX) {
            ++position;
            continue;
        }
        if (isStandaloneJpegMarker(marker)) {
            position += JPEG_MARKER_SIZE;
            continue;
        }
        const std::size_t length = readBigEndianAt<std::uint16_t>(bytes, position + JPEG_MARKER_SIZE);
        if (length < JPEG_MARKER_SIZE || length > bytes.size() - position - JPEG_MARKER_SIZE) {
            return ImageDimensions {};
        }
        if (isJpegStartOfFrame(marker)) {
            if (length < JPEG_FRAME_DIMENSIONS_END - JPEG_MARKER_SIZE
                || position + JPEG_FRAME_DIMENSIONS_END > bytes.size())
            {
                return ImageDimensions {};
            }
            const std::int64_t height = readBigEndianAt<std::uint16_t>(bytes, position + JPEG_HEIGHT_OFFSET);
            const std::int64_t width = readBigEndianAt<std::uint16_t>(bytes, position + JPEG_WIDTH_OFFSET);
            if (!isAllowedDimension(width) || !isAllowedDimension(height)) {
                return ImageDimensions {};
            }
            return ImageDimensions {.width = static_cast<int>(width), .height = static_cast<int>(height)};
        }
        position += JPEG_MARKER_SIZE + length;
    }
    return ImageDimensions {};
}
}  // namespace

/// Starts the worker. Completion runs on that worker and must only post delivery, never wait for the UI.
ArtworkLoader::ArtworkLoader(Completion onCompletion, Decoder decoder) :
    completion(std::move(onCompletion)),
    decode(std::move(decoder)),
    worker([this](const std::stop_token& stopToken) { run(stopToken); })
{}

/// Invalidates queued deliveries and joins the worker before releasing the callback captures.
ArtworkLoader::~ArtworkLoader()
{
    cancelAndWait();
}

/// Reads the pictures of a file and returns the thumbnail of the first one that decodes, front covers first.
/// Returns an invalid image when the file has no usable picture or the request is cancelled.
juce::Image ArtworkLoader::loadArtworkThumbnail(const juce::File& file, const utils::CancellationCheck& isCancelled)
{
    if (isCancelled && isCancelled()) {
        return {};
    }
    for (const auto& picture : readArtworkCandidates(file)) {
        if (isCancelled && isCancelled()) {
            return {};
        }
        if (auto image
            = decodeThumbnail(std::span(static_cast<const std::byte*>(picture.data.getData()), picture.data.getSize()));
            image.isValid())
        {
            return image;
        }
    }
    return {};
}

/// Decodes a JPEG or PNG picture into a thumbnail no larger than artworkThumbnailSize on either side.
/// The header is checked first, so another format, a corrupt header,
/// or dimensions above artworkMaximumSourceDimension return an invalid image without decoding.
juce::Image ArtworkLoader::decodeThumbnail(const std::span<const std::byte> encoded)
{
    const void* data = encoded.data();
    const auto size = encoded.size();
    if (data == nullptr || size == 0 || size > ArtworkReadLimits {}.maximumBytes) {
        return {};
    }
    const std::span bytes(static_cast<const std::uint8_t*>(data), size);
    auto dimensions = readPngDimensions(bytes);
    if (!dimensions) {
        dimensions = readJpegDimensions(bytes);
    }
    if (!dimensions || dimensions->width <= 0 || dimensions->height <= 0) {
        return {};
    }

    constexpr std::size_t bytesPerPixel {4};
    const auto widthPixels = static_cast<std::size_t>(dimensions->width);
    const auto heightPixels = static_cast<std::size_t>(dimensions->height);
    if (widthPixels > ARTWORK_MAXIMUM_DECODED_BYTES / bytesPerPixel / heightPixels) {
        return {};
    }

    juce::MemoryInputStream stream(data, size, false);
    auto* format = juce::ImageFileFormat::findImageFormatForStream(stream);
    if (format == nullptr) {
        return {};
    }
    stream.setPosition(0);
    const auto image = format->decodeImage(stream);
    if (!image.isValid() || image.getWidth() != dimensions->width || image.getHeight() != dimensions->height) {
        return {};
    }

    const auto scale = juce::jmin(
        1.0,
        static_cast<double>(ARTWORK_THUMBNAIL_SIZE)
            / static_cast<double>(juce::jmax(image.getWidth(), image.getHeight()))
    );
    if (scale >= 1.0) {
        return image.convertedToFormat(juce::Image::ARGB);
    }
    const auto width = juce::jmax(1, juce::roundToInt(image.getWidth() * scale));
    const auto height = juce::jmax(1, juce::roundToInt(image.getHeight() * scale));
    return image.rescaled(width, height, juce::Graphics::highResamplingQuality).convertedToFormat(juce::Image::ARGB);
}

/// Replaces pending work and invalidates earlier reads and deliveries.
/// A missing file cancels prior work without scheduling a read and returns zero.
/// Called by the owner thread, never from completion.
std::uint64_t ArtworkLoader::request(const juce::File& file)
{
    const utils::FileIdentity identity(file);
    std::uint64_t nextGeneration = 0;
    {
        const std::scoped_lock lock(mutex);
        nextGeneration = ++generation;
        pending.reset();
        if (!stopped && file.existsAsFile() && identity.matches(file)) {
            pending = ArtworkRequest {.file = file, .identity = identity, .generation = nextGeneration};
        } else {
            nextGeneration = 0;
        }
    }
    condition.notify_one();
    return nextGeneration;
}

/// Invalidates active and queued completions and drops pending work without waiting for TagLib.
void ArtworkLoader::cancel()
{
    {
        const std::scoped_lock lock(mutex);
        ++generation;
        pending.reset();
    }
    condition.notify_one();
}

/// Stops permanently and joins the worker. Safe to call repeatedly.
void ArtworkLoader::cancelAndWait()
{
    stopped = true;
    cancel();
    worker.request_stop();
    condition.notify_one();
    if (worker.joinable()) {
        worker.join();
    }
}

/// Checks the generation, the selected path, and the current file stats again at delivery time.
bool ArtworkLoader::canAdopt(
    const utils::FileIdentity& identity,
    const std::uint64_t resultGeneration,
    const juce::File& selectedFile
) const
{
    return isCurrentGeneration(resultGeneration) && identity.path == selectedFile.getFullPathName()
        && identity.matches(selectedFile);
}

/// Waits interruptibly for the latest request and processes it outside the queue lock.
void ArtworkLoader::run(const std::stop_token& stopToken)
{
    while (!stopToken.stop_requested()) {
        std::optional<ArtworkRequest> active;
        {
            std::unique_lock lock(mutex);
            if (!condition.wait(lock, stopToken, [this] { return pending.has_value(); })) {
                return;
            }
            active = std::move(pending);
            pending.reset();
        }
        if (active && !stopToken.stop_requested()) {
            process(*active, stopToken);
        }
    }
}

/// Decodes the artwork of one request and delivers it while the request is current and the file unchanged.
/// Identity is checked before extraction, between candidate decodes, and before delivering a result.
void ArtworkLoader::process(const ArtworkRequest& active, const std::stop_token& stopToken) const
{
    const utils::CancellationCheck isCancelled = [this, &active, stopToken] {
        return stopToken.stop_requested() || !isCurrentGeneration(active.generation)
            || !active.identity.matches(active.file);
    };
    if (isCancelled()) {
        return;
    }
    juce::Image image;
    try {
        if (active.identity.matches(active.file) && decode) {
            image = decode(active.file, isCancelled);
        }
    } catch (...) {
        // The worker is a std::jthread, where an escaping exception would terminate the process.
        utils::logDebug("Artwork read failed: {}", utils::describeCurrentException());
        image = {};
    }
    if (!active.identity.matches(active.file)) {
        image = {};
    }
    if (!isCancelled() && completion) {
        completion(active.identity, active.generation, std::move(image));
    }
}

/// Checks whether a result still belongs to the latest request.
bool ArtworkLoader::isCurrentGeneration(const std::uint64_t resultGeneration) const
{
    return !stopped && generation == resultGeneration;
}
}  // namespace audiobatch
