//! Bounded decoding of the embedded album artwork shown beside the file name in the information panel.
//!
//! Pictures are read independently of display tags and Serato cues, front covers first.
//! Encoded size, format, dimensions, and pixel storage are checked before pixel decoding,
//! with an explicit allocation budget passed to the decoder.
//! Only a BGRA thumbnail no larger than `THUMBNAIL_SIZE` on either side is retained.
//! Everything here blocks, so it runs on a background thread.

use std::io::Cursor;
use std::path::Path;

use audiobatch_core::tags::{ARTWORK_MAXIMUM_BYTES, ARTWORK_MAXIMUM_COUNT, read_artwork_candidates};
use gpui_kit::RenderImage;
use image::{DynamicImage, Frame, ImageDecoder as _, ImageFormat, ImageReader, Limits, RgbaImage};

/// Largest width or height of a decoded thumbnail, twice the size it is drawn at for high density displays.
pub const THUMBNAIL_SIZE: u32 = 96;

/// Largest width or height a source picture may declare,
/// so a hostile header cannot make the decoder allocate gigabytes.
pub const MAXIMUM_SOURCE_DIMENSION: u32 = 4096;

/// Allocation budget given to image decoders, enough for a bounded source with eight bytes per pixel.
/// Decoder allocation limits are best effort, while dimension and output storage checks are strict.
const MAXIMUM_DECODE_BYTES: u64 = 128 * 1024 * 1024;

/// Reads the pictures of a file and returns the thumbnail of the first one that decodes, front covers first.
/// Returns none when the file has no usable picture.
#[must_use]
#[expect(
    dead_code,
    reason = "Keeps the entry point for callers without a cancellable request lifecycle."
)]
pub fn load_artwork_thumbnail(path: &Path) -> Option<RenderImage> {
    load_artwork_thumbnail_if_current(path, || true)
}

/// Reads artwork only while the caller's generation and file identity remain current.
///
/// Checks before reading tags, before each candidate decode, and after decoding.
/// Reading tags or decoding one candidate cannot be interrupted, so callers must also check at foreground adoption.
#[must_use]
pub fn load_artwork_thumbnail_if_current(path: &Path, is_current: impl Fn() -> bool) -> Option<RenderImage> {
    read_thumbnail_if_current(path, &is_current, read_artwork_candidates, decode_thumbnail)
}

/// Decodes a PNG or JPEG picture into a BGRA thumbnail no larger than `THUMBNAIL_SIZE` on either side.
/// Returns none for another format, a corrupt picture, encoded bytes above 16 MiB,
/// or declared dimensions of zero or above `MAXIMUM_SOURCE_DIMENSION`, checked before pixel decoding.
#[must_use]
pub fn decode_thumbnail(data: &[u8]) -> Option<RenderImage> {
    Some(render_thumbnail(decode_thumbnail_rgba(data)?))
}

/// Reads bounded candidates and tries each only while the request is current.
/// Reader and decoder injection lets lifecycle tests cover cancellation without GUI rendering or file races.
fn read_thumbnail_if_current<Thumbnail>(
    path: &Path,
    is_current: &impl Fn() -> bool,
    read: impl FnOnce(&Path) -> Vec<Vec<u8>>,
    decode: impl Fn(&[u8]) -> Option<Thumbnail>,
) -> Option<Thumbnail> {
    if !is_current() {
        return None;
    }
    let candidates = read(path);
    for data in candidates.iter().take(ARTWORK_MAXIMUM_COUNT) {
        if !is_current() {
            return None;
        }
        if let Some(thumbnail) = decode(data) {
            return is_current().then_some(thumbnail);
        }
    }
    None
}

/// Converts RGBA thumbnail pixels to the BGRA frame format gpui draws.
fn render_thumbnail(mut buffer: RgbaImage) -> RenderImage {
    let (pixels, _) = buffer.as_chunks_mut::<4>();
    for pixel in pixels {
        pixel.swap(0, 2);
    }
    RenderImage::new([Frame::new(buffer)])
}

/// Returns the explicit limits used for header parsing and pixel decoding.
fn decode_limits() -> Limits {
    let mut limits = Limits::default();
    limits.max_image_width = Some(MAXIMUM_SOURCE_DIMENSION);
    limits.max_image_height = Some(MAXIMUM_SOURCE_DIMENSION);
    limits.max_alloc = Some(MAXIMUM_DECODE_BYTES);
    limits
}

/// Checks dimensions and calculates output storage without overflowing before any pixels are allocated.
fn checked_source_bytes(width: u32, height: u32, bytes_per_pixel: u8) -> Option<u64> {
    if width == 0 || height == 0 || width > MAXIMUM_SOURCE_DIMENSION || height > MAXIMUM_SOURCE_DIMENSION {
        return None;
    }
    let bytes = u64::from(width)
        .checked_mul(u64::from(height))?
        .checked_mul(u64::from(bytes_per_pixel))?;
    (bytes > 0 && bytes <= MAXIMUM_DECODE_BYTES).then_some(bytes)
}

