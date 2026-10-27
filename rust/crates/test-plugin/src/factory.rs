//! The plugin factory of the fake plugins.
//!
//! It lists a component class for every kind and the two controller classes,
//! with the extended class information of `IPluginFactory2`.

use std::ffi::c_void;

use vst3::Steinberg::{
    FIDString, FUnknown, IPluginFactory, IPluginFactory2, IPluginFactory2Trait, IPluginFactoryTrait, PClassInfo,
    PClassInfo_::ClassCardinality_, PClassInfo2, PFactoryInfo, PFactoryInfo_::FactoryFlags_, TUID, kInvalidArgument,
    kResultFalse, kResultOk, tresult,
};
use vst3::{Class, ComWrapper};

use crate::controller::TestController;
use crate::processor::TestProcessor;
use crate::text::copy_char8;
use crate::{CONTROLLER_ID, Kind, RECORDER_CONTROLLER_ID, VENDOR};

/// Category of audio effect components.
const AUDIO_EFFECT_CATEGORY: &str = "Audio Module Class";

/// Category of edit controllers.
const CONTROLLER_CATEGORY: &str = "Component Controller Class";

/// Version every class reports.
const VERSION: &str = "1.0.0";

/// SDK version every class reports.
const SDK_VERSION: &str = "VST 3.8.0";

/// The plugin factory.
pub struct Factory;

/// One class the factory lists.
struct ClassEntry {
    /// Class id.
    id: TUID,
    /// Class category.
    category: &'static str,
    /// Display name.
    name: &'static str,
    /// Subcategories, empty for controllers.
    subcategories: &'static str,
}

impl Class for Factory {
    type Interfaces = (IPluginFactory, IPluginFactory2);
}

impl IPluginFactoryTrait for Factory {
    unsafe fn getFactoryInfo(&self, info: *mut PFactoryInfo) -> tresult {
        if info.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host passes a writable PFactoryInfo, checked for null above.
        let info = unsafe { &mut *info };
        copy_char8(VENDOR, &mut info.vendor);
        copy_char8("https://example.com", &mut info.url);
        copy_char8("", &mut info.email);
        info.flags = i32::try_from(i64::from(FactoryFlags_::kUnicode)).unwrap_or_default();
        kResultOk
    }

    unsafe fn countClasses(&self) -> i32 {
        i32::try_from(class_entries().len()).unwrap_or_default()
    }

    unsafe fn getClassInfo(&self, index: i32, info: *mut PClassInfo) -> tresult {
        let Some(entry) = class_entry(index) else {
            return kInvalidArgument;
        };
        if info.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host passes a writable PClassInfo, checked for null above.
        let info = unsafe { &mut *info };
        info.cid = entry.id;
        info.cardinality = i32::try_from(i64::from(ClassCardinality_::kManyInstances)).unwrap_or_default();
        copy_char8(entry.category, &mut info.category);
        copy_char8(entry.name, &mut info.name);
        kResultOk
    }

    unsafe fn createInstance(&self, cid: FIDString, iid: FIDString, obj: *mut *mut c_void) -> tresult {
        if cid.is_null() || iid.is_null() || obj.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host passes a 16-byte class id, checked for null above.
        let class_id = unsafe { *cid.cast::<TUID>() };
        let unknown = match Kind::from_class_id(&class_id) {
            Some(kind) => ComWrapper::new(TestProcessor::new(kind)).to_com_ptr::<FUnknown>(),
            None if class_id == CONTROLLER_ID => ComWrapper::new(TestController::new(false)).to_com_ptr::<FUnknown>(),
            None if class_id == RECORDER_CONTROLLER_ID => {
                ComWrapper::new(TestController::new(true)).to_com_ptr::<FUnknown>()
            }
            None => None,
        };
        let Some(unknown) = unknown else {
            // SAFETY: the result slot is writable, checked for null above.
            unsafe { *obj = std::ptr::null_mut() };
            return kResultFalse;
        };
        let pointer = unknown.as_ptr();
        // SAFETY: the object is valid, and `queryInterface` writes an owned reference to the result slot.
        unsafe { ((*(*pointer).vtbl).queryInterface)(pointer, iid.cast(), obj) }
    }
}

impl IPluginFactory2Trait for Factory {
    unsafe fn getClassInfo2(&self, index: i32, info: *mut PClassInfo2) -> tresult {
        let Some(entry) = class_entry(index) else {
            return kInvalidArgument;
        };
        if info.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host passes a writable PClassInfo2, checked for null above.
        let info = unsafe { &mut *info };
        info.cid = entry.id;
        info.cardinality = i32::try_from(i64::from(ClassCardinality_::kManyInstances)).unwrap_or_default();
        copy_char8(entry.category, &mut info.category);
        copy_char8(entry.name, &mut info.name);
        info.classFlags = 0;
        copy_char8(entry.subcategories, &mut info.subCategories);
        copy_char8(VENDOR, &mut info.vendor);
        copy_char8(VERSION, &mut info.version);
        copy_char8(SDK_VERSION, &mut info.sdkVersion);
        kResultOk
    }
}

/// Returns every class the factory lists: the components first, then the controllers.
fn class_entries() -> Vec<ClassEntry> {
    let mut entries: Vec<ClassEntry> = Kind::ALL
        .into_iter()
        .map(|kind| ClassEntry {
            id: kind.class_id(),
            category: AUDIO_EFFECT_CATEGORY,
            name: kind.name(),
            subcategories: kind.subcategories(),
        })
        .collect();
    for (id, name) in [
        (CONTROLLER_ID, "Test Controller"),
        (RECORDER_CONTROLLER_ID, "Test Recorder Controller"),
    ] {
        entries.push(ClassEntry {
            id,
            category: CONTROLLER_CATEGORY,
            name,
            subcategories: "",
        });
    }
    entries
}

/// Returns the class at an index.
fn class_entry(index: i32) -> Option<ClassEntry> {
    let index = usize::try_from(index).ok()?;
    class_entries().into_iter().nth(index)
}
