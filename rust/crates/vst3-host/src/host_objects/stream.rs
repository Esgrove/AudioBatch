//! An in-memory byte stream for reading and writing plugin state.

#![expect(unsafe_code, reason = "implements the IBStream and ISizeableStream COM interfaces")]

use std::ffi::c_void;
use std::sync::{Mutex, PoisonError};

use vst3::Steinberg::IBStream_::IStreamSeekMode_;
use vst3::Steinberg::{
    IBStream, IBStreamTrait, ISizeableStream, ISizeableStreamTrait, kInvalidArgument, kResultFalse, kResultOk, tresult,
};
use vst3::{Class, ComWrapper};

/// Largest stream a plugin may write or size, well above the state of large sample-based instruments.
const MAXIMUM_STREAM_BYTES: usize = 1 << 30;

/// A growable in-memory stream.
#[derive(Default)]
pub struct MemoryStream {
    /// The contents and position.
    buffer: Mutex<StreamBuffer>,
}

/// The bytes of a stream and the position of the next read or write.
#[derive(Debug, Default)]
struct StreamBuffer {
    /// Stream contents.
    data: Vec<u8>,
    /// Position of the next read or write, which may lie past the end after a seek.
    position: usize,
}

impl Class for MemoryStream {
    type Interfaces = (IBStream, ISizeableStream);
}

impl MemoryStream {
    /// Creates an empty stream for a plugin to write into.
    pub fn create() -> ComWrapper<Self> {
        ComWrapper::new(Self::default())
    }

    /// Creates a stream positioned at the start of `data` for a plugin to read.
    pub fn with_data(data: Vec<u8>) -> ComWrapper<Self> {
        ComWrapper::new(Self {
            buffer: Mutex::new(StreamBuffer { data, position: 0 }),
        })
    }

    /// Returns a copy of the stream contents.
    pub fn data(&self) -> Vec<u8> {
        self.buffer.lock().unwrap_or_else(PoisonError::into_inner).data.clone()
    }
}

impl IBStreamTrait for MemoryStream {
    unsafe fn read(&self, buffer: *mut c_void, num_bytes: i32, num_bytes_read: *mut i32) -> tresult {
        let Ok(requested) = usize::try_from(num_bytes) else {
            return kInvalidArgument;
        };
        if buffer.is_null() && requested > 0 {
            return kInvalidArgument;
        }
        let mut stream = self.buffer.lock().unwrap_or_else(PoisonError::into_inner);
        let available = stream.data.len().saturating_sub(stream.position);
        let count = requested.min(available);
        if count > 0 {
            let source = stream
                .data
                .get(stream.position..stream.position + count)
                .unwrap_or_default();
            // SAFETY: the plugin passes a buffer of at least `num_bytes` bytes, and `count` is no larger.
            unsafe { std::ptr::copy_nonoverlapping(source.as_ptr(), buffer.cast::<u8>(), count) };
            stream.position += count;
        }
        drop(stream);
        if !num_bytes_read.is_null() {
            // SAFETY: the count slot is writable, checked for null above.
            unsafe { *num_bytes_read = i32::try_from(count).unwrap_or(i32::MAX) };
        }
        kResultOk
    }

    unsafe fn write(&self, buffer: *mut c_void, num_bytes: i32, num_bytes_written: *mut i32) -> tresult {
        let Ok(count) = usize::try_from(num_bytes) else {
            return kInvalidArgument;
        };
        if buffer.is_null() && count > 0 {
            return kInvalidArgument;
        }
        let mut stream = self.buffer.lock().unwrap_or_else(PoisonError::into_inner);
        if count > 0 {
            // SAFETY: the plugin passes `num_bytes` readable bytes, checked for null above.
            let source = unsafe { std::slice::from_raw_parts(buffer.cast::<u8>(), count) };
            let start = stream.position;
            // A seek far past the end followed by a write would otherwise overflow or grow the buffer without bound.
            let Some(end) = start.checked_add(count).filter(|&end| end <= MAXIMUM_STREAM_BYTES) else {
                return kResultFalse;
            };
            if stream.data.len() < end {
                stream.data.resize(end, 0);
            }
            if let Some(target) = stream.data.get_mut(start..end) {
                target.copy_from_slice(source);
            }
            stream.position = end;
        }
        drop(stream);
        if !num_bytes_written.is_null() {
            // SAFETY: the count slot is writable, checked for null above.
            unsafe { *num_bytes_written = num_bytes };
        }
        kResultOk
    }

    unsafe fn seek(&self, position: i64, mode: i32, result: *mut i64) -> tresult {
        let mut stream = self.buffer.lock().unwrap_or_else(PoisonError::into_inner);
        let mode = i64::from(mode);
        let origin = if mode == i64::from(IStreamSeekMode_::kIBSeekSet) {
            0
        } else if mode == i64::from(IStreamSeekMode_::kIBSeekCur) {
            i64::try_from(stream.position).unwrap_or(i64::MAX)
        } else if mode == i64::from(IStreamSeekMode_::kIBSeekEnd) {
            i64::try_from(stream.data.len()).unwrap_or(i64::MAX)
        } else {
            return kInvalidArgument;
        };
        let Some(Ok(target)) = origin.checked_add(position).map(usize::try_from) else {
            return kResultFalse;
        };
        stream.position = target;
        drop(stream);
        if !result.is_null() {
            // SAFETY: the result slot is writable, checked for null above.
            unsafe { *result = i64::try_from(target).unwrap_or(i64::MAX) };
        }
        kResultOk
    }

