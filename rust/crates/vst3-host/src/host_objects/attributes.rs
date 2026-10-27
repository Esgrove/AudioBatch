//! Messages and attribute lists that plugins create through the host to talk between their parts.
//!
//! A plugin's processor and controller exchange `IMessage` objects over their connection points.
//! The host only creates and stores them, it never reads their contents.

#![expect(unsafe_code, reason = "implements the IMessage and IAttributeList COM interfaces")]

use std::collections::HashMap;
use std::ffi::{CStr, CString, c_char, c_void};
use std::sync::{Mutex, PoisonError};

use vst3::Steinberg::Vst::{
    IAttributeList, IAttributeList_::AttrID, IAttributeListTrait, IMessage, IMessageTrait, TChar,
};
use vst3::Steinberg::{FIDString, kInvalidArgument, kResultFalse, kResultOk, tresult};
use vst3::{Class, ComWrapper};

/// One stored attribute value.
#[derive(Debug, Clone)]
enum AttributeValue {
    /// A whole number.
    Integer(i64),
    /// A floating point number.
    Float(f64),
    /// UTF-16 text, with its terminating null.
    Text(Vec<TChar>),
    /// Raw bytes.
    Binary(Vec<u8>),
}

/// An attribute list.
#[derive(Default)]
pub struct HostAttributeList {
    /// The values by attribute id.
    values: Mutex<HashMap<String, AttributeValue>>,
}

/// A message with an id and an attribute list.
pub struct HostMessage {
    /// The message id.
    id: Mutex<CString>,
    /// The attributes of the message.
    attributes: ComWrapper<HostAttributeList>,
}

impl Class for HostAttributeList {
    type Interfaces = (IAttributeList,);
}

impl HostAttributeList {
    /// Stores a value under an attribute id.
    fn set(&self, id: AttrID, value: AttributeValue) -> tresult {
        let Some(key) = attribute_key(id) else {
            return kInvalidArgument;
        };
        self.values
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .insert(key, value);
        kResultOk
    }

    /// Runs `read` on the value stored under an attribute id.
    fn with_value(&self, id: AttrID, read: impl FnOnce(&AttributeValue) -> tresult) -> tresult {
        let Some(key) = attribute_key(id) else {
            return kInvalidArgument;
        };
        let values = self.values.lock().unwrap_or_else(PoisonError::into_inner);
        values.get(&key).map_or(kResultFalse, read)
    }
}

impl IAttributeListTrait for HostAttributeList {
    unsafe fn setInt(&self, id: AttrID, value: i64) -> tresult {
        self.set(id, AttributeValue::Integer(value))
    }

    unsafe fn getInt(&self, id: AttrID, value: *mut i64) -> tresult {
        self.with_value(id, |stored| match (stored, value.is_null()) {
            (AttributeValue::Integer(number), false) => {
                // SAFETY: the plugin passes a writable value slot, checked for null.
                unsafe { *value = *number };
                kResultOk
            }
            _ => kResultFalse,
        })
    }

    unsafe fn setFloat(&self, id: AttrID, value: f64) -> tresult {
        self.set(id, AttributeValue::Float(value))
    }

    unsafe fn getFloat(&self, id: AttrID, value: *mut f64) -> tresult {
        self.with_value(id, |stored| match (stored, value.is_null()) {
            (AttributeValue::Float(number), false) => {
                // SAFETY: the plugin passes a writable value slot, checked for null.
                unsafe { *value = *number };
                kResultOk
            }
            _ => kResultFalse,
        })
    }

    unsafe fn setString(&self, id: AttrID, string: *const TChar) -> tresult {
        if string.is_null() {
            return kInvalidArgument;
        }
        let mut text = Vec::new();
        // SAFETY: the plugin passes a null-terminated UTF-16 string, read up to and including its null.
        unsafe {
            let mut cursor = string;
            loop {
                let unit = *cursor;
                text.push(unit);
                if unit == 0 {
                    break;
                }
                cursor = cursor.add(1);
            }
        }
        self.set(id, AttributeValue::Text(text))
    }

    unsafe fn getString(&self, id: AttrID, string: *mut TChar, size_in_bytes: u32) -> tresult {
        if string.is_null() {
            return kInvalidArgument;
        }
        let capacity = size_in_bytes as usize / size_of::<TChar>();
        self.with_value(id, |stored| match stored {
            AttributeValue::Text(text) if capacity > 0 => {
                let count = text.len().min(capacity);
                // SAFETY: the plugin passes a buffer of `size_in_bytes` bytes, and at most that many are written,
                // with the last written unit set to null.
                unsafe {
                    std::ptr::copy_nonoverlapping(text.as_ptr(), string, count);
                    *string.add(count - 1) = 0;
                }
                kResultOk
            }
            _ => kResultFalse,
        })
    }

