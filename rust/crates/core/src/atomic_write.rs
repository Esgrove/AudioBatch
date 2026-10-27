//! Atomic replacement of small data files, such as the settings and the plugin lists.
//!
//! The contents go to a temporary file in the same directory,
//! which is flushed to disk and then renamed over the target,
//! so a reader, or the next launch after a crash, never sees a partly written file.

use std::fs;
use std::io::Write as _;
use std::path::Path;

use anyhow::{Context as _, Result};

/// Writes the contents to a file, replacing it atomically and creating its directory when needed.
///
/// # Errors
///
/// Returns an error when the directory, the temporary file, or the replacement cannot be written.
pub fn write_atomically(path: &Path, contents: &[u8]) -> Result<()> {
    let directory = path.parent().unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(directory).with_context(|| format!("Could not create {}", directory.display()))?;
    let mut temporary = tempfile::NamedTempFile::new_in(directory)
        .with_context(|| format!("Could not create a temporary file in {}", directory.display()))?;
    temporary.write_all(contents)?;
    temporary.as_file().sync_all()?;
    temporary
        .persist(path)
        .with_context(|| format!("Could not replace {}", path.display()))?;
    Ok(())
}

#[cfg(test)]
mod test_write_atomically {
    use super::*;

    #[test]
    fn creates_the_directory_and_the_file() {
        let directory = tempfile::tempdir().expect("temp dir");
        let path = directory.path().join("nested").join("settings.json");
        write_atomically(&path, b"first").expect("written");
        assert_eq!(fs::read(&path).expect("read"), b"first");
    }

    #[test]
    fn replaces_an_existing_file_without_leaving_temporary_files() {
        let directory = tempfile::tempdir().expect("temp dir");
        let path = directory.path().join("settings.json");
        write_atomically(&path, b"first").expect("written");
        write_atomically(&path, b"second").expect("replaced");
        assert_eq!(fs::read(&path).expect("read"), b"second");
        let entries = fs::read_dir(directory.path()).expect("listed").count();
        assert_eq!(entries, 1);
    }

    #[test]
    fn fails_when_the_target_is_a_directory() {
        let directory = tempfile::tempdir().expect("temp dir");
        let target = directory.path().join("occupied");
        fs::create_dir(&target).expect("created");
        fs::write(target.join("child"), b"keep").expect("written");
        assert!(write_atomically(&target, b"contents").is_err());
        assert_eq!(fs::read(target.join("child")).expect("read"), b"keep");
    }
}