    unsafe fn tell(&self, position: *mut i64) -> tresult {
        if position.is_null() {
            return kInvalidArgument;
        }
        let stream = self.buffer.lock().unwrap_or_else(PoisonError::into_inner);
        // SAFETY: the result slot is writable, checked for null above.
        unsafe { *position = i64::try_from(stream.position).unwrap_or(i64::MAX) };
        kResultOk
    }
}

impl ISizeableStreamTrait for MemoryStream {
    unsafe fn getStreamSize(&self, size: *mut i64) -> tresult {
        if size.is_null() {
            return kInvalidArgument;
        }
        let stream = self.buffer.lock().unwrap_or_else(PoisonError::into_inner);
        // SAFETY: the result slot is writable, checked for null above.
        unsafe { *size = i64::try_from(stream.data.len()).unwrap_or(i64::MAX) };
        kResultOk
    }

    unsafe fn setStreamSize(&self, size: i64) -> tresult {
        let Ok(size) = usize::try_from(size) else {
            return kInvalidArgument;
        };
        if size > MAXIMUM_STREAM_BYTES {
            return kResultFalse;
        }
        self.buffer
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .data
            .resize(size, 0);
        kResultOk
    }
}

#[cfg(test)]
mod test_memory_stream {
    use super::*;
    use crate::enums::to_i32;

    #[test]
    fn writes_and_sizes_past_the_limit_are_refused() {
        let stream = MemoryStream::default();
        let mut payload = *b"x";
        let mut written = 0;
        // SAFETY: the payload buffer is live and one byte long.
        unsafe {
            assert_eq!(
                stream.seek(i64::MAX, to_i32(IStreamSeekMode_::kIBSeekSet), std::ptr::null_mut()),
                kResultOk
            );
            assert_eq!(
                stream.write(payload.as_mut_ptr().cast(), 1, &raw mut written),
                kResultFalse
            );
            assert_eq!(stream.setStreamSize(i64::MAX), kResultFalse);
        }
        assert_eq!(stream.data(), Vec::<u8>::new());
    }

    #[test]
    fn written_bytes_read_back() {
        let stream = MemoryStream::default();
        let mut payload = *b"state";
        let mut written = 0;
        let mut read_back = [0_u8; 8];
        let mut read = 0;
        // SAFETY: every buffer is live and at least as large as the sizes passed.
        unsafe {
            assert_eq!(
                stream.write(payload.as_mut_ptr().cast(), 5, &raw mut written),
                kResultOk
            );
            assert_eq!(
                stream.seek(0, to_i32(IStreamSeekMode_::kIBSeekSet), std::ptr::null_mut()),
                kResultOk
            );
            assert_eq!(stream.read(read_back.as_mut_ptr().cast(), 8, &raw mut read), kResultOk);
        }
        assert_eq!(written, 5);
        assert_eq!(read, 5);
        assert_eq!(read_back.get(..5), Some(b"state".as_slice()));
        assert_eq!(stream.data(), b"state");
    }

    #[test]
    fn seeking_past_the_end_and_writing_fills_with_zeros() {
        let stream = MemoryStream::default();
        let mut payload = [7_u8];
        let mut position = 0;
        // SAFETY: every buffer is live and at least as large as the sizes passed.
        unsafe {
            assert_eq!(
                stream.seek(3, to_i32(IStreamSeekMode_::kIBSeekEnd), &raw mut position),
                kResultOk
            );
            assert_eq!(
                stream.write(payload.as_mut_ptr().cast(), 1, std::ptr::null_mut()),
                kResultOk
            );
            assert_eq!(stream.tell(&raw mut position), kResultOk);
        }
        assert_eq!(position, 4);
        assert_eq!(stream.data(), [0, 0, 0, 7]);
    }

    #[test]
    fn rejects_negative_positions_and_bad_modes() {
        let stream = MemoryStream::default();
        // SAFETY: no result slots are passed.
        unsafe {
            assert_eq!(
                stream.seek(-1, to_i32(IStreamSeekMode_::kIBSeekSet), std::ptr::null_mut()),
                kResultFalse
            );
            assert_eq!(stream.seek(0, 9, std::ptr::null_mut()), kInvalidArgument);
        }
    }

    #[test]
    fn size_can_be_read_and_changed() {
        let stream = MemoryStream::with_data(vec![1, 2, 3]);
        let mut size = 0;
        // SAFETY: the result slot is live.
        unsafe {
            assert_eq!(stream.getStreamSize(&raw mut size), kResultOk);
            assert_eq!(size, 3);
            assert_eq!(stream.setStreamSize(1), kResultOk);
        }
        assert_eq!(stream.data(), [1]);
    }
}
