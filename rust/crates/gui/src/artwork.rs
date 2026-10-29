//! Bounded decoding of the embedded album artwork shown beside the file name in the information panel.
//!
//! The pictures come from `read_artwork_candidates` of the core crate, front covers first.
//! Each one is checked by its header before decoding,
//! so a picture that is not a PNG or a JPEG, a corrupt header,
//! or dimensions above `MAXIMUM_SOURCE_DIMENSION` are skipped without allocating the image.
//! The first picture that decodes becomes a BGRA thumbnail no larger than `THUMBNAIL_SIZE` on either side,
//! the pixel format gpui draws.
//! Everything here blocks, so it runs on a background thread.

use std::io::Cursor;
use std::path::Path;

use audiobatch_core::tags::read_artwork_candidates;
use gpui_kit::RenderImage;
use image::{DynamicImage, Frame, ImageDecoder as _, ImageFormat, ImageReader, Limits};

/// Largest width or height of a decoded thumbnail, twice the size it is drawn at for high density displays.
pub const THUMBNAIL_SIZE: u32 = 96;

/// Largest width or height a source picture may declare,
/// so a hostile header cannot make the decoder allocate gigabytes.
pub const MAXIMUM_SOURCE_DIMENSION: u32 = 4096;

/// Reads the pictures of a file and returns the thumbnail of the first one that decodes, front covers first.
/// Returns none when the file has no usable picture.
#[must_use]
pub fn load_artwork_thumbnail(path: &Path) -> Option<RenderImage> {
    read_artwork_candidates(path)
        .iter()
        .find_map(|data| decode_thumbnail(data))
}

/// Decodes a PNG or JPEG picture into a BGRA thumbnail no larger than `THUMBNAIL_SIZE` on either side.
/// Returns none for another format, a corrupt picture,
/// or declared dimensions of zero or above `MAXIMUM_SOURCE_DIMENSION`, checked before any pixel is decoded.
#[must_use]
pub fn decode_thumbnail(data: &[u8]) -> Option<RenderImage> {
    let pixels = decode_thumbnail_rgba(data)?;
    let mut buffer = pixels.into_rgba8();
    let (pixels, _) = buffer.as_chunks_mut::<4>();
    for pixel in pixels {
        pixel.swap(0, 2);
    }
    Some(RenderImage::new([Frame::new(buffer)]))
}

/// Decodes a PNG or JPEG picture and scales it down to fit `THUMBNAIL_SIZE`, keeping its aspect ratio.
/// The orientation a JPEG stores in its EXIF data is applied, so a rotated photo shows upright.
fn decode_thumbnail_rgba(data: &[u8]) -> Option<DynamicImage> {
    let mut reader = ImageReader::new(Cursor::new(data)).with_guessed_format().ok()?;
    if !matches!(reader.format(), Some(ImageFormat::Png | ImageFormat::Jpeg)) {
        return None;
    }
    let mut limits = Limits::default();
    limits.max_image_width = Some(MAXIMUM_SOURCE_DIMENSION);
    limits.max_image_height = Some(MAXIMUM_SOURCE_DIMENSION);
    reader.limits(limits);
    let mut decoder = reader.into_decoder().ok()?;
    let (width, height) = decoder.dimensions();
    if width == 0 || height == 0 || width > MAXIMUM_SOURCE_DIMENSION || height > MAXIMUM_SOURCE_DIMENSION {
        return None;
    }
    let orientation = decoder.orientation().ok()?;
    let mut image = DynamicImage::from_decoder(decoder).ok()?;
    image.apply_orientation(orientation);
    if image.width() > THUMBNAIL_SIZE || image.height() > THUMBNAIL_SIZE {
        image = image.resize(THUMBNAIL_SIZE, THUMBNAIL_SIZE, image::imageops::FilterType::Triangle);
    }
    Some(image)
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

    /// Returns the width and height of the first frame of a thumbnail.
    fn dimensions(image: &RenderImage) -> (i32, i32) {
        let size = image.size(0);
        let DevicePixels(width) = size.width;
        let DevicePixels(height) = size.height;
        (width, height)
    }

    #[test]
    fn small_pictures_keep_their_size_and_become_bgra() {
        let thumbnail = decode_thumbnail(&encoded(40, 20, ImageFormat::Png)).expect("decodes");
        assert_eq!(dimensions(&thumbnail), (40, 20));
        let bytes = thumbnail.as_bytes(0).expect("has a frame");
        assert_eq!(&bytes[..4], &[0, 0, 255, 255]);
    }

    #[test]
    fn large_pictures_scale_down_keeping_their_aspect_ratio() {
        let thumbnail = decode_thumbnail(&encoded(400, 200, ImageFormat::Jpeg)).expect("decodes");
        let expected = i32::try_from(THUMBNAIL_SIZE).expect("small size");
        assert_eq!(dimensions(&thumbnail), (expected, expected / 2));
    }

    #[test]
    fn oversized_corrupt_and_unsupported_pictures_are_skipped() {
        assert!(decode_thumbnail(&[]).is_none());
        assert!(decode_thumbnail(b"not an image at all").is_none());
        assert!(decode_thumbnail(&encoded(MAXIMUM_SOURCE_DIMENSION + 1, 1, ImageFormat::Png)).is_none());
        let jpeg = encoded(64, 64, ImageFormat::Jpeg);
        assert!(decode_thumbnail(&jpeg[..jpeg.len() / 4]).is_none());
    }
}