    unsafe fn setBinary(&self, id: AttrID, data: *const c_void, size_in_bytes: u32) -> tresult {
        let bytes = if data.is_null() || size_in_bytes == 0 {
            Vec::new()
        } else {
            // SAFETY: the plugin passes `size_in_bytes` readable bytes at `data`, checked for null above.
            unsafe { std::slice::from_raw_parts(data.cast::<u8>(), size_in_bytes as usize) }.to_vec()
        };
        self.set(id, AttributeValue::Binary(bytes))
    }

    unsafe fn getBinary(&self, id: AttrID, data: *mut *const c_void, size_in_bytes: *mut u32) -> tresult {
        if data.is_null() || size_in_bytes.is_null() {
            return kInvalidArgument;
        }
        self.with_value(id, |stored| match stored {
            AttributeValue::Binary(bytes) => {
                // SAFETY: both result slots are writable, checked for null above.
                // The returned pointer stays valid until the attribute is replaced or the list is released,
                // as the VST3 API documents.
                unsafe {
                    *data = bytes.as_ptr().cast();
                    *size_in_bytes = u32::try_from(bytes.len()).unwrap_or(u32::MAX);
                }
                kResultOk
            }
            _ => kResultFalse,
        })
    }
}

impl Class for HostMessage {
    type Interfaces = (IMessage,);
}

impl Default for HostMessage {
    fn default() -> Self {
        Self {
            id: Mutex::new(CString::default()),
            attributes: ComWrapper::new(HostAttributeList::default()),
        }
    }
}

impl IMessageTrait for HostMessage {
    unsafe fn getMessageID(&self) -> FIDString {
        // The pointer stays valid until the id is replaced, as the VST3 API documents.
        self.id.lock().unwrap_or_else(PoisonError::into_inner).as_ptr()
    }

    unsafe fn setMessageID(&self, id: FIDString) {
        let text = if id.is_null() {
            CString::default()
        } else {
            // SAFETY: the plugin passes a null-terminated string, checked for null above.
            unsafe { CStr::from_ptr(id) }.to_owned()
        };
        *self.id.lock().unwrap_or_else(PoisonError::into_inner) = text;
    }

    unsafe fn getAttributes(&self) -> *mut IAttributeList {
        self.attributes
            .as_com_ref::<IAttributeList>()
            .map_or(std::ptr::null_mut(), |attributes| attributes.as_ptr())
    }
}

/// Returns an attribute id as a map key.
fn attribute_key(id: *const c_char) -> Option<String> {
    if id.is_null() {
        return None;
    }
    // SAFETY: the plugin passes a null-terminated attribute id, checked for null above.
    Some(unsafe { CStr::from_ptr(id) }.to_string_lossy().into_owned())
}

#[cfg(test)]
mod test_attributes {
    use super::*;

    #[test]
    fn stores_numbers_text_and_bytes() {
        let list = HostAttributeList::default();
        let integer_id = c"count".as_ptr();
        let text_id = c"label".as_ptr();
        let binary_id = c"blob".as_ptr();
        let text: Vec<u16> = "Hi\0".encode_utf16().collect();
        let blob = [1_u8, 2, 3];
        // SAFETY: every pointer refers to live, correctly sized data for the duration of the calls.
        unsafe {
            assert_eq!(list.setInt(integer_id, 7), kResultOk);
            assert_eq!(list.setString(text_id, text.as_ptr()), kResultOk);
            assert_eq!(list.setBinary(binary_id, blob.as_ptr().cast(), 3), kResultOk);
            let mut integer = 0;
            assert_eq!(list.getInt(integer_id, &raw mut integer), kResultOk);
            assert_eq!(integer, 7);
            let mut read_text = [0_u16; 8];
            assert_eq!(list.getString(text_id, read_text.as_mut_ptr(), 16), kResultOk);
            assert_eq!(read_text.get(..3), Some(text.as_slice()));
            let mut data: *const c_void = std::ptr::null();
            let mut size = 0;
            assert_eq!(list.getBinary(binary_id, &raw mut data, &raw mut size), kResultOk);
            assert_eq!(std::slice::from_raw_parts(data.cast::<u8>(), size as usize), blob);
            let mut float = 0.0;
            assert_eq!(list.getFloat(integer_id, &raw mut float), kResultFalse);
        }
    }

    #[test]
    fn message_keeps_its_id() {
        let message = HostMessage::default();
        // SAFETY: the id is a valid null-terminated string, and the returned pointer is read while the message lives.
        unsafe {
            message.setMessageID(c"hello".as_ptr());
            assert_eq!(CStr::from_ptr(message.getMessageID()), c"hello");
            assert!(!message.getAttributes().is_null());
        }
    }

    #[test]
    fn string_is_truncated_to_the_buffer() {
        let list = HostAttributeList::default();
        let text: Vec<u16> = "Hello\0".encode_utf16().collect();
        let mut buffer = [9_u16; 3];
        // SAFETY: both buffers are live and the size matches the target buffer.
        unsafe {
            list.setString(c"id".as_ptr(), text.as_ptr());
            assert_eq!(list.getString(c"id".as_ptr(), buffer.as_mut_ptr(), 6), kResultOk);
        }
        assert_eq!(buffer, [u16::from(b'H'), u16::from(b'e'), 0]);
    }
}
