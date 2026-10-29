//! Writing processed output next to its source without losing either file on failure.
//!
//! Output is rendered into a hidden temporary file in the destination folder,
//! then renamed into place, which is atomic within one folder.
//! A file being replaced is first renamed to a hidden backup, so a failed commit can put it back.
//! Files that leave the folder go through a `FileDisposer`,
//! which moves them to the system trash unless a caller such as a test passes another one.

use std::fs;
#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use tempfile::NamedTempFile;
#[cfg(target_os = "macos")]
use trash::macos::{DeleteMethod, TrashContextExtMacos};

use crate::decode::{AudioFileDecoder, DecodeError};

#[cfg(windows)]
mod windows_recycle_bin;

/// Highest number tried for a backup name when earlier backups are still around.
const MAXIMUM_BACKUP_INDEX: u32 = 1000;

/// An output destination that cannot be replaced.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum OutputPathError {
    /// The destination is a directory or another non-file.
    #[error("Output path {} is not a file and cannot be replaced.", file_name(.0))]
    NotFile(PathBuf),
    /// Replacing a different file requires explicit permission.
    #[error("Output file {} already exists. Use force overwrite to replace it.", file_name(.0))]
    Exists(PathBuf),
}

/// Written audio that cannot be read back with its expected properties.
#[derive(Debug, thiserror::Error)]
pub enum OutputValidationError {
    /// The new file could not be decoded.
    #[error("the output file cannot be read")]
    Unreadable(#[source] DecodeError),
    /// The channel count differs from the rendered audio.
    #[error("the output has {actual} channels instead of {expected}")]
    Channels {
        /// Channel count read back.
        actual: u32,
        /// Channel count rendered.
        expected: u32,
    },
    /// The sample rate differs from the rendered audio.
    #[error("the output sample rate is {actual} Hz instead of {expected} Hz")]
    SampleRate {
        /// Sample rate read back.
        actual: u32,
        /// Sample rate rendered.
        expected: u32,
    },
    /// The frame count differs from the rendered audio.
    #[error("the output has {actual} samples instead of {expected}")]
    Frames {
        /// Frame count read back.
        actual: u64,
        /// Frame count rendered.
        expected: u64,
    },
}

/// A failure to create or commit a rendered output safely.
#[derive(Debug, thiserror::Error)]
pub enum OutputError {
    /// The destination cannot be replaced.
    #[error(transparent)]
    Path(#[from] OutputPathError),
    /// A temporary file could not be created.
    #[error("Could not create temporary output file")]
    Temporary(#[source] std::io::Error),
    /// No unused backup name is available.
    #[error("Could not move the existing output file aside")]
    BackupNamesExhausted,
    /// The previous output could not be renamed to its backup.
    #[error("Could not move the existing output file aside")]
    Backup(#[source] std::io::Error),
    /// The rendered data could not be synchronized to disk.
    #[error("Could not write the new audio to the disk")]
    Sync(#[source] std::io::Error),
    /// Replacing the source failed without losing the source.
    #[error("Could not replace the original file with the new audio")]
    Replace(#[source] std::io::Error),
    /// Moving a converted file into place failed.
    #[error("Could not move the new audio file into place")]
    Persist(#[source] std::io::Error),
    /// The converted source could not be disposed of.
    #[error("Could not move the original audio file to the system trash")]
    DisposeSource,
    /// The original was lost during replacement, but the rendered file was recovered.
    #[error("Could not replace the original file. The new audio was kept as {}", file_name(.path))]
    Recovered {
        /// Location of the recovered audio.
        path: PathBuf,
        /// The replacement failure that required recovery.
        #[source]
        source: std::io::Error,
    },
    /// The original was lost and the rendered file could not be recovered.
    #[error("Could not replace the original file, and the new audio could not be kept")]
    RecoveryFailed(#[source] std::io::Error),
}

/// Disposes of a file, returning true when the file is gone afterwards, including when it did not exist.
pub type FileDisposer = Arc<dyn Fn(&Path) -> bool + Send + Sync>;

/// Returns the disposer that moves files to the system trash.
#[must_use]
pub fn trash_disposer() -> FileDisposer {
    Arc::new(trash_file)
}

/// Moves a file to the system trash.
/// Returns true when the file is gone afterwards, including when it did not exist.
///
/// On macOS the file goes through `NSFileManager`,
/// rather than through Finder, which needs an automation permission and a logged in session.
/// On Windows a file on a drive without a recycle bin, such as a network share, is refused,
/// since the shell would delete it permanently, which the C++ application refuses too.
#[must_use]
pub(crate) fn trash_file(path: &Path) -> bool {
    if !path.exists() {
        return true;
    }
    #[cfg(windows)]
    if let Some(folder) = path.parent()
        && !windows_recycle_bin::folder_has_recycle_bin(folder)
    {
        tracing::warn!(
            "Did not move {} to the recycle bin, because its drive would delete it permanently",
            path.display()
        );
        return false;
    }
    #[cfg_attr(
        not(target_os = "macos"),
        expect(unused_mut, reason = "only macOS changes the delete method")
    )]
    let mut context = trash::TrashContext::default();
    #[cfg(target_os = "macos")]
    context.set_delete_method(DeleteMethod::NsFileManager);
    match context.delete(path) {
        Ok(()) => !path.exists(),
        Err(error) => {
            tracing::warn!("Could not move {} to the trash: {error}", path.display());
            false
        }
    }
}

/// Checks that the output path can take the output of the given source,
/// and returns the user-facing error when it cannot.
///
/// A folder at the output path is never replaced, even with `overwrite_existing`,
/// and a different file there needs `overwrite_existing`.
///
/// # Errors
///
/// Returns the user-facing message when the output path is taken.
pub fn check_output_path(source: &Path, output: &Path, overwrite_existing: bool) -> Result<(), OutputPathError> {
    if output == source || !output.exists() {
        return Ok(());
    }
    if !output.is_file() {
        return Err(OutputPathError::NotFile(output.to_path_buf()));
    }
    if !overwrite_existing {
        return Err(OutputPathError::Exists(output.to_path_buf()));
    }
    Ok(())
}

/// Creates a hidden temporary file in the folder of the output path, removed when dropped.
///
/// The hidden name keeps a half-written output out of folder scans, also after a crash.
///
/// # Errors
///
/// Returns an error when the file cannot be created.
pub fn create_temporary_output(output: &Path) -> Result<NamedTempFile, OutputError> {
    let directory = output.parent().filter(|parent| !parent.as_os_str().is_empty());
    let stem = output.file_stem().and_then(|stem| stem.to_str()).unwrap_or("output");
    let extension = output
        .extension()
        .and_then(|extension| extension.to_str())
        .map(|extension| format!(".{extension}"))
        .unwrap_or_default();
    let prefix = format!(".{stem}_temp");
    let mut builder = tempfile::Builder::new();
    builder.prefix(&prefix).suffix(&extension);
    // Temporary files are private by default, but the output should get the usual permissions.
    #[cfg(unix)]
    builder.permissions(fs::Permissions::from_mode(0o666));
    builder
        .tempfile_in(directory.unwrap_or_else(|| Path::new(".")))
        .map_err(OutputError::Temporary)
}

/// Checks that a written file reads back with the channels, sample rate, and length that were rendered.
///
/// # Errors
///
/// Returns the user-facing reason when the file does not match.
pub fn validate_written_audio(
    path: &Path,
    channels: u32,
    sample_rate: u32,
    frames: u64,
) -> Result<(), OutputValidationError> {
    let decoder = AudioFileDecoder::open(path).map_err(OutputValidationError::Unreadable)?;
    let info = decoder.info();
    if info.channels != channels {
        return Err(OutputValidationError::Channels {
            actual: info.channels,
            expected: channels,
        });
    }
    if info.sample_rate != sample_rate {
        return Err(OutputValidationError::SampleRate {
            actual: info.sample_rate,
            expected: sample_rate,
        });
    }
    let written_frames = info.expected_frames.unwrap_or_default();
    if written_frames != frames {
        return Err(OutputValidationError::Frames {
            actual: written_frames,
            expected: frames,
        });
    }
    Ok(())
}

/// Disposes of a copy of the file under the same name, leaving the file itself in place.
///
/// Used before an in-place rewrite that cannot be undone,
/// so the original stays recoverable from the trash.
/// The copy is made in a new uniquely named folder in the system temp directory, removed afterwards,
/// because workers back up files concurrently.
/// Returns true when the copy was made and disposed of.
#[must_use]
pub fn dispose_of_copy(path: &Path, dispose: &FileDisposer) -> bool {
    let Ok(directory) = tempfile::Builder::new()
        .prefix("original-")
        .tempdir_in(temporary_root())
    else {
        return false;
    };
    let Some(name) = path.file_name() else {
        return false;
    };
    let copy = directory.path().join(name);
    // The disposer reports a missing file as gone, so the copy is checked first,
    // otherwise a lost copy would count as a recoverable original.
    fs::copy(path, &copy).is_ok() && copy.is_file() && dispose(&copy)
}

/// Moves a finished temporary output into place next to its source and disposes of the source.
///
/// When the output path is the source itself, the source is replaced in place.
/// Otherwise an output path that fails `check_output_path` fails the commit,
/// and a file allowed to be overwritten is renamed to a hidden backup next to it first.
/// After the output is in place, a source with a different path is disposed of,
/// and when that fails the output is removed again and the backup is renamed back,
/// so the folder is left as it was.
/// The backup is disposed of last, once everything else succeeded.
///
/// # Errors
///
/// Returns the user-facing message on failure, and the temporary file is removed.
pub fn commit_temporary_output(
    temporary: NamedTempFile,
    source: &Path,
    output: &Path,
    overwrite_existing: bool,
    dispose: &FileDisposer,
) -> Result<(), OutputError> {
    let replacing_source = output == source;
    check_output_path(source, output, overwrite_existing)?;

    let backup = if !replacing_source && output.exists() {
        // Renaming onto a taken backup name would overwrite an earlier backup, so a full set of backups fails.
        let Some(path) = backup_path(output) else {
            return Err(OutputError::BackupNamesExhausted);
        };
        fs::rename(output, &path).map_err(OutputError::Backup)?;
        Some(path)
    } else {
        None
    };

    // The data reaches the disk before the rename, so a power loss cannot leave an empty file in place.
    if let Err(error) = temporary.as_file().sync_all() {
        restore_backup(backup.as_deref(), output);
        return Err(OutputError::Sync(error));
    }

    if let Err(error) = temporary.persist(output) {
        restore_backup(backup.as_deref(), output);
        if replacing_source && !source.exists() && error.file.path().is_file() {
            return Err(keep_new_audio_after_lost_original(error.file, output, error.error));
        }
        return Err(if replacing_source {
            OutputError::Replace(error.error)
        } else {
            OutputError::Persist(error.error)
        });
    }

    if !replacing_source && source.is_file() && !dispose(source) {
        if let Err(error) = fs::remove_file(output) {
            tracing::error!("Failed to delete file {}: {error}", output.display());
        }
        restore_backup(backup.as_deref(), output);
        return Err(OutputError::DisposeSource);
    }

    if let Some(backup) = backup.filter(|backup| backup.is_file())
        && !dispose(&backup)
    {
        tracing::warn!(
            "Could not move the replaced output to the system trash, it was kept as {}",
            backup.display()
        );
    }
    Ok(())
}

/// Returns the folder in the system temp directory where copies of originals are made.
fn temporary_root() -> PathBuf {
    let root = std::env::temp_dir().join("AudioBatch-rs");
    if let Err(error) = fs::create_dir_all(&root) {
        tracing::warn!("Could not create {}: {error}", root.display());
    }
    root
}

/// Moves the new audio out of the temporary file after an in-place replace lost the original,
/// so dropping the temporary file cannot delete the only remaining copy of the audio.
/// On Windows, replacing a file can remove the original and then fail to move the replacement into place.
/// The audio is kept next to the original as "<name> (recovered).<extension>", the name the C++ application uses.
/// Returns the user-facing error that says where the new audio was kept.
fn keep_new_audio_after_lost_original(temporary: NamedTempFile, output: &Path, source: std::io::Error) -> OutputError {
    let stem = output
        .file_stem()
        .map(|stem| stem.to_string_lossy().into_owned())
        .unwrap_or_default();
    let extension = output
        .extension()
        .map(|extension| format!(".{}", extension.to_string_lossy()))
        .unwrap_or_default();
    let kept = std::iter::once(format!("{stem} (recovered){extension}"))
        .chain((2..=MAXIMUM_BACKUP_INDEX).map(|index| format!("{stem} (recovered){index}{extension}")))
        .map(|name| output.with_file_name(name))
        .find(|candidate| !candidate.exists());
    if let Some(kept) = kept
        && temporary.persist_noclobber(&kept).is_ok()
    {
        tracing::error!(
            "The original file {} was removed while being replaced, the new audio was kept as {}",
            output.display(),
            kept.display()
        );
        return OutputError::Recovered { path: kept, source };
    }
    tracing::error!(
        "The original file {} was removed while being replaced",
        output.display()
    );
    OutputError::RecoveryFailed(source)
}

/// Returns an unused hidden backup path next to the output,
/// or `None` when every backup name up to the highest index is taken.
fn backup_path(output: &Path) -> Option<PathBuf> {
    let name = file_name(output);
    std::iter::once(output.with_file_name(format!(".{name}.backup")))
        .chain((1..=MAXIMUM_BACKUP_INDEX).map(|index| output.with_file_name(format!(".{name}.backup.{index}"))))
        .find(|candidate| !candidate.exists())
}

/// Renames the backup back to the output path, when there is one.
fn restore_backup(backup: Option<&Path>, output: &Path) {
    let Some(backup) = backup.filter(|backup| backup.is_file()) else {
        return;
    };
    if fs::rename(backup, output).is_err() {
        tracing::error!(
            "Could not restore the previous output {} from its backup {}",
            output.display(),
            backup.display()
        );
    }
}

/// Returns the file name of the path for messages.
fn file_name(path: &Path) -> String {
    path.file_name().map_or_else(
        || path.display().to_string(),
        |name| name.to_string_lossy().into_owned(),
    )
}

#[cfg(test)]
mod test_check_output_path {
    use std::error::Error as _;

    use super::*;

    #[test]
    fn validation_preserves_the_decoder_cause() {
        let error = OutputValidationError::Unreadable(DecodeError::Open(std::io::Error::new(
            std::io::ErrorKind::PermissionDenied,
            "permission denied",
        )));
        assert_eq!(error.to_string(), "the output file cannot be read");
        let decoder = error.source().expect("decoder cause");
        assert_eq!(decoder.to_string(), "File could not be opened for reading");
        assert_eq!(decoder.source().expect("I/O cause").to_string(), "permission denied");
    }

    #[test]
    fn output_path_errors_keep_the_existing_messages() {
        assert_eq!(
            OutputPathError::Exists(PathBuf::from("a.aif")).to_string(),
            "Output file a.aif already exists. Use force overwrite to replace it."
        );
        assert_eq!(
            OutputPathError::NotFile(PathBuf::from("a.aif")).to_string(),
            "Output path a.aif is not a file and cannot be replaced."
        );
    }

    #[test]
    fn source_and_missing_paths_are_free() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("a.aif");
        assert_eq!(check_output_path(&source, &source, false), Ok(()));
        assert_eq!(
            check_output_path(&source, &directory.path().join("b.aif"), false),
            Ok(())
        );
    }

    #[test]
    fn existing_file_needs_overwrite() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("a.wav");
        let output = directory.path().join("a.aif");
        fs::write(&output, b"x").expect("writes");
        assert_eq!(
            check_output_path(&source, &output, false),
            Err(OutputPathError::Exists(output.clone()))
        );
        assert_eq!(check_output_path(&source, &output, true), Ok(()));
    }

    #[test]
    fn folder_is_never_replaced() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("a.wav");
        let output = directory.path().join("a.aif");
        fs::create_dir(&output).expect("creates folder");
        assert_eq!(
            check_output_path(&source, &output, true),
            Err(OutputPathError::NotFile(output.clone()))
        );
    }
}

#[cfg(test)]
mod test_commit_temporary_output {
    use std::io::Write;
    use std::sync::Mutex;

    use super::*;

    /// A disposer that deletes files and records their names, failing for names in `failing`.
    fn recording_disposer(disposed: Arc<Mutex<Vec<String>>>, failing: &'static [&'static str]) -> FileDisposer {
        Arc::new(move |path: &Path| {
            let name = file_name(path);
            if failing.contains(&name.as_str()) {
                return false;
            }
            disposed.lock().expect("lock").push(name);
            fs::remove_file(path).is_ok() || !path.exists()
        })
    }

    /// Creates a temporary output with the given contents.
    fn temporary_with(output: &Path, contents: &[u8]) -> NamedTempFile {
        let mut temporary = create_temporary_output(output).expect("creates temporary");
        temporary.write_all(contents).expect("writes");
        temporary
    }

    #[test]
    fn temporary_output_is_hidden_next_to_the_output() {
        let directory = tempfile::tempdir().expect("creates directory");
        let output = directory.path().join("song.aif");
        let temporary = create_temporary_output(&output).expect("creates temporary");
        assert_eq!(temporary.path().parent(), Some(directory.path()));
        assert!(file_name(temporary.path()).starts_with(".song_temp"));
        assert_eq!(
            temporary.path().extension().and_then(|extension| extension.to_str()),
            Some("aif")
        );
    }

    #[test]
    fn converted_source_is_disposed_of() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("song.wav");
        let output = directory.path().join("song.aif");
        fs::write(&source, b"source").expect("writes");
        let disposed = Arc::new(Mutex::new(Vec::new()));
        let dispose = recording_disposer(Arc::clone(&disposed), &[]);

        commit_temporary_output(temporary_with(&output, b"new"), &source, &output, false, &dispose).expect("commits");

        assert_eq!(fs::read(&output).expect("reads"), b"new");
        assert!(!source.exists());
        assert_eq!(*disposed.lock().expect("lock"), vec!["song.wav".to_string()]);
    }

