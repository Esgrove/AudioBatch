//! Focused CLAP contracts exercise the actual configuration, registry, and processing helpers.

use super::*;
use clack_extensions::configurable_audio_ports::AudioPortRequestDetails;

/// One transaction must keep both bus directions matching and reject duplicate/unknown ports.
#[test]
fn port_configuration_is_atomic_and_matching() {
    let mono = [
        AudioPortRequest::new(true, 0, AudioPortRequestDetails::mono()),
        AudioPortRequest::new(false, 0, AudioPortRequestDetails::mono()),
    ];
    let stereo = [
        AudioPortRequest::new(true, 0, AudioPortRequestDetails::stereo()),
        AudioPortRequest::new(false, 0, AudioPortRequestDetails::stereo()),
    ];
    assert_eq!(requested_channels(2, &mono), Some(1));
    assert_eq!(requested_channels(1, &stereo), Some(2));
    assert_eq!(requested_channels(2, &mono[..1]), None);
    assert_eq!(requested_channels(2, &[mono[0], mono[0]]), None);
    assert_eq!(
        requested_channels(2, &[AudioPortRequest::new(true, 1, AudioPortRequestDetails::stereo())]),
        None
    );
    assert_eq!(
        requested_channels(
            2,
            &[AudioPortRequest::new(true, 0, AudioPortRequestDetails::untyped(6))]
        ),
        None
    );
    assert_eq!(requested_channels(2, &[]), Some(2));
}

/// Discovery identity is stable and independent from VST3's COM class identifier.
#[test]
fn descriptor_uses_the_approved_clap_identity() {
    let descriptor = ImageControl::get_descriptor();
    assert_eq!(
        descriptor.id().expect("valid contract setup"),
        c"com.esgrove.imagecontrol-rs-own"
    );
    assert_eq!(descriptor.name().expect("valid contract setup"), c"ImageControl RS");
}

/// Neutral prepared stereo keeps delayed input bits for both out-of-place and in-place buffers.
#[test]
fn prepared_audio_is_bit_exact_in_both_buffer_modes() {
    let frames = 4608;
    let input_left: Vec<_> = (0..frames).map(|index| ((index as f32) * 0.017).sin() * 0.2).collect();
    let input_right: Vec<_> = (0..frames)
        .map(|index| if index % 2 == 0 { -0.0 } else { 0.0 })
        .collect();
    for in_place in [false, true] {
        let instance = Instance::new();
        let prepared = instance
            .prepare(48_000.0, 2, true, frames as u32)
            .expect("valid contract setup");
        assert_eq!(prepared.latency_samples, 4352);
        assert_eq!(prepared.tail_samples, 4096);
        let latency = prepared.latency_samples;
        let mut left = if in_place {
            input_left.clone()
        } else {
            vec![0.0; frames]
        };
        let mut right = if in_place {
            input_right.clone()
        } else {
            vec![0.0; frames]
        };
        let mut left_pair = if in_place {
            ChannelPair::InPlace(&mut left)
        } else {
            ChannelPair::InputOutput(&input_left, &mut left)
        };
        let mut right_pair = if in_place {
            ChannelPair::InPlace(&mut right)
        } else {
            ChannelPair::InputOutput(&input_right, &mut right)
        };
        instance
            .audio
            .try_with(|imager| {
                instance.parameters.publish(&instance.shared);
                imager.begin_block();
                process_channels(imager, &mut left_pair, Some(&mut right_pair), frames);
            })
            .expect("valid contract setup");
        assert!(left[..latency as usize].iter().all(|sample| *sample == 0.0));
        for index in latency as usize..frames {
            assert_eq!(left[index].to_bits(), input_left[index - latency as usize].to_bits());
            assert_eq!(right[index].to_bits(), input_right[index - latency as usize].to_bits());
        }
    }
}
