//! VST3 controller metadata and finite parameter conversion sharing the authoritative UI registry.

#![expect(
    unsafe_code,
    reason = "implements controller COM methods and bounded UTF-16 host string access"
)]

#[cfg(feature = "editor")]
use std::ffi::CStr;
use std::ffi::c_char;
#[cfg(feature = "editor")]
use std::sync::Arc;

use vst3::ComRef;
use vst3::Steinberg::Vst::{
    IComponentHandler, IEditControllerTrait, ParamID, ParamValue, ParameterInfo, ParameterInfo_::ParameterFlags_,
    String128, TChar,
};
use vst3::Steinberg::{IBStream, IPlugView, kInvalidArgument, kResultFalse, kResultOk, tresult};

use super::component::Component;
use super::state;
use crate::params::{BYPASS_PARAMETER, PARAMETER_COUNT, ParameterKind, parameter_spec};
use crate::strings_utf16;

impl IEditControllerTrait for Component {
    unsafe fn setComponentState(&self, stream: *mut IBStream) -> tresult {
        // SAFETY: the host supplies a stream live throughout this non-audio callback, or null.
        if unsafe { state::restore(stream, &self.parameters, &self.shared) }.is_ok() {
            kResultOk
        } else {
            kResultFalse
        }
    }

    unsafe fn setState(&self, _stream: *mut IBStream) -> tresult {
        // Component state is authoritative. No separate controller-only state is required.
        kResultOk
    }

    unsafe fn getState(&self, _stream: *mut IBStream) -> tresult {
        kResultOk
    }

    unsafe fn getParameterCount(&self) -> i32 {
        i32::try_from(PARAMETER_COUNT).unwrap_or_default()
    }

    unsafe fn getParameterInfo(&self, index: i32, info: *mut ParameterInfo) -> tresult {
        let Some(spec) = u32::try_from(index).ok().and_then(parameter_spec) else {
            return kInvalidArgument;
        };
        if info.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host provides a writable ParameterInfo.
        let info = unsafe { &mut *info };
        info.id = spec.id;
        strings_utf16(spec.title, &mut info.title);
        strings_utf16(spec.title, &mut info.shortTitle);
        strings_utf16(spec.units, &mut info.units);
        info.stepCount = i32::from(spec.kind == ParameterKind::Switch);
        info.defaultNormalizedValue = spec.default_normalized;
        info.unitId = 0;
        info.flags = ParameterFlags_::kCanAutomate
            | if spec.id == BYPASS_PARAMETER {
                ParameterFlags_::kIsBypass
            } else {
                0
            };
        kResultOk
    }

    unsafe fn getParamStringByValue(&self, id: ParamID, value: ParamValue, string: *mut String128) -> tresult {
        let Some(spec) = parameter_spec(id).filter(|_| value.is_finite() && !string.is_null()) else {
            return kInvalidArgument;
        };
        // SAFETY: the host provides one writable fixed UTF-16 String128.
        strings_utf16(&spec.format(value), unsafe { &mut *string });
        kResultOk
    }

    unsafe fn getParamValueByString(&self, id: ParamID, string: *mut TChar, value: *mut ParamValue) -> tresult {
        let Some(spec) = parameter_spec(id).filter(|_| !string.is_null() && !value.is_null()) else {
            return kInvalidArgument;
        };
        let mut units = Vec::new();
        for index in 0..128 {
            // SAFETY: the host provides terminated UTF-16 text. Reads stop at its first NUL.
            let unit = unsafe { string.add(index).read() };
            if unit == 0 {
                break;
            }
            units.push(unit);
        }
        let Ok(text) = String::from_utf16(&units) else {
            return kResultFalse;
        };
        let Some(normalized) = spec.parse(&text) else {
            return kResultFalse;
        };
        // SAFETY: the host provides one writable normalized value slot.
        unsafe { value.write(normalized) };
        kResultOk
    }

    unsafe fn normalizedParamToPlain(&self, id: ParamID, value: ParamValue) -> ParamValue {
        parameter_spec(id)
            .filter(|_| value.is_finite())
            .map_or(0.0, |spec| spec.normalized_to_plain(value))
    }

    unsafe fn plainParamToNormalized(&self, id: ParamID, value: ParamValue) -> ParamValue {
        parameter_spec(id)
            .filter(|_| value.is_finite())
            .map_or(0.0, |spec| spec.plain_to_normalized(value))
    }

    unsafe fn getParamNormalized(&self, id: ParamID) -> ParamValue {
        self.parameters.get(id).unwrap_or_default()
    }

    unsafe fn setParamNormalized(&self, id: ParamID, value: ParamValue) -> tresult {
        if self.parameters.set(id, value) {
            kResultOk
        } else {
            kInvalidArgument
        }
    }

    unsafe fn setComponentHandler(&self, handler: *mut IComponentHandler) -> tresult {
        // SAFETY: the host supplies a live handler or null. The retained COM reference owns its lifetime.
        let handler = unsafe { ComRef::from_raw(handler) }.map(|handler| handler.to_com_ptr());
        // Dropping the old handler after unlocking permits its release callback to reenter this controller.
        let old = std::mem::replace(
            &mut *self.handler.lock().unwrap_or_else(std::sync::PoisonError::into_inner),
            handler,
        );
        drop(old);
        kResultOk
    }

    unsafe fn createView(&self, name: *const c_char) -> *mut IPlugView {
        #[cfg(feature = "editor")]
        {
            // SAFETY: a non-null view name is a terminated host string valid throughout this call.
            if !name.is_null() && unsafe { CStr::from_ptr(name) } == c"editor" {
                return super::view::PlugView::create(
                    Arc::clone(&self.parameters),
                    Arc::clone(&self.shared),
                    Arc::clone(&self.handler),
                );
            }
        }
        let _ = name;
        std::ptr::null_mut()
    }
}
