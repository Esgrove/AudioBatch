//! Miri-compatible raw callback aliasing, lifecycle admission, and detached concurrent state contracts.

#![expect(
    unsafe_code,
    reason = "exercises raw VST3 calls with live bounded sample and metadata allocations"
)]

use std::sync::Arc;
use std::sync::atomic::Ordering;

use image_control_dsp::Imager;
use vst3::Steinberg::Vst::{
    AudioBusBuffers, AudioBusBuffers__type0, IAudioProcessorTrait as _, IComponentTrait as _, ProcessData,
    ProcessModes_, ProcessSetup, SpeakerArr, SymbolicSampleSizes_,
};
use vst3::Steinberg::{kInvalidArgument, kResultFalse, kResultOk};

use super::super::audio::AudioState;
use super::super::enums::to_i32;
use super::Component;
use crate::params::GAIN_PARAMETER;

/// Builds fully allocated zero-initialized scalar callback metadata without invoking native DSP preparation.
fn process_data(bus: *mut AudioBusBuffers, frames: i32) -> ProcessData {
    // SAFETY: every field is an integer, float, raw pointer, or nullable C handle, for which zero is valid.
    let mut data: ProcessData = unsafe { std::mem::zeroed() };
    data.processMode = to_i32(ProcessModes_::kRealtime);
    data.symbolicSampleSize = to_i32(SymbolicSampleSizes_::kSample32);
    data.numSamples = frames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = bus;
    data.outputs = bus;
    data
}

#[test]
#[cfg_attr(
    miri,
    ignore = "full FIR preparation and native worker creation are verified natively"
)]
fn mono_arrangement_after_setup_updates_the_prepared_audio_layout() {
    let component = Component::new();
    let mut setup = ProcessSetup {
        processMode: to_i32(ProcessModes_::kOffline),
        symbolicSampleSize: to_i32(SymbolicSampleSizes_::kSample32),
        maxSamplesPerBlock: 256,
        sampleRate: 48_000.0,
    };
    // JUCE hosts may prepare before publishing their final speaker arrangements.
    // SAFETY: setup and both arrangement slots remain live for the complete lifecycle calls.
    unsafe {
        assert_eq!(component.setupProcessing(&raw mut setup), kResultOk);
        let mut input = SpeakerArr::kMono;
        let mut output = SpeakerArr::kMono;
        assert_eq!(
            component.setBusArrangements(&raw mut input, 1, &raw mut output, 1),
            kResultOk
        );
        let pan = crate::params::band_parameter_id(0, 4).expect("Pan");
        for band in 0..4 {
            component.parameters.set(pan + band * 6, 1.0);
        }
        assert_eq!(component.setActive(1), kResultOk);
        assert_eq!(component.setProcessing(1), kResultOk);
        let mut samples = [0.0_f32; 256];
        let mut channels = [samples.as_mut_ptr()];
        let mut bus = AudioBusBuffers {
            numChannels: 1,
            silenceFlags: 0,
            __field0: AudioBusBuffers__type0 {
                channelBuffers32: channels.as_mut_ptr(),
            },
        };
        for block in 0..20 {
            samples.fill(0.0);
            if block == 0 {
                *samples.first_mut().expect("first sample") = 0.75;
            }
            let mut data = process_data(&raw mut bus, 256);
            data.processMode = to_i32(ProcessModes_::kOffline);
            assert_eq!(component.process(&raw mut data), kResultOk);
            for (offset, sample) in samples.iter().enumerate() {
                let expected: f32 = if block * 256 + offset == 4352 { 0.75 } else { 0.0 };
                assert_eq!(sample.to_bits(), expected.to_bits());
            }
        }
        assert_eq!(component.setProcessing(0), kResultOk);
        assert_eq!(component.setActive(0), kResultOk);
    }
}

#[test]
fn identical_input_output_bus_and_sample_arrays_are_alias_safe() {
    let component = Component::new();
    // This non-prepared processor's direct neutral fallback exercises the real ABI sample access cheaply under Miri.
    component.maximum_frames.store(4, Ordering::Relaxed);
    component.active.store(true, Ordering::Relaxed);
    let mut left = [0.25_f32, -0.0, -0.75, 1.0];
    let mut right = [-0.5_f32, 0.0, 0.125, -1.0];
    let expected_left = left.map(f32::to_bits);
    let expected_right = right.map(f32::to_bits);
    let mut channels = [left.as_mut_ptr(), right.as_mut_ptr()];
    let mut bus = AudioBusBuffers {
        numChannels: 2,
        silenceFlags: 0,
        __field0: AudioBusBuffers__type0 {
            channelBuffers32: channels.as_mut_ptr(),
        },
    };
    let mut data = process_data(&raw mut bus, 4);
    // SAFETY: both bus pointers and channel buffers deliberately alias exactly as legal VST3 in-place buffers do.
    // All metadata and sample storage remain allocated and exclusively available for this callback.
    assert_eq!(unsafe { component.process(&raw mut data) }, kResultOk);
    assert_eq!(left.map(f32::to_bits), expected_left);
    assert_eq!(right.map(f32::to_bits), expected_right);
}

