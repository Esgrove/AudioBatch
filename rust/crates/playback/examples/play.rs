//! Plays an audio file through an output device, for checking the engine by ear.
//!
//! Usage: `cargo run -p audiobatch-playback --example play -- <file> [seconds] [sample rate] [buffer size]`
//! Plays from the start for the given number of seconds, then seeks to the middle and plays briefly again.
//! The optional sample rate in hertz and buffer size in frames open the default device with that configuration,
//! after listing what the device supports.

use std::path::PathBuf;
use std::thread::sleep;
use std::time::Duration;

use anyhow::{Context, Result};
use audiobatch_playback::{OutputConfig, Player, device_capabilities};

/// Plays the file given as the first argument, optionally on a device configuration from the other arguments.
fn main() -> Result<()> {
    let mut arguments = std::env::args().skip(1);
    let path = PathBuf::from(arguments.next().context("Give an audio file to play")?);
    let seconds: f64 = arguments.next().map_or(Ok(3.0), |value| value.parse())?;
    let sample_rate = arguments.next().map(|value| value.parse::<u32>()).transpose()?;
    let buffer_size = arguments.next().map(|value| value.parse::<u32>()).transpose()?;

    println!("Capabilities: {:?}", device_capabilities(None)?);
    let mut player = Player::with_config(&OutputConfig {
        device: None,
        sample_rate,
        buffer_size,
    })?;
    println!(
        "Output: {} at {} Hz, buffer {:?}",
        player.device_name(),
        player.output_rate(),
        player.buffer_size()
    );
    player.load(&path, 0.0, true);
    for _ in 0..(seconds * 10.0) as u32 {
        sleep(Duration::from_millis(100));
        for event in player.poll_events() {
            println!("{event:?}");
        }
        println!("position {:.2} s", player.position_seconds());
    }
    if let Some(duration) = player.duration_seconds() {
        player.seek(duration / 2.0);
        println!("Seeked to {:.2} s", duration / 2.0);
        sleep(Duration::from_millis(500));
        println!("position {:.2} s", player.position_seconds());
    }
    println!("Underruns: {}", player.underrun_count());
    Ok(())
}
