//! The edit controller of the fake plugins.
//!
//! It mirrors the gain from the component state and keeps a small state blob of its own.
//! Setting `EDIT_TRIGGER_PARAMETER` makes it report a gain edit to the component handler,
//! the way an editor reports a change the user made.

#![expect(
    unsafe_code,
    reason = "implements the VST3 edit controller COM interfaces of the fake plugins"
)]

use std::ffi::{CStr, c_char};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, PoisonError};

use vst3::Steinberg::Vst::{
    IComponentHandler, IComponentHandlerTrait as _, IConnectionPoint, IConnectionPointTrait, IEditController,
    IEditControllerTrait, IMessage, IMessageTrait as _, ParamID, ParamValue, ParameterInfo,
    ParameterInfo_::ParameterFlags_, String128, TChar,
};
use vst3::Steinberg::{
    FUnknown, IBStream, IBStreamTrait as _, IPlugView, IPluginBaseTrait, kInvalidArgument, kResultFalse, kResultOk,
    tresult,
};
use vst3::{Class, ComPtr, ComRef};
use vst3_host::strings::to_utf16;

use crate::processor::CONNECTED_MESSAGE;
use crate::{EDIT_TRIGGER_PARAMETER, GAIN_PARAMETER, INITIAL_CONTROLLER_STATE, record};

/// The edit controller of a fake plugin.
pub struct TestController {
    /// Whether calls are logged.
    recording: bool,
    /// The gain the controller knows, as `f64` bits.
    gain: AtomicU64,
    /// The controller's own state.
    state: Mutex<Vec<u8>>,
    /// The component handler from the host.
    handler: Mutex<Option<ComPtr<IComponentHandler>>>,
}

impl Class for TestController {
    type Interfaces = (IEditController, IConnectionPoint);
}

impl TestController {
    /// Creates a controller, which logs its calls when `recording` is true.
    pub fn new(recording: bool) -> Self {
        Self {
            recording,
            gain: AtomicU64::new(1.0_f64.to_bits()),
            state: Mutex::new(INITIAL_CONTROLLER_STATE.to_vec()),
            handler: Mutex::new(None),
        }
    }

    /// Logs a call when this controller records.
    fn record(&self, name: &'static str) {
        record(self.recording, name);
    }

    /// Reports a gain edit to the component handler.
    fn perform_gain_edit(&self, value: f64) {
        let handler = self.handler.lock().unwrap_or_else(PoisonError::into_inner).clone();
        if let Some(handler) = handler {
            // SAFETY: the handler is valid while the host holds the controller.
            unsafe {
                handler.beginEdit(GAIN_PARAMETER);
                handler.performEdit(GAIN_PARAMETER, value);
                handler.endEdit(GAIN_PARAMETER);
            }
        }
    }
}

impl IPluginBaseTrait for TestController {
    unsafe fn initialize(&self, _context: *mut FUnknown) -> tresult {
        self.record("controller.initialize");
        kResultOk
    }

    unsafe fn terminate(&self) -> tresult {
        self.record("controller.terminate");
        *self.handler.lock().unwrap_or_else(PoisonError::into_inner) = None;
        kResultOk
    }
}

impl IEditControllerTrait for TestController {
    unsafe fn setComponentState(&self, state: *mut IBStream) -> tresult {
        self.record("controller.setComponentState");
        // SAFETY: the host passes a valid stream or null.
        let Some(stream) = (unsafe { ComRef::from_raw(state) }) else {
            return kInvalidArgument;
        };
        let mut bytes = [0_u8; 8];
        let mut read = 0;
        // SAFETY: the stream is valid and the buffer holds the eight bytes requested.
        let result = unsafe { stream.read(bytes.as_mut_ptr().cast(), 8, &raw mut read) };
        if result != kResultOk || read != 8 {
            return kResultFalse;
        }
        self.gain.store(f64::from_le_bytes(bytes).to_bits(), Ordering::Relaxed);
        kResultOk
    }

    unsafe fn setState(&self, state: *mut IBStream) -> tresult {
        self.record("controller.setState");
        // SAFETY: the host passes a valid stream or null.
        let Some(stream) = (unsafe { ComRef::from_raw(state) }) else {
            return kInvalidArgument;
        };
        let mut bytes = [0_u8; 256];
        let mut read = 0;
        // SAFETY: the stream is valid and the buffer holds the bytes requested.
        let result = unsafe { stream.read(bytes.as_mut_ptr().cast(), 256, &raw mut read) };
        if result != kResultOk {
            return kResultFalse;
        }
        let length = usize::try_from(read).unwrap_or_default();
        *self.state.lock().unwrap_or_else(PoisonError::into_inner) = bytes.get(..length).unwrap_or_default().to_vec();
        kResultOk
    }

