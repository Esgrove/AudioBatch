//! Deterministic external replacement and output ownership regressions.

use std::fs;
use std::io::Write as _;
use std::path::Path;
use std::sync::Arc;

use super::{
    FileDisposer, MAXIMUM_BACKUP_INDEX, OutputError, SourceIdentity, commit_with_hook, create_temporary_output,
    keep_new_audio_after_lost_original,
};

/// Writes completed audio to a transaction temporary.
fn rendered(output: &Path) -> tempfile::NamedTempFile {
    let mut temporary = create_temporary_output(output).expect("temporary");
    temporary.write_all(b"rendered").expect("write");
    temporary
}

#[test]
fn source_replacement_with_identical_size_and_mtime_is_refused() {
    let directory = tempfile::tempdir().expect("directory");
    let source = directory.path().join("song.wav");
    let output = directory.path().join("song.aif");
    fs::write(&source, b"original").expect("source");
    fs::write(&output, b"approved").expect("output");
    let identity = SourceIdentity::capture(&source).expect("identity");
    let modified = fs::metadata(&source).expect("metadata").modified().expect("modified");
    let replacement = directory.path().join("replacement");
    fs::write(&replacement, b"external").expect("replacement");
    fs::File::options()
        .write(true)
        .open(&replacement)
        .expect("file")
        .set_modified(modified)
        .expect("mtime");
    fs::rename(&replacement, &source).expect("replace");
    let dispose: FileDisposer = Arc::new(|_| panic!("newer source must not be disposed"));
    let error =
        commit_with_hook(rendered(&output), &source, &output, true, &dispose, &identity, &|_| {}).expect_err("changed");
    assert!(matches!(error, OutputError::SourceChanged));
    assert_eq!(fs::read(source).expect("source"), b"external");
    assert_eq!(fs::read(output).expect("output"), b"approved");
}

#[test]
fn source_replacements_at_commit_boundaries_are_never_adopted_or_disposed() {
    for (in_place, stage) in [
        (true, "before_publish"),
        (false, "before_publish"),
        (false, "before_dispose"),
    ] {
        let directory = tempfile::tempdir().expect("directory");
        let source = directory.path().join("song.wav");
        let output = if in_place {
            source.clone()
        } else {
            directory.path().join("song.aif")
        };
        fs::write(&source, b"original").expect("source");
        if !in_place {
            fs::write(&output, b"approved").expect("output");
        }
        let identity = SourceIdentity::capture(&source).expect("identity");
        let dispose: FileDisposer = Arc::new(|_| panic!("replacement must not be disposed"));
        let error = commit_with_hook(
            rendered(&output),
            &source,
            &output,
            true,
            &dispose,
            &identity,
            &|current| {
                if current == stage {
                    let replacement = directory.path().join("replacement");
                    fs::write(&replacement, b"external").expect("replacement");
                    fs::rename(replacement, &source).expect("replace");
                }
            },
        )
        .expect_err("source changed");
        assert!(matches!(error, OutputError::SourceChanged), "{error}");
        assert_eq!(fs::read(&source).expect("source"), b"external");
        if !in_place {
            assert_eq!(fs::read(&output).expect("output"), b"approved");
        }
    }
}

#[test]
fn publication_does_not_clobber_a_concurrent_occupant_with_either_overwrite_policy() {
    for overwrite in [false, true] {
        let directory = tempfile::tempdir().expect("directory");
        let source = directory.path().join("song.wav");
        let output = directory.path().join("song.aif");
        fs::write(&source, b"source").expect("source");
        if overwrite {
            fs::write(&output, b"approved").expect("output");
        }
        let identity = SourceIdentity::capture(&source).expect("identity");
        let dispose: FileDisposer = Arc::new(|_| panic!("no disposal after failed publication"));
        let error = commit_with_hook(
            rendered(&output),
            &source,
            &output,
            overwrite,
            &dispose,
            &identity,
            &|stage| {
                if stage == "before_publish" {
                    fs::write(&output, b"external").expect("occupant");
                }
            },
        )
        .expect_err("occupied");
        let OutputError::Recovered { path, .. } = error else {
            panic!("render retained: {error}");
        };
        assert_eq!(fs::read(path).expect("rendered"), b"rendered");
        assert_eq!(fs::read(&output).expect("output"), b"external");
        assert_eq!(fs::read(&source).expect("source"), b"source");
        if overwrite {
            assert_eq!(
                fs::read(directory.path().join(".song.aif.backup")).expect("backup"),
                b"approved"
            );
        }
    }
}

