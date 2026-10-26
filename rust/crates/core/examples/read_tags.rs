//! Prints the display tags and the Serato data of audio files, for checking the tag reader by hand.
//!
//! Usage: `cargo run -p audiobatch-core --example read_tags -- <files...>`

use std::path::PathBuf;

use audiobatch_core::read_tags;

fn main() {
    for path in std::env::args().skip(1).map(PathBuf::from) {
        println!("{}", path.display());
        match read_tags(&path) {
            Ok(tags) => {
                println!(
                    "  title: {:?}, artist: {:?}, album: {:?}",
                    tags.title, tags.artist, tags.album
                );
                println!(
                    "  genre: {:?}, year: {:?}, bpm: {:?}, key: {:?}",
                    tags.genre, tags.year, tags.bpm, tags.key
                );
                println!("  picture: {}", tags.has_picture);
                if let Some(serato) = tags.serato {
                    print!("{serato}");
                }
            }
            Err(error) => println!("  error: {error:#}"),
        }
    }
}
