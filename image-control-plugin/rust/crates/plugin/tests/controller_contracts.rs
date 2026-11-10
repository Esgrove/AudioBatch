//! Actual COM metadata, conversion, validation, and process-context contracts, runnable without the editor.

#![expect(
    unsafe_code,
    reason = "tests valid host-owned arguments at the public VST3 ABI boundary"
)]

use std::ffi::c_void;

use image_control_plugin_rs::{BYPASS_PARAMETER, COMPONENT_ID, PARAMETER_COUNT, PLUGIN_NAME, factory, parameter_spec};
use vst3::Steinberg::Vst::{
    IAudioProcessor, IAudioProcessorTrait as _, IEditController, IEditControllerTrait as _,
    IProcessContextRequirements, IProcessContextRequirementsTrait as _, ParameterInfo, ParameterInfo_::ParameterFlags_,
    SymbolicSampleSizes_,
};
use vst3::Steinberg::{
    IPluginFactory2, IPluginFactory2Trait as _, IPluginFactoryTrait as _, PClassInfo2, kInvalidArgument, kResultFalse,
    kResultOk,
};
use vst3::{ComPtr, Interface};

/// Creates an owned controller through the same factory ABI as a native host.
fn controller() -> ComPtr<IEditController> {
    let factory = factory();
    let mut pointer: *mut c_void = std::ptr::null_mut();
    let iid = IEditController::IID;
    // SAFETY: both ids cover 16 bytes, and pointer is one writable owned-reference output.
    let result = unsafe { factory.createInstance(COMPONENT_ID.as_ptr(), iid.as_ptr().cast(), &raw mut pointer) };
    assert_eq!(result, kResultOk);
    // SAFETY: successful createInstance transfers one owned reference of the requested interface.
    unsafe { ComPtr::from_raw(pointer.cast()) }.expect("controller")
}

/// Decodes a bounded zero-terminated SDK UTF-16 field.
fn text(units: &[u16]) -> String {
    String::from_utf16(&units.iter().copied().take_while(|unit| *unit != 0).collect::<Vec<_>>())
        .expect("UTF-16 metadata")
}

#[test]
fn class_identity_and_all_parameter_metadata_match_registry() {
    let factory = factory();
    let extended = factory.cast::<IPluginFactory2>().expect("extended factory");
    // SAFETY: class description and all parameter outputs have their exact SDK types and sizes.
    unsafe {
        assert_eq!(factory.countClasses(), 1);
        let mut class: PClassInfo2 = std::mem::zeroed();
        assert_eq!(extended.getClassInfo2(0, &raw mut class), kResultOk);
        assert_eq!(class.cid, COMPONENT_ID);
        assert_eq!(
            String::from_utf8(
                class
                    .name
                    .iter()
                    .take_while(|unit| **unit != 0)
                    .map(|unit| unit.to_ne_bytes()[0])
                    .collect()
            )
            .expect("ASCII"),
            PLUGIN_NAME
        );
        let controller = controller();
        assert_eq!(controller.getParameterCount(), 31);
        for id in 0..PARAMETER_COUNT as u32 {
            let spec = parameter_spec(id).expect("registry");
            let mut info: ParameterInfo = std::mem::zeroed();
            assert_eq!(
                controller.getParameterInfo(i32::try_from(id).expect("small id"), &raw mut info),
                kResultOk
            );
            assert_eq!(info.id, id);
            assert_eq!(text(&info.title), spec.title);
            assert_eq!(text(&info.units), spec.units);
            assert_eq!(info.defaultNormalizedValue.to_bits(), spec.default_normalized.to_bits());
            assert_eq!(
                controller.getParamNormalized(id).to_bits(),
                spec.default_normalized.to_bits()
            );
            assert_ne!(info.flags & ParameterFlags_::kCanAutomate, 0);
            assert_eq!(info.flags & ParameterFlags_::kIsBypass != 0, id == BYPASS_PARAMETER);
        }
        assert_eq!(controller.getParameterInfo(31, std::ptr::null_mut()), kInvalidArgument);
        assert_eq!(controller.getParameterInfo(-1, std::ptr::null_mut()), kInvalidArgument);
        assert_eq!(controller.getParameterInfo(0, std::ptr::null_mut()), kInvalidArgument);
    }
}

#[test]
fn process_context_requirements_and_float32_support_are_explicit() {
    let controller = controller();
    let context = controller
        .cast::<IProcessContextRequirements>()
        .expect("mandatory VST3 3.8 interface");
    let processor = controller
        .cast::<IAudioProcessor>()
        .expect("single component processor");
    // SAFETY: all interfaces retain the live component and scalar values are valid SDK discriminants.
    unsafe {
        assert_eq!(context.getProcessContextRequirements(), 0);
        assert_eq!(
            processor.canProcessSampleSize(
                i32::try_from(i64::from(SymbolicSampleSizes_::kSample32)).expect("SDK discriminant")
            ),
            kResultOk
        );
        assert_eq!(
            processor.canProcessSampleSize(
                i32::try_from(i64::from(SymbolicSampleSizes_::kSample64)).expect("SDK discriminant")
            ),
            kResultFalse
        );
    }
}

#[test]
fn conversions_and_non_finite_validation_use_real_controller() {
    let controller = controller();
    // SAFETY: controller stays owned, text buffers have String128 size, and outputs are writable.
    unsafe {
        for id in 0..PARAMETER_COUNT as u32 {
            let spec = parameter_spec(id).expect("spec");
            assert_eq!(controller.setParamNormalized(id, 0.25), kResultOk);
            for invalid in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
                assert_eq!(controller.setParamNormalized(id, invalid), kInvalidArgument);
                assert_eq!(controller.getParamNormalized(id).to_bits(), 0.25_f64.to_bits());
            }
            for normalized in [0.0, spec.default_normalized, 1.0] {
                let mut string = [0_u16; 128];
                let mut value = -1.0;
                assert_eq!(
                    controller.getParamStringByValue(id, normalized, &raw mut string),
                    kResultOk
                );
                assert_eq!(
                    controller.getParamValueByString(id, string.as_mut_ptr(), &raw mut value),
                    kResultOk
                );
                assert!((value - normalized).abs() < 1e-5, "parameter {id}");
            }
        }
        assert_eq!(
            controller.normalizedParamToPlain(0, 0.0).to_bits(),
            (-24.0_f64).to_bits()
        );
        assert_eq!(controller.normalizedParamToPlain(0, 1.0).to_bits(), 12.0_f64.to_bits());
        assert_eq!(controller.setParamNormalized(u32::MAX, 0.5), kInvalidArgument);
        assert_eq!(
            controller.getParamStringByValue(0, f64::NAN, std::ptr::null_mut()),
            kInvalidArgument
        );
        let mut output = -1.0;
        assert_eq!(
            controller.getParamValueByString(0, std::ptr::null_mut(), &raw mut output),
            kInvalidArgument
        );
    }
}

#[test]
fn failed_factory_lookup_clears_output_without_constructing_an_instance() {
    let factory = factory();
    let mut marker = 1_u8;
    let mut output: *mut c_void = (&raw mut marker).cast();
    let unknown = vst3::uid(0, 0, 0, 0);
    let iid = IEditController::IID;
    // SAFETY: both ids contain 16 bytes. Output is writable, and the marker is never dereferenced.
    let result = unsafe { factory.createInstance(unknown.as_ptr(), iid.as_ptr().cast(), &raw mut output) };
    assert_eq!(result, kResultFalse);
    assert!(output.is_null());
}