#[test]
fn rollback_never_deletes_or_replaces_a_new_output_occupant() {
    let directory = tempfile::tempdir().expect("directory");
    let source = directory.path().join("song.wav");
    let output = directory.path().join("song.aif");
    fs::write(&source, b"source").expect("source");
    fs::write(&output, b"approved").expect("output");
    let identity = SourceIdentity::capture(&source).expect("identity");
    let dispose: FileDisposer = Arc::new(|_| false);
    let error = commit_with_hook(
        rendered(&output),
        &source,
        &output,
        true,
        &dispose,
        &identity,
        &|stage| {
            if stage == "before_rollback" {
                fs::remove_file(&output).expect("remove published");
                fs::write(&output, b"external").expect("occupant");
            }
        },
    )
    .expect_err("rollback failed");
    let OutputError::Recovered { path, .. } = error else {
        panic!("render retained: {error}");
    };
    assert_eq!(fs::read(path).expect("render"), b"rendered");
    assert_eq!(fs::read(output).expect("output"), b"external");
    assert_eq!(fs::read(source).expect("source"), b"source");
    assert_eq!(
        fs::read(directory.path().join(".song.aif.backup")).expect("backup"),
        b"approved"
    );
}

#[test]
fn a_backup_name_claimed_at_the_seam_is_never_replaced() {
    let directory = tempfile::tempdir().expect("directory");
    let source = directory.path().join("song.wav");
    let output = directory.path().join("song.aif");
    let backup = directory.path().join(".song.aif.backup");
    fs::write(&source, b"source").expect("source");
    fs::write(&output, b"approved").expect("output");
    let identity = SourceIdentity::capture(&source).expect("identity");
    let dispose: FileDisposer = Arc::new(|path| fs::remove_file(path).is_ok());
    commit_with_hook(
        rendered(&output),
        &source,
        &output,
        true,
        &dispose,
        &identity,
        &|stage| {
            if stage == "before_backup_claim" && !backup.exists() {
                fs::write(&backup, b"external backup").expect("backup");
            }
        },
    )
    .expect("commit");
    assert_eq!(fs::read(backup).expect("backup"), b"external backup");
}

#[test]
fn exhausted_recovery_names_keep_the_original_temporary_path() {
    let directory = tempfile::tempdir().expect("directory");
    let output = directory.path().join("song.aif");
    for index in 1..=MAXIMUM_BACKUP_INDEX {
        let suffix = if index == 1 { String::new() } else { index.to_string() };
        fs::write(
            directory.path().join(format!("song (recovered){suffix}.aif")),
            b"occupied",
        )
        .expect("occupant");
    }
    let temporary = rendered(&output);
    let original = temporary.path().to_path_buf();
    let error = keep_new_audio_after_lost_original(temporary, &output, std::io::Error::other("injected"));
    let OutputError::Recovered { path, .. } = error else {
        panic!("retained");
    };
    assert_eq!(path, original);
    assert_eq!(fs::read(path).expect("render retained"), b"rendered");
}

#[test]
fn recovery_filesystem_failure_keeps_the_original_temporary_path() {
    let directory = tempfile::tempdir().expect("directory");
    let output = directory.path().join("song.aif");
    let temporary = rendered(&output);
    let original = temporary.path().to_path_buf();
    let impossible_output = directory.path().join("missing").join("song.aif");
    let error = keep_new_audio_after_lost_original(temporary, &impossible_output, std::io::Error::other("injected"));
    let OutputError::Recovered { path, .. } = error else {
        panic!("retained");
    };
    assert_eq!(path, original);
    assert_eq!(fs::read(path).expect("render retained"), b"rendered");
}
