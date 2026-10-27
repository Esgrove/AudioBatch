//! The host context plugins are initialized with.
//!
//! It reports the host name, creates the message and attribute list objects that plugins use to talk
//! between their processor and controller, and tells plugins which optional host interfaces exist.

#![expect(
    unsafe_code,
    reason = "implements the IHostApplication and IPlugInterfaceSupport COM interfaces"
)]

use std::ffi::c_void;

use vst3::Steinberg::Vst::{
    IAttributeList, IAudioProcessor, IComponent, IComponentHandler, IComponentHandler2, IConnectionPoint,
    IEditController, IHostApplication, IHostApplicationTrait, IMessage, IParamValueQueue, IParameterChanges,
    IPlugInterfaceSupport, IPlugInterfaceSupportTrait, String128,
};
use vst3::Steinberg::{
    FUnknown, IBStream, IPlugFrame, IPlugView, IPlugViewContentScaleSupport, TUID, kInvalidArgument, kResultFalse,
    kResultOk, kResultTrue, tresult,
};
use vst3::{Class, ComWrapper, Interface};

use super::attributes::{HostAttributeList, HostMessage};
use crate::strings::to_utf16;

/// Name the host reports to plugins.
const HOST_NAME: &str = "AudioBatch";

/// The host context.
pub struct HostApplication;

impl Class for HostApplication {
    type Interfaces = (IHostApplication, IPlugInterfaceSupport);
}

impl HostApplication {
    /// Creates a host context object.
    pub fn create() -> ComWrapper<Self> {
        ComWrapper::new(Self)
    }
}

impl IHostApplicationTrait for HostApplication {
    unsafe fn getName(&self, name: *mut String128) -> tresult {
        if name.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the plugin passes a writable String128, checked for null above.
        to_utf16(HOST_NAME, unsafe { &mut *name });
        kResultOk
    }

    unsafe fn createInstance(&self, cid: *mut TUID, iid: *mut TUID, obj: *mut *mut c_void) -> tresult {
        if cid.is_null() || iid.is_null() || obj.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the plugin passes valid 16-byte ids and a writable result slot, checked for null above.
        unsafe {
            *obj = std::ptr::null_mut();
            let class_id = tuid_bytes(&*cid);
            let unknown = if class_id == IMessage::IID {
                ComWrapper::new(HostMessage::default()).to_com_ptr::<FUnknown>()
            } else if class_id == IAttributeList::IID {
                ComWrapper::new(HostAttributeList::default()).to_com_ptr::<FUnknown>()
            } else {
                None
            };
            let Some(unknown) = unknown else {
                return kResultFalse;
            };
            let pointer = unknown.as_ptr();
            ((*(*pointer).vtbl).queryInterface)(pointer, iid.cast_const(), obj)
        }
    }
}

impl IPlugInterfaceSupportTrait for HostApplication {
    unsafe fn isPlugInterfaceSupported(&self, iid: *const TUID) -> tresult {
        if iid.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the plugin passes a valid 16-byte id, checked for null above.
        let id = tuid_bytes(unsafe { &*iid });
        let supported = [
            IComponent::IID,
            IAudioProcessor::IID,
            IEditController::IID,
            IConnectionPoint::IID,
            IComponentHandler::IID,
            IComponentHandler2::IID,
            IParameterChanges::IID,
            IParamValueQueue::IID,
            IBStream::IID,
            IPlugView::IID,
            IPlugFrame::IID,
            IPlugViewContentScaleSupport::IID,
        ];
        if supported.contains(&id) {
            kResultTrue
        } else {
            kResultFalse
        }
    }
}

/// Returns the bytes of a TUID for comparing with interface ids.
fn tuid_bytes(tuid: &TUID) -> [u8; 16] {
    tuid.map(|byte| byte as u8)
}
