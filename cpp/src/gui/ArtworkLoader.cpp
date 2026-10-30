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
    /// Declared width in pixels.
    int width {0};
    /// Declared height in pixels.
    int height {0};
};

/// Bytes that open every PNG file.
constexpr std::array<std::uint8_t, 8> pngSignature {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
/// Type of the first PNG chunk, which holds the dimensions.
constexpr std::array<std::uint8_t, 4> pngHeaderChunk {'I', 'H', 'D', 'R'};
/// Required byte length of the PNG IHDR data.
constexpr std::uint32_t pngHeaderDataSize {13};
/// Offset of the first chunk type in a PNG file.
constexpr std::size_t pngHeaderChunkOffset {12};
/// Offset of the big-endian width in a PNG file. The height follows it.
constexpr std::size_t pngWidthOffset {16};
/// Offset of the big-endian height in a PNG file.
constexpr std::size_t pngHeightOffset {20};
/// Bytes a PNG needs to hold its signature and dimensions.
constexpr std::size_t pngMinimumSize {24};

/// Byte that starts every JPEG marker.
constexpr std::uint8_t jpegMarkerPrefix {0xFF};
/// Start-of-image marker.
constexpr std::uint8_t jpegStartOfImage {0xD8};
/// Temporary marker, which has no length field.
constexpr std::uint8_t jpegTemporary {0x01};
/// First restart marker, which has no length field.
constexpr std::uint8_t jpegFirstRestart {0xD0};
/// Last restart marker.
constexpr std::uint8_t jpegLastRestart {0xD7};
/// First start-of-frame marker.
constexpr std::uint8_t jpegFirstStartOfFrame {0xC0};
/// Last start-of-frame marker.
constexpr std::uint8_t jpegLastStartOfFrame {0xCF};
/// Huffman table marker inside the start-of-frame range.
constexpr std::uint8_t jpegHuffmanTable {0xC4};
/// Extension marker inside the start-of-frame range.
constexpr std::uint8_t jpegExtension {0xC8};
/// Arithmetic coding marker inside the start-of-frame range.
constexpr std::uint8_t jpegArithmeticCoding {0xCC};
/// Bytes of a marker without its segment.
constexpr std::size_t jpegMarkerSize {2};
/// Bytes of a marker with its segment length.
constexpr std::size_t jpegSegmentHeaderSize {4};
/// Offset of the height from a start-of-frame marker. The width follows it.
constexpr std::size_t jpegHeightOffset {5};
/// Offset of the width from a start-of-frame marker.
constexpr std::size_t jpegWidthOffset {7};
/// Bytes from a start-of-frame marker to the end of its dimensions.
constexpr std::size_t jpegFrameDimensionsEnd {9};

/// Reads a big-endian unsigned value of the given type at an offset the caller checked.
template<std::unsigned_integral Value>
Value readBigEndianAt(const std::span<const std::uint8_t> bytes, const std::size_t offset)
{
    return serato::readBigEndian<Value>(bytes.subspan(offset).first<sizeof(Value)>());
}

/// Returns whether a declared size fits the artwork source limit.
bool isAllowedDimension(const std::int64_t value)
{
    return value > 0 && value <= artworkMaximumSourceDimension;
}

/// Reads the dimensions from the IHDR chunk of a PNG image, or nullopt when the header is not a PNG.
std::optional<ImageDimensions> readPngDimensions(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < pngMinimumSize || !std::ranges::equal(bytes.first<pngSignature.size()>(), pngSignature)
        || readBigEndianAt<std::uint32_t>(bytes, pngSignature.size()) != pngHeaderDataSize
        || !std::ranges::equal(bytes.subspan(pngHeaderChunkOffset, pngHeaderChunk.size()), pngHeaderChunk))
    {
        return std::nullopt;
    }
    const std::int64_t width = readBigEndianAt<std::uint32_t>(bytes, pngWidthOffset);
    const std::int64_t height = readBigEndianAt<std::uint32_t>(bytes, pngHeightOffset);
    if (!isAllowedDimension(width) || !isAllowedDimension(height)) {
        return ImageDimensions {};
    }
    return ImageDimensions {.width = static_cast<int>(width), .height = static_cast<int>(height)};
}

/// Returns whether a JPEG marker stands alone, without a segment length.
bool isStandaloneJpegMarker(const std::uint8_t marker)
{
    return marker == jpegStartOfImage || marker == jpegTemporary
        || (marker >= jpegFirstRestart && marker <= jpegLastRestart);
}

/// Returns whether a JPEG marker starts a frame header, which holds the dimensions.
bool isJpegStartOfFrame(const std::uint8_t marker)
{
    return marker >= jpegFirstStartOfFrame && marker <= jpegLastStartOfFrame && marker != jpegHuffmanTable
        && marker != jpegExtension && marker != jpegArithmeticCoding;
}

/// Walks the JPEG markers to the first start-of-frame segment and reads its dimensions,
/// or returns nullopt when the data is not a JPEG.
std::optional<ImageDimensions> readJpegDimensions(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < jpegSegmentHeaderSize || bytes[0] != jpegMarkerPrefix || bytes[1] != jpegStartOfImage) {
        return std::nullopt;
    }
    std::size_t position {jpegMarkerSize};
    while (position + jpegSegmentHeaderSize <= bytes.size()) {
        if (bytes[position] != jpegMarkerPrefix) {
            return ImageDimensions {};
        }
        const auto marker = bytes[position + 1];
        if (marker == jpegMarkerPrefix) {
            ++position;
            continue;
        }
        if (isStandaloneJpegMarker(marker)) {
            position += jpegMarkerSize;
            continue;
        }
        const std::size_t length = readBigEndianAt<std::uint16_t>(bytes, position + jpegMarkerSize);
        if (length < jpegMarkerSize || length > bytes.size() - position - jpegMarkerSize) {
            return ImageDimensions {};
        }
        if (isJpegStartOfFrame(marker)) {
            if (length < jpegFrameDimensionsEnd - jpegMarkerSize || position + jpegFrameDimensionsEnd > bytes.size()) {
                return ImageDimensions {};
            }
            const std::int64_t height = readBigEndianAt<std::uint16_t>(bytes, position + jpegHeightOffset);
            const std::int64_t width = readBigEndianAt<std::uint16_t>(bytes, position + jpegWidthOffset);
            if (!isAllowedDimension(width) || !isAllowedDimension(height)) {
                return ImageDimensions {};
            }
            return ImageDimensions {.width = static_cast<int>(width), .height = static_cast<int>(height)};
        }
        position += jpegMarkerSize + length;
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
        if (auto image = decodeThumbnail(picture.data.getData(), picture.data.getSize()); image.isValid()) {
            return image;
        }
    }
    return {};
}

/// Decodes a JPEG or PNG picture into a thumbnail no larger than artworkThumbnailSize on either side.
/// The header is checked first, so another format, a corrupt header,
/// or dimensions above artworkMaximumSourceDimension return an invalid image without decoding.
juce::Image ArtworkLoader::decodeThumbnail(const void* data, const std::size_t size)
{
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
    if (widthPixels > artworkMaximumDecodedBytes / bytesPerPixel / heightPixels) {
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
        static_cast<double>(artworkThumbnailSize) / static_cast<double>(juce::jmax(image.getWidth(), image.getHeight()))
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