/// Decodes a PNG or JPEG picture and scales it down to fit `THUMBNAIL_SIZE`, keeping its aspect ratio.
/// The orientation a JPEG stores in its EXIF data is applied, so a rotated photo shows upright.
fn decode_thumbnail_rgba(data: &[u8]) -> Option<RgbaImage> {
    if data.is_empty() || data.len() > ARTWORK_MAXIMUM_BYTES {
        return None;
    }
    let format = image::guess_format(data).ok()?;
    if !matches!(format, ImageFormat::Png | ImageFormat::Jpeg) {
        return None;
    }
    let mut reader = ImageReader::with_format(Cursor::new(data), format);
    reader.limits(decode_limits());
    let mut decoder = reader.into_decoder().ok()?;
    let (width, height) = decoder.dimensions();
    let source_bytes = checked_source_bytes(width, height, decoder.color_type().bytes_per_pixel())?;
    if decoder.total_bytes() != source_bytes {
        return None;
    }
    // Account for the output allocation as well as the decoder's own supported allocation limits.
    let mut limits = decode_limits();
    limits.reserve(source_bytes).ok()?;
    decoder.set_limits(limits).ok()?;
    let orientation = decoder.orientation().ok()?;
    let mut image = DynamicImage::from_decoder(decoder).ok()?;
    image.apply_orientation(orientation);
    if image.width() > THUMBNAIL_SIZE || image.height() > THUMBNAIL_SIZE {
        image = image.resize(THUMBNAIL_SIZE, THUMBNAIL_SIZE, image::imageops::FilterType::Triangle);
    }
    Some(image.into_rgba8())
}

#[cfg(test)]
mod test_decode_thumbnail {
    use gpui_kit::DevicePixels;
    use image::{ImageBuffer, Rgb, RgbImage};

    use super::*;

    /// Encodes a solid image of a size in a format.
    fn encoded(width: u32, height: u32, format: ImageFormat) -> Vec<u8> {
        let image: RgbImage = ImageBuffer::from_pixel(width, height, Rgb([255, 0, 0]));
        let mut bytes = Cursor::new(Vec::new());
        image.write_to(&mut bytes, format).expect("encodes the image");
        bytes.into_inner()
    }

    #[test]
    fn small_png_and_jpeg_keep_their_size() {
        for format in [ImageFormat::Png, ImageFormat::Jpeg] {
            let thumbnail = decode_thumbnail_rgba(&encoded(40, 20, format)).expect("decodes");
            assert_eq!(thumbnail.dimensions(), (40, 20));
        }
        let thumbnail = decode_thumbnail_rgba(&encoded(1, 1, ImageFormat::Png)).expect("decodes tiny PNG");
        assert_eq!(thumbnail.get_pixel(0, 0).0, [255, 0, 0, 255]);
    }

    #[test]
    fn small_pictures_keep_their_size_and_become_bgra() {
        let thumbnail = decode_thumbnail(&encoded(40, 20, ImageFormat::Png)).expect("decodes");
        let size = thumbnail.size(0);
        assert_eq!(size.width, DevicePixels(40));
        assert_eq!(size.height, DevicePixels(20));
        let bytes = thumbnail.as_bytes(0).expect("has a frame");
        assert_eq!(&bytes[..4], &[0, 0, 255, 255]);
    }

    #[test]
    fn large_pictures_scale_down_keeping_their_aspect_ratio() {
        let thumbnail = decode_thumbnail(&encoded(400, 200, ImageFormat::Jpeg)).expect("decodes");
        let size = thumbnail.size(0);
        let expected = i32::try_from(THUMBNAIL_SIZE).expect("small size");
        assert_eq!(size.width, DevicePixels(expected));
        assert_eq!(size.height, DevicePixels(expected / 2));
    }

    #[test]
    fn large_and_tall_pictures_keep_their_aspect_ratio() {
        for (width, height, expected) in [(400, 200, (96, 48)), (200, 400, (48, 96)), (1, 400, (1, 96))] {
            let thumbnail = decode_thumbnail_rgba(&encoded(width, height, ImageFormat::Jpeg)).expect("decodes");
            assert_eq!(thumbnail.dimensions(), expected);
        }
    }