#[test]
fn silent_null_input_channels_do_not_get_dereferenced() {
    let component = Component::new();
    component.maximum_frames.store(4, Ordering::Relaxed);
    component.active.store(true, Ordering::Relaxed);
    let mut left = [1.0_f32; 4];
    let mut right = [1.0_f32; 4];
    let mut inputs = [std::ptr::null_mut(); 2];
    let mut outputs = [left.as_mut_ptr(), right.as_mut_ptr()];
    let mut input = AudioBusBuffers {
        numChannels: 2,
        silenceFlags: 3,
        __field0: AudioBusBuffers__type0 {
            channelBuffers32: inputs.as_mut_ptr(),
        },
    };
    let mut output = AudioBusBuffers {
        numChannels: 2,
        silenceFlags: 3,
        __field0: AudioBusBuffers__type0 {
            channelBuffers32: outputs.as_mut_ptr(),
        },
    };
    let mut data = process_data(&raw mut input, 4);
    data.outputs = &raw mut output;
    // SAFETY: silent flags permit null input channel samples, and writable output arrays cover the whole block.
    assert_eq!(unsafe { component.process(&raw mut data) }, kResultOk);
    assert!(left.into_iter().chain(right).all(|sample| sample.abs() < f32::EPSILON));
    assert_eq!(output.silenceFlags, 0);
    input.silenceFlags = 0;
    let mut data = process_data(&raw mut input, 4);
    data.outputs = &raw mut output;
    // SAFETY: the arrays remain live, and invalid non-silent null samples must be rejected before access.
    assert_eq!(unsafe { component.process(&raw mut data) }, kInvalidArgument);
}

#[test]
fn overlapping_lifecycle_reset_and_process_are_rejected_without_waiting() {
    let component = Component::new();
    component.maximum_frames.store(1, Ordering::Relaxed);
    component.active.store(true, Ordering::Relaxed);
    let mut samples = [0.25_f32; 1];
    let mut channels = [samples.as_mut_ptr(); 2];
    let mut bus = AudioBusBuffers {
        numChannels: 2,
        silenceFlags: 0,
        __field0: AudioBusBuffers__type0 {
            channelBuffers32: channels.as_mut_ptr(),
        },
    };
    let mut data = process_data(&raw mut bus, 1);
    component
        .audio
        .try_with(|_| {
            // SAFETY: all process storage is valid, but admission must reject access before any mutable DSP alias occurs.
            unsafe {
                assert_eq!(component.process(&raw mut data), kResultFalse);
                assert_eq!(component.setProcessing(1), kResultFalse);
                assert_eq!(component.setProcessing(0), kResultFalse);
            }
        })
        .expect("outer admission");
    // SAFETY: no outer admission is held, so reset can obtain exclusive DSP ownership.
    assert_eq!(unsafe { component.setProcessing(1) }, kResultOk);
}

#[test]
fn detached_restore_and_snapshot_overlap_audio_ownership_without_dsp_aliases() {
    let component = Arc::new(Component::new());
    let audio = Arc::clone(&component);
    let worker = std::thread::spawn(move || {
        for _ in 0..8 {
            audio
                .audio
                .try_with(|imager| {
                    audio.parameters.publish(&audio.shared);
                    imager.begin_block();
                    for _ in 0..16 {
                        assert!(imager.process_frame([0.25, -0.25]).into_iter().all(f32::is_finite));
                    }
                })
                .expect("audio owner");
        }
    });
    for _ in 0..8 {
        let mut snapshot = component.parameters.snapshot();
        snapshot.gain = 0.25;
        assert!(component.parameters.restore(snapshot));
        assert!(component.parameters.snapshot().is_valid());
        assert_eq!(
            component.parameters.get(GAIN_PARAMETER).expect("gain").to_bits(),
            0.25_f64.to_bits()
        );
    }
    worker.join().expect("audio thread");
    assert!(component.audio.try_with(Imager::reset).is_some());
}

#[test]
fn admission_releases_after_nested_rejection_and_unwind() {
    let audio = AudioState::new(Imager::new());
    assert_eq!(audio.try_with(|_| audio.try_with(|_| ())), Some(None));
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        audio.try_with(|_| panic!("exercise admission guard unwinding"));
    }));
    assert!(result.is_err());
    assert!(audio.try_with(Imager::reset).is_some());
}
