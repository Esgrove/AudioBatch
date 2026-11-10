//! VST3 factory for the independent single-component `ImageControl` RS identity.

#![expect(unsafe_code, reason = "implements the host-facing VST3 factory ABI")]

use std::ffi::c_void;

use vst3::Steinberg::{
    FIDString, FUnknown, IPluginFactory, IPluginFactory2, IPluginFactory2Trait, IPluginFactoryTrait, PClassInfo,
    PClassInfo2, PFactoryInfo, TUID, kInvalidArgument, kResultFalse, kResultOk, tresult,
};
use vst3::{Class, ComWrapper};

use super::component::Component;
use crate::{COMPONENT_ID, PLUGIN_NAME, PLUGIN_VENDOR, PLUGIN_VERSION, strings_char8};

/// Constructs one class exposing both processor and controller interfaces.
pub struct Factory;

impl Class for Factory {
    type Interfaces = (IPluginFactory, IPluginFactory2);
}

impl IPluginFactoryTrait for Factory {
    unsafe fn getFactoryInfo(&self, info: *mut PFactoryInfo) -> tresult {
        if info.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host provides one writable factory description.
        let info = unsafe { &mut *info };
        strings_char8(PLUGIN_VENDOR, &mut info.vendor);
        strings_char8("https://github.com/Esgrove/AudioBatch", &mut info.url);
        strings_char8("esgrove@outlook.com", &mut info.email);
        info.flags = 0;
        kResultOk
    }

    unsafe fn countClasses(&self) -> i32 {
        1
    }

    unsafe fn getClassInfo(&self, index: i32, info: *mut PClassInfo) -> tresult {
        if index != 0 || info.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host provides one writable class description.
        let info = unsafe { &mut *info };
        info.cid = COMPONENT_ID;
        info.cardinality = i32::MAX;
        strings_char8("Audio Module Class", &mut info.category);
        strings_char8(PLUGIN_NAME, &mut info.name);
        kResultOk
    }

    unsafe fn createInstance(&self, cid: FIDString, iid: FIDString, object: *mut *mut c_void) -> tresult {
        if object.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host provides one writable pointer slot.
        unsafe { object.write(std::ptr::null_mut()) };
        if cid.is_null() || iid.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: class identifiers contain 16 readable bytes, without an alignment requirement.
        if unsafe { cid.cast::<TUID>().read_unaligned() } != COMPONENT_ID {
            return kResultFalse;
        }
        let Some(unknown) = ComWrapper::new(Component::new()).to_com_ptr::<FUnknown>() else {
            return kResultFalse;
        };
        let pointer = unknown.as_ptr();
        // SAFETY: the object is live, iid contains 16 bytes, and queryInterface transfers an owned reference.
        unsafe { ((*(*pointer).vtbl).queryInterface)(pointer, iid.cast(), object) }
    }
}

impl IPluginFactory2Trait for Factory {
    unsafe fn getClassInfo2(&self, index: i32, info: *mut PClassInfo2) -> tresult {
        if index != 0 || info.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host provides one writable extended class description.
        let info = unsafe { &mut *info };
        info.cid = COMPONENT_ID;
        info.cardinality = i32::MAX;
        strings_char8("Audio Module Class", &mut info.category);
        strings_char8(PLUGIN_NAME, &mut info.name);
        info.classFlags = 0;
        strings_char8("Fx|Spatial", &mut info.subCategories);
        strings_char8(PLUGIN_VENDOR, &mut info.vendor);
        strings_char8(PLUGIN_VERSION, &mut info.version);
        strings_char8("VST 3.8.0", &mut info.sdkVersion);
        kResultOk
    }
}