    #[test]
    fn oversized_corrupt_and_unsupported_pictures_are_skipped() {
        for data in [
            b"".as_slice(),
            b"not an image at all",
            b"GIF89a\x01\0\x01\0",
            b"\x89PNG\r\n\x1a\n",
        ] {
            assert!(decode_thumbnail_rgba(data).is_none());
        }
        for format in [ImageFormat::Png, ImageFormat::Jpeg] {
            for (width, height) in [(MAXIMUM_SOURCE_DIMENSION + 1, 1), (1, MAXIMUM_SOURCE_DIMENSION + 1)] {
                assert!(decode_thumbnail_rgba(&encoded(width, height, format)).is_none());
            }
        }
        let jpeg = encoded(64, 64, ImageFormat::Jpeg);
        assert!(decode_thumbnail_rgba(&jpeg[..jpeg.len() / 4]).is_none());
    }

    #[test]
    fn encoded_size_limit_also_applies_to_direct_decode() {
        let mut data = encoded(1, 1, ImageFormat::Png);
        data.resize(ARTWORK_MAXIMUM_BYTES, 0);
        assert!(decode_thumbnail_rgba(&data).is_some());
        data.push(0);
        assert!(decode_thumbnail(&data).is_none());
    }

    #[test]
    fn dimensions_and_storage_are_checked_against_explicit_limits() {
        assert_eq!(checked_source_bytes(4096, 4096, 8), Some(MAXIMUM_DECODE_BYTES));
        for (width, height, bytes_per_pixel) in [(0, 1, 4), (1, 0, 4), (4097, 1, 4), (1, 4097, 4), (1, 1, 0)] {
            assert!(checked_source_bytes(width, height, bytes_per_pixel).is_none());
        }
        assert!(checked_source_bytes(u32::MAX, u32::MAX, u8::MAX).is_none());
        assert!(checked_source_bytes(4096, 4096, 9).is_none());
        let limits = decode_limits();
        assert_eq!(limits.max_image_width, Some(4096));
        assert_eq!(limits.max_image_height, Some(4096));
        assert_eq!(limits.max_alloc, Some(MAXIMUM_DECODE_BYTES));
    }

    #[test]
    fn candidate_decoding_falls_back_after_corrupt_front_cover() {
        let fallback = encoded(3, 2, ImageFormat::Png);
        let thumbnail = read_thumbnail_if_current(
            Path::new("artwork.flac"),
            &|| true,
            |_| vec![b"corrupt front cover".to_vec(), fallback],
            decode_thumbnail_rgba,
        )
        .expect("decodes fallback");
        assert_eq!(thumbnail.dimensions(), (3, 2));
    }
}

#[cfg(test)]
mod test_artwork_cancellation {
    use std::cell::Cell;

    use super::*;

    #[test]
    fn stale_request_does_not_read_tags() {
        let thumbnail: Option<()> = read_thumbnail_if_current(
            Path::new("missing.wav"),
            &|| false,
            |_| panic!("must not read"),
            |_| panic!("must not decode"),
        );
        assert!(thumbnail.is_none());
        assert!(load_artwork_thumbnail_if_current(Path::new("missing.wav"), || false).is_none());
    }

    #[test]
    fn cancellation_after_tag_parsing_skips_all_decodes() {
        let current = Cell::new(true);
        let thumbnail: Option<()> = read_thumbnail_if_current(
            Path::new("artwork.flac"),
            &|| current.get(),
            |_| {
                current.set(false);
                vec![vec![1]]
            },
            |_| panic!("must not decode stale candidates"),
        );
        assert!(thumbnail.is_none());
    }

    #[test]
    fn cancellation_between_candidates_prevents_fallback_decode() {
        let current = Cell::new(true);
        let decodes = Cell::new(0);
        let thumbnail: Option<()> = read_thumbnail_if_current(
            Path::new("artwork.flac"),
            &|| current.get(),
            |_| vec![vec![1], vec![2]],
            |_| {
                decodes.set(decodes.get() + 1);
                current.set(false);
                None
            },
        );
        assert!(thumbnail.is_none());
        assert_eq!(decodes.get(), 1);
    }

    #[test]
    fn cancellation_during_decode_discards_the_result() {
        let current = Cell::new(true);
        let thumbnail = read_thumbnail_if_current(
            Path::new("artwork.flac"),
            &|| current.get(),
            |_| vec![vec![1]],
            |_| {
                current.set(false);
                Some(())
            },
        );
        assert!(thumbnail.is_none());
    }

    #[test]
    fn decoding_attempts_are_bounded_and_empty_candidates_are_harmless() {
        let decodes = Cell::new(0);
        let thumbnail: Option<()> = read_thumbnail_if_current(
            Path::new("artwork.flac"),
            &|| true,
            |_| vec![vec![1]; ARTWORK_MAXIMUM_COUNT + 1],
            |_| {
                decodes.set(decodes.get() + 1);
                None
            },
        );
        assert!(thumbnail.is_none());
        assert_eq!(decodes.get(), ARTWORK_MAXIMUM_COUNT);
        assert!(read_thumbnail_if_current(Path::new("missing.wav"), &|| true, |_| vec![], |_| Some(())).is_none());
    }
}