    unsafe fn getState(&self, state: *mut IBStream) -> tresult {
        self.record("controller.getState");
        // SAFETY: the host passes a valid stream or null.
        let Some(stream) = (unsafe { ComRef::from_raw(state) }) else {
            return kInvalidArgument;
        };
        let mut bytes = self.state.lock().unwrap_or_else(PoisonError::into_inner).clone();
        let length = i32::try_from(bytes.len()).unwrap_or_default();
        // SAFETY: the stream is valid and the buffer holds the bytes written.
        unsafe { stream.write(bytes.as_mut_ptr().cast(), length, std::ptr::null_mut()) }
    }

    unsafe fn getParameterCount(&self) -> i32 {
        1
    }

    unsafe fn getParameterInfo(&self, index: i32, info: *mut ParameterInfo) -> tresult {
        if index != 0 || info.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host passes a writable ParameterInfo, checked for null above.
        let info = unsafe { &mut *info };
        info.id = GAIN_PARAMETER;
        to_utf16("Gain", &mut info.title);
        to_utf16("Gain", &mut info.shortTitle);
        to_utf16("", &mut info.units);
        info.stepCount = 0;
        info.defaultNormalizedValue = 1.0;
        info.unitId = 0;
        info.flags = i32::try_from(i64::from(ParameterFlags_::kCanAutomate)).unwrap_or_default();
        kResultOk
    }

    unsafe fn getParamStringByValue(&self, _id: ParamID, value: ParamValue, string: *mut String128) -> tresult {
        if string.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host passes a writable String128, checked for null above.
        to_utf16(&format!("{value:.2}"), unsafe { &mut *string });
        kResultOk
    }

    unsafe fn getParamValueByString(&self, _id: ParamID, _string: *mut TChar, _value: *mut ParamValue) -> tresult {
        kResultFalse
    }

    unsafe fn normalizedParamToPlain(&self, _id: ParamID, value: ParamValue) -> ParamValue {
        value
    }

    unsafe fn plainParamToNormalized(&self, _id: ParamID, value: ParamValue) -> ParamValue {
        value
    }

    unsafe fn getParamNormalized(&self, id: ParamID) -> ParamValue {
        if id == GAIN_PARAMETER {
            f64::from_bits(self.gain.load(Ordering::Relaxed))
        } else {
            0.0
        }
    }

    unsafe fn setParamNormalized(&self, id: ParamID, value: ParamValue) -> tresult {
        match id {
            GAIN_PARAMETER => {
                self.gain.store(value.to_bits(), Ordering::Relaxed);
                kResultOk
            }
            EDIT_TRIGGER_PARAMETER => {
                self.gain.store(value.to_bits(), Ordering::Relaxed);
                self.perform_gain_edit(value);
                kResultOk
            }
            _ => kInvalidArgument,
        }
    }

    unsafe fn setComponentHandler(&self, handler: *mut IComponentHandler) -> tresult {
        self.record("controller.setComponentHandler");
        // SAFETY: the host passes a valid handler or null.
        let handler = unsafe { ComRef::from_raw(handler) }.map(|handler| handler.to_com_ptr());
        *self.handler.lock().unwrap_or_else(PoisonError::into_inner) = handler;
        kResultOk
    }

    unsafe fn createView(&self, _name: *const c_char) -> *mut IPlugView {
        std::ptr::null_mut()
    }
}

impl IConnectionPointTrait for TestController {
    unsafe fn connect(&self, _other: *mut IConnectionPoint) -> tresult {
        self.record("controller.connect");
        kResultOk
    }

    unsafe fn disconnect(&self, _other: *mut IConnectionPoint) -> tresult {
        self.record("controller.disconnect");
        kResultOk
    }

    unsafe fn notify(&self, message: *mut IMessage) -> tresult {
        // SAFETY: the sender passes a valid message or null.
        let Some(message) = (unsafe { ComRef::from_raw(message) }) else {
            return kInvalidArgument;
        };
        // SAFETY: the message is valid, and its id is a null-terminated string or null.
        let id = unsafe { message.getMessageID() };
        // SAFETY: a non-null id is a null-terminated string that lives as long as the message.
        if !id.is_null() && unsafe { CStr::from_ptr(id) } == CONNECTED_MESSAGE {
            self.record("controller.notify");
        }
        kResultOk
    }
}