    #[test]
    fn in_place_output_replaces_the_source() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("song.aif");
        fs::write(&source, b"old").expect("writes");
        let disposed = Arc::new(Mutex::new(Vec::new()));
        let dispose = recording_disposer(Arc::clone(&disposed), &[]);

        commit_temporary_output(temporary_with(&source, b"new"), &source, &source, false, &dispose).expect("commits");

        assert_eq!(fs::read(&source).expect("reads"), b"new");
        assert!(disposed.lock().expect("lock").is_empty());
    }

    #[test]
    fn overwritten_output_is_disposed_of_last() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("song.wav");
        let output = directory.path().join("song.aif");
        fs::write(&source, b"source").expect("writes");
        fs::write(&output, b"previous").expect("writes");
        let disposed = Arc::new(Mutex::new(Vec::new()));
        let dispose = recording_disposer(Arc::clone(&disposed), &[]);

        commit_temporary_output(temporary_with(&output, b"new"), &source, &output, true, &dispose).expect("commits");

        assert_eq!(fs::read(&output).expect("reads"), b"new");
        assert_eq!(
            *disposed.lock().expect("lock"),
            vec!["song.wav".to_string(), ".song.aif.backup".to_string()]
        );
    }

    #[test]
    fn failed_source_disposal_restores_the_folder() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("song.wav");
        let output = directory.path().join("song.aif");
        fs::write(&source, b"source").expect("writes");
        fs::write(&output, b"previous").expect("writes");
        let disposed = Arc::new(Mutex::new(Vec::new()));
        let dispose = recording_disposer(Arc::clone(&disposed), &["song.wav"]);

        let error = commit_temporary_output(temporary_with(&output, b"new"), &source, &output, true, &dispose)
            .expect_err("commit fails");

        assert!(matches!(error, OutputError::DisposeSource));
        assert_eq!(
            error.to_string(),
            "Could not move the original audio file to the system trash"
        );
        assert_eq!(fs::read(&source).expect("reads"), b"source");
        assert_eq!(fs::read(&output).expect("reads"), b"previous");
        let names: Vec<String> = fs::read_dir(directory.path())
            .expect("lists")
            .filter_map(|entry| entry.ok().map(|entry| entry.file_name().to_string_lossy().into_owned()))
            .collect();
        assert_eq!(names.len(), 2, "{names:?}");
    }

    #[test]
    fn failed_move_into_place_restores_the_replaced_output() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("song.wav");
        let output = directory.path().join("song.aif");
        fs::write(&source, b"source").expect("writes");
        fs::write(&output, b"previous").expect("writes");
        let disposed = Arc::new(Mutex::new(Vec::new()));
        let dispose = recording_disposer(Arc::clone(&disposed), &[]);
        let temporary = temporary_with(&output, b"new");
        // Removing the temporary file makes the rename onto the output fail after the backup was made.
        fs::remove_file(temporary.path()).expect("removes the temporary file");

        let error = commit_temporary_output(temporary, &source, &output, true, &dispose).expect_err("commit fails");

        assert!(matches!(error, OutputError::Persist(_)));
        assert_eq!(error.to_string(), "Could not move the new audio file into place");
        assert_eq!(fs::read(&output).expect("reads"), b"previous");
        assert_eq!(fs::read(&source).expect("reads"), b"source");
        assert!(disposed.lock().expect("lock").is_empty());
        let mut names: Vec<String> = fs::read_dir(directory.path())
            .expect("lists")
            .filter_map(|entry| entry.ok().map(|entry| entry.file_name().to_string_lossy().into_owned()))
            .collect();
        names.sort();
        assert_eq!(names, ["song.aif", "song.wav"]);
    }

    #[test]
    fn failed_in_place_replace_keeps_the_original() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("song.aif");
        fs::write(&source, b"original").expect("writes");
        let disposed = Arc::new(Mutex::new(Vec::new()));
        let dispose = recording_disposer(Arc::clone(&disposed), &[]);
        let temporary = temporary_with(&source, b"new");
        fs::remove_file(temporary.path()).expect("removes the temporary file");

        let error = commit_temporary_output(temporary, &source, &source, false, &dispose).expect_err("commit fails");

        assert!(matches!(error, OutputError::Replace(_)));
        assert_eq!(
            error.to_string(),
            "Could not replace the original file with the new audio"
        );
        assert_eq!(fs::read(&source).expect("reads"), b"original");
        assert!(disposed.lock().expect("lock").is_empty());
    }

    #[test]
    fn existing_output_without_overwrite_fails() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("song.wav");
        let output = directory.path().join("song.aif");
        fs::write(&source, b"source").expect("writes");
        fs::write(&output, b"previous").expect("writes");
        let dispose = recording_disposer(Arc::new(Mutex::new(Vec::new())), &[]);

        assert!(commit_temporary_output(temporary_with(&output, b"new"), &source, &output, false, &dispose).is_err());
        assert_eq!(fs::read(&output).expect("reads"), b"previous");
    }

    #[test]
    fn copy_of_the_original_is_disposed_of() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("song.aif");
        fs::write(&source, b"original").expect("writes");
        let disposed = Arc::new(Mutex::new(Vec::new()));
        let dispose = recording_disposer(Arc::clone(&disposed), &[]);

        assert!(dispose_of_copy(&source, &dispose));
        assert!(source.is_file());
        assert_eq!(*disposed.lock().expect("lock"), vec!["song.aif".to_string()]);
    }

    #[test]
    fn new_audio_is_kept_when_the_original_was_lost() {
        let directory = tempfile::tempdir().expect("creates directory");
        let output = directory.path().join("song.aif");
        fs::write(directory.path().join("song (recovered).aif"), b"earlier").expect("writes");

        let error = keep_new_audio_after_lost_original(
            temporary_with(&output, b"new"),
            &output,
            std::io::Error::other("replacement failed"),
        );

        assert_eq!(
            error.to_string(),
            "Could not replace the original file. The new audio was kept as song (recovered)2.aif"
        );
        assert_eq!(
            fs::read(directory.path().join("song (recovered)2.aif")).expect("reads"),
            b"new"
        );
        assert_eq!(
            fs::read(directory.path().join("song (recovered).aif")).expect("reads"),
            b"earlier"
        );
    }

    #[test]
    fn a_full_set_of_backups_fails_instead_of_overwriting_one() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("song.wav");
        let output = directory.path().join("song.aif");
        fs::write(&source, b"source").expect("writes");
        fs::write(&output, b"previous").expect("writes");
        fs::write(directory.path().join(".song.aif.backup"), b"backup").expect("writes");
        for index in 1..=MAXIMUM_BACKUP_INDEX {
            fs::write(directory.path().join(format!(".song.aif.backup.{index}")), b"backup").expect("writes");
        }
        let dispose = recording_disposer(Arc::new(Mutex::new(Vec::new())), &[]);

        let error = commit_temporary_output(temporary_with(&output, b"new"), &source, &output, true, &dispose)
            .expect_err("commit fails");

        assert!(matches!(error, OutputError::BackupNamesExhausted));
        assert_eq!(error.to_string(), "Could not move the existing output file aside");
        assert_eq!(fs::read(&output).expect("reads"), b"previous");
        assert_eq!(
            fs::read(directory.path().join(".song.aif.backup")).expect("reads"),
            b"backup"
        );
    }
}
