//! Atomic replacement of small data files, such as the settings and the plugin lists.
//!
//! The contents go to a temporary file in the same directory,
//! which is flushed to disk and then renamed over the target,
//! so a reader, or the next launch after a crash, never sees a partly written file.

use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};

/// A failure to prepare, write, or commit an atomic file replacement.
#[derive(Debug, thiserror::Error)]
pub enum AtomicWriteError {
    /// The destination directory could not be created.
    #[error("Could not create {}", path.display())]
    CreateDirectory {
        /// Directory to create.
        path: PathBuf,
        /// Filesystem failure.
        #[source]
        source: std::io::Error,
    },
    /// A temporary file could not be created beside the destination.
    #[error("Could not create a temporary file in {}", directory.display())]
    CreateTemporary {
        /// Directory for the temporary file.
        directory: PathBuf,
        /// Filesystem failure.
        #[source]
        source: std::io::Error,
    },
    /// Writing or flushing the temporary file failed.
    #[error(transparent)]
    Write(#[from] std::io::Error),
    /// Committing the replacement failed.
    #[error("Could not replace {}", path.display())]
    Replace {
        /// Destination to replace.
        path: PathBuf,
        /// Persistence failure, retaining the temporary file.
        #[source]
        source: tempfile::PersistError,
    },
}

/// Writes the contents to a file, replacing it atomically and creating its directory when needed.
///
/// # Errors
///
/// Returns an error when the directory, the temporary file, or the replacement cannot be written.
pub fn write_atomically(path: &Path, contents: &[u8]) -> Result<(), AtomicWriteError> {
    let directory = path.parent().unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(directory).map_err(|source| AtomicWriteError::CreateDirectory {
        path: directory.to_path_buf(),
        source,
    })?;
    let mut temporary =
        tempfile::NamedTempFile::new_in(directory).map_err(|source| AtomicWriteError::CreateTemporary {
            directory: directory.to_path_buf(),
            source,
        })?;
    temporary.write_all(contents)?;
    temporary.as_file().sync_all()?;
    temporary.persist(path).map_err(|source| AtomicWriteError::Replace {
        path: path.to_path_buf(),
        source,
    })?;
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
        let error = write_atomically(&target, b"contents").expect_err("directory cannot be replaced");
        assert!(matches!(error, AtomicWriteError::Replace { .. }));
        assert_eq!(error.to_string(), format!("Could not replace {}", target.display()));
        assert!(std::error::Error::source(&error).is_some());
        assert_eq!(fs::read(target.join("child")).expect("read"), b"keep");
    }
}
