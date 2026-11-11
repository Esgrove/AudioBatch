//! Versioned `ImageControl` RS sound presets with optimistic, exact-byte conflict checks.
//!
//! These synchronous operations allocate and access files. Call them only from a caller-owned non-audio worker,
//! never from a host state callback. The lock coordinates cooperating instances and processes without waiting.
//! Checks reject symlinks, but do not provide a sandbox against uncooperative processes replacing path components.

use std::collections::{HashSet, hash_map::RandomState};
use std::fs::{self, File, OpenOptions, TryLockError};
use std::hash::BuildHasher;
use std::io::{self, Read, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use serde::{Deserialize, Serialize};

use crate::BUNDLE_ID;
use crate::editing::{SoundSnapshot, valid_identity, valid_name};

/// First supported sound-preset schema, independent of the host-state envelope.
const PRESET_VERSION: u32 = 1;
/// Limit both allocation and reads, including files that grow after their metadata is inspected.
const MAXIMUM_PRESET_BYTES: usize = 64 * 1024;
/// This file must remain in place so every instance locks the same filesystem object.
const LOCK_FILENAME: &str = ".imagecontrol-rs.lock";
/// Temporary files are never considered library entries.
const TEMPORARY_PREFIX: &str = ".imagecontrol-rs-preset-";
/// Adds process-local uniqueness without claiming cryptographic identity generation.
static IDENTITY_COUNTER: AtomicU64 = AtomicU64::new(1);

/// A detached accepted library revision. Keep the complete selection, not only its stable identity.
#[derive(Clone, Debug)]
pub struct UserPreset {
    /// Stable opaque identity, preserved by updates and independent of its display name.
    pub id: String,
    /// Portable display name, unique case-insensitively within the library.
    pub name: String,
    /// Fresh opaque revision for every successful save, including metadata-only changes.
    pub revision: String,
    /// Exact saved sound, without audition controls, editor settings, history, or comparison state.
    pub sound: SoundSnapshot,
    /// Exact original bytes make whitespace-only external changes conflict as well.
    fingerprint: Vec<u8>,
    /// Private binding prevents project provenance from granting filesystem mutation permission.
    path: PathBuf,
}

/// An injected library location, retained without inspecting or creating anything at construction.
#[derive(Clone, Debug)]
pub struct PresetLibrary {
    /// Absolute directory used only by explicit filesystem operations.
    directory: PathBuf,
}

/// Owns a nonblocking library lock and explicitly releases it before closing the file.
/// A forked child can briefly inherit the open file description even when its descriptor is close-on-exec.
/// Unlocking explicitly prevents an inherited or duplicated descriptor from extending this guard's lock lifetime.
struct PresetLock {
    /// The locked regular file remains open until after explicit unlock.
    file: File,
}

impl Drop for PresetLock {
    /// Releases the operating-system lock even if an unrelated child still retains an inherited descriptor.
    /// Destructors cannot report unlock errors. Closing the owned file remains the fallback release mechanism.
    fn drop(&mut self) {
        let _ = self.file.unlock();
    }
}

/// Strict standalone JSON envelope, deliberately not a serialized host-state blob.
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct PresetDocument {
    /// Independent, explicit preset-format version.
    version: u32,
    /// Keeps unrelated `ImageControl` implementations and arbitrary JSON out of this format.
    plugin: String,
    /// Stable library identity, not a filename supplied by an imported document.
    id: String,
    /// Portable display name.
    name: String,
    /// Mutation identity used alongside the exact original bytes.
    revision: String,
    /// Complete normalized sound values in the stable sound-schema order.
    sound: SoundSnapshot,
}

impl PresetLibrary {
    /// Retains an injected directory without creating it or reading normal application storage.
    pub fn new(path: impl Into<PathBuf>) -> Self {
        Self { directory: path.into() }
    }

    /// Chooses storage shared by Rust plugin formats, separate from the C++ `ImageControl` library.
    pub fn default_path() -> io::Result<PathBuf> {
        dirs::data_dir()
            .map(|directory| directory.join("esgrove").join("ImageControl RS").join("Presets"))
            .ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::NotFound,
                    "The user application-data directory is unavailable",
                )
            })
    }

    /// Returns validated presets sorted by case-insensitive name. An absent library remains absent.
    /// Invalid or ambiguous JSON entries fail explicitly rather than silently hiding a conflicting preset.
    pub fn list(&self) -> io::Result<Vec<UserPreset>> {
        if !check_directory(&self.directory, false)? {
            return Ok(Vec::new());
        }
        let _lock = self.lock()?;
        self.scan()
    }

    /// Creates a new identity with an immutable identity-based filename, without overwriting any entry.
    pub fn create(&self, name: &str, sound: SoundSnapshot) -> io::Result<UserPreset> {
        validate_content(name, sound)?;
        check_directory(&self.directory, true)?;
        let _lock = self.lock()?;
        self.create_locked(name, sound)
    }

    /// Atomically replaces only the exact accepted revision, preserving its stable identity and filename.
    /// Renaming and sound replacement form one mutation and always receive a fresh revision.
    pub fn update(&self, selection: &UserPreset, name: &str, sound: SoundSnapshot) -> io::Result<UserPreset> {
        validate_content(name, sound)?;
        self.require_directory()?;
        let _lock = self.lock()?;
        self.accepted(selection)?;
        ensure_unique_name(&self.scan()?, name, Some(&selection.id))?;
        let document = PresetDocument {
            version: PRESET_VERSION,
            plugin: BUNDLE_ID.to_owned(),
            id: selection.id.clone(),
            name: name.to_owned(),
            revision: fresh_identity(),
            sound,
        };
        let bytes = encode(&document)?;
        atomic_write(&selection.path, &bytes, true, || self.accepted(selection).map(|_| ()))?;
        Ok(document.into_selection(selection.path.clone(), bytes))
    }

    /// Deletes only the accepted saved revision. The caller owns confirmation and project metadata changes.
    pub fn delete(&self, selection: &UserPreset) -> io::Result<()> {
        self.require_directory()?;
        let _lock = self.lock()?;
        self.accepted(selection)?;
        fs::remove_file(&selection.path)
    }

    /// Explicitly accepts the current library revision for a project identity, without applying its sound.
    /// A missing identity returns `None`, while malformed identities and unreadable libraries are errors.
    pub fn reconnect(&self, id: &str) -> io::Result<Option<UserPreset>> {
        if !valid_identity(id) {
            return Err(invalid_input("The preset identity is invalid"));
        }
        Ok(self.list()?.into_iter().find(|preset| preset.id == id))
    }

    /// Validates an external preset and saves its sound under a fresh local identity and revision.
    /// Import does not recall the sound, select the result, or preserve the source's mutation permission.
    pub fn import(&self, path: impl AsRef<Path>) -> io::Result<UserPreset> {
        let imported = read_preset(path.as_ref())?;
        self.create(&imported.name, imported.sound)
    }

    /// Exports the accepted saved bytes, never current edits, without overwriting an existing destination.
    /// Export does not create destination directories and leaves the library revision unchanged.
    pub fn export(&self, selection: &UserPreset, path: impl AsRef<Path>) -> io::Result<()> {
        self.require_directory()?;
        let _lock = self.lock()?;
        let saved = self.accepted(selection)?;
        atomic_write(path.as_ref(), &saved.fingerprint, false, || {
            self.accepted(selection).map(|_| ())
        })
    }

    /// Reports a missing library without creating directories during update, delete, or export.
    fn require_directory(&self) -> io::Result<()> {
        if check_directory(&self.directory, false)? {
            Ok(())
        } else {
            Err(io::Error::new(
                io::ErrorKind::NotFound,
                "The preset library no longer exists",
            ))
        }
    }

    /// Acquires an exclusive cross-thread and cross-process lock, failing immediately when it is busy.
    /// The returned guard explicitly unlocks before closing. The persistent lock file is never removed.
    fn lock(&self) -> io::Result<PresetLock> {
        self.require_directory()?;
        let path = self.directory.join(LOCK_FILENAME);
        check_regular_file_if_present(&path)?;
        let file = safe_options()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(&path)?;
        let metadata = file.metadata()?;
        if !metadata.is_file() || metadata_is_link(&metadata) || metadata_is_link(&fs::symlink_metadata(&path)?) {
            return Err(invalid_input("The preset library lock is not a regular file"));
        }
        file.try_lock().map_err(|error| match error {
            TryLockError::WouldBlock => io::Error::new(
                io::ErrorKind::WouldBlock,
                "The preset library is busy. Try the operation again",
            ),
            TryLockError::Error(error) => error,
        })?;
        Ok(PresetLock { file })
    }

    /// Enumerates only JSON files while holding the library lock, rejecting malformed and ambiguous entries.
    fn scan(&self) -> io::Result<Vec<UserPreset>> {
        let mut presets = Vec::new();
        let mut names = HashSet::new();
        let mut identities = HashSet::new();
        for entry in fs::read_dir(&self.directory)? {
            let path = entry?.path();
            if !path
                .extension()
                .is_some_and(|extension| extension.eq_ignore_ascii_case("json"))
            {
                continue;
            }
            let preset = read_preset(&path)?;
            if path.file_name() != Some(std::ffi::OsStr::new(&format!("{}.json", preset.id))) {
                return Err(invalid_data("A preset filename does not match its stable identity"));
            }
            if !names.insert(preset.name.to_lowercase()) || !identities.insert(preset.id.clone()) {
                return Err(invalid_data(
                    "The preset library contains duplicate names or identities",
                ));
            }
            presets.push(preset);
        }
        presets.sort_by_cached_key(|preset| (preset.name.to_lowercase(), preset.id.clone()));
        Ok(presets)
    }

    /// Creates an entry while holding the lock, including case-insensitive filename collision checks.
    fn create_locked(&self, name: &str, sound: SoundSnapshot) -> io::Result<UserPreset> {
        let presets = self.scan()?;
        ensure_unique_name(&presets, name, None)?;
        let document = PresetDocument {
            version: PRESET_VERSION,
            plugin: BUNDLE_ID.to_owned(),
            id: fresh_identity(),
            name: name.to_owned(),
            revision: fresh_identity(),
            sound,
        };
        let path = self.directory.join(format!("{}.json", document.id));
        let bytes = encode(&document)?;
        atomic_write(&path, &bytes, false, || check_filename_available(&path))?;
        Ok(document.into_selection(path, bytes))
    }

    /// Re-reads both content and private binding before granting permission to mutate or export a selection.
    fn accepted(&self, selection: &UserPreset) -> io::Result<UserPreset> {
        if !valid_identity(&selection.id)
            || selection.path != self.directory.join(format!("{}.json", selection.id))
            || selection.fingerprint.is_empty()
        {
            return Err(invalid_input("The selected preset does not belong to this library"));
        }
        let current = read_preset(&selection.path)?;
        if current.id != selection.id
            || current.name != selection.name
            || current.revision != selection.revision
            || current.sound.values.map(f64::to_bits) != selection.sound.values.map(f64::to_bits)
            || current.fingerprint != selection.fingerprint
        {
            return Err(io::Error::other(
                "The selected preset changed. Refresh the library and select it again",
            ));
        }
        Ok(current)
    }
}

impl PresetDocument {
    /// Detaches accepted file data from the codec without serializing paths or fingerprints into JSON.
    fn into_selection(self, path: PathBuf, fingerprint: Vec<u8>) -> UserPreset {
        UserPreset {
            id: self.id,
            name: self.name,
            revision: self.revision,
            sound: self.sound,
            fingerprint,
            path,
        }
    }
}

/// Validates every persisted value before creating a directory or temporary output.
fn validate_content(name: &str, sound: SoundSnapshot) -> io::Result<()> {
    if !valid_name(name)
        || name.eq_ignore_ascii_case(LOCK_FILENAME)
        || name.to_ascii_lowercase().starts_with(TEMPORARY_PREFIX)
    {
        return Err(invalid_input("The preset name is invalid or reserved"));
    }
    if !sound.is_valid() {
        return Err(invalid_input("The preset sound contains invalid normalized values"));
    }
    Ok(())
}

/// Rejects Unicode-lowercased name collisions, including case-only duplicates on case-sensitive volumes.
fn ensure_unique_name(presets: &[UserPreset], name: &str, except_id: Option<&str>) -> io::Result<()> {
    let folded = name.to_lowercase();
    if presets
        .iter()
        .any(|preset| Some(preset.id.as_str()) != except_id && preset.name.to_lowercase() == folded)
    {
        return Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "A preset with this name already exists",
        ));
    }
    Ok(())
}

/// Generates opaque nonzero 128-bit identities using two independently seeded standard-library hashers.
/// These are randomized uniqueness tokens, not standard UUIDs or security capabilities.
fn fresh_identity() -> String {
    let sequence = IDENTITY_COUNTER.fetch_add(1, Ordering::Relaxed);
    loop {
        let first = RandomState::new().hash_one((std::process::id(), sequence, 0_u8));
        let second = RandomState::new().hash_one((std::process::id(), sequence, 1_u8));
        let identity = format!("{first:016x}{second:016x}");
        if valid_identity(&identity) {
            return identity;
        }
    }
}

/// Encodes one bounded standalone document without including any library binding or host-state data.
fn encode(document: &PresetDocument) -> io::Result<Vec<u8>> {
    let mut bytes = serde_json::to_vec_pretty(document).map_err(invalid_data)?;
    bytes.push(b'\n');
    if bytes.len() > MAXIMUM_PRESET_BYTES {
        return Err(invalid_data("The preset exceeds the 64 KiB size limit"));
    }
    Ok(bytes)
}

/// Opens and reads at most the format limit plus one byte, rejecting links, directories, and oversized files.
fn read_preset(path: &Path) -> io::Result<UserPreset> {
    let parent = path
        .parent()
        .ok_or_else(|| invalid_input("The preset path has no parent directory"))?;
    if !check_directory(parent, false)? {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            "The preset directory does not exist",
        ));
    }
    let metadata = fs::symlink_metadata(path)?;
    if metadata_is_link(&metadata) || !metadata.is_file() {
        return Err(invalid_input(
            "The preset must be a regular file, not a directory or symbolic link",
        ));
    }
    let file = safe_options().read(true).open(path)?;
    let metadata = file.metadata()?;
    if !metadata.is_file() || metadata_is_link(&metadata) {
        return Err(invalid_input("The preset is not a regular file"));
    }
    let mut bytes = Vec::new();
    file.take(MAXIMUM_PRESET_BYTES as u64 + 1).read_to_end(&mut bytes)?;
    if bytes.is_empty() || bytes.len() > MAXIMUM_PRESET_BYTES {
        return Err(invalid_data("The preset is empty or exceeds the 64 KiB size limit"));
    }
    let document: PresetDocument = serde_json::from_slice(&bytes).map_err(invalid_data)?;
    if document.version != PRESET_VERSION
        || document.plugin != BUNDLE_ID
        || !valid_identity(&document.id)
        || !valid_identity(&document.revision)
    {
        return Err(invalid_data("The preset format, version, or identity is invalid"));
    }
    validate_content(&document.name, document.sound).map_err(invalid_data)?;
    Ok(document.into_selection(path.to_path_buf(), bytes))
}

/// Checks each explicit directory component before using or creating it, never following a symbolic link.
/// Absolute normalized paths avoid working-directory changes and ambiguous private selection bindings.
fn check_directory(path: &Path, create: bool) -> io::Result<bool> {
    if !path.is_absolute() {
        return Err(invalid_input("The preset directory must be an absolute path"));
    }
    if path
        .components()
        .any(|component| matches!(component, Component::CurDir | Component::ParentDir))
    {
        return Err(invalid_input(
            "The preset directory must not contain relative components",
        ));
    }
    let mut current = PathBuf::new();
    for component in path.components() {
        match component {
            Component::Prefix(_) | Component::RootDir => {
                current.push(component);
                continue;
            }
            Component::Normal(_) => current.push(component),
            Component::CurDir | Component::ParentDir => {
                return Err(invalid_input(
                    "The preset directory must not contain relative components",
                ));
            }
        }
        match fs::symlink_metadata(&current) {
            Ok(metadata) => {
                if metadata_is_link(&metadata) || !metadata.is_dir() {
                    return Err(invalid_input(
                        "A preset directory component is a symbolic link or is not a directory",
                    ));
                }
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound && create => {
                match fs::create_dir(&current) {
                    Ok(()) => {}
                    Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {}
                    Err(error) => return Err(error),
                }
                let metadata = fs::symlink_metadata(&current)?;
                if metadata_is_link(&metadata) || !metadata.is_dir() {
                    return Err(invalid_input("The created preset directory is not a regular directory"));
                }
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(false),
            Err(error) => return Err(error),
        }
    }
    Ok(true)
}

/// Rejects existing non-files before opening a persistent lock or staging an output.
fn check_regular_file_if_present(path: &Path) -> io::Result<bool> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata_is_link(&metadata) || !metadata.is_file() => Err(invalid_input(
            "The file path is a symbolic link, directory, or another non-regular file",
        )),
        Ok(_) => Ok(true),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error),
    }
}

/// Reserves case-insensitive output basenames even on case-sensitive volumes and for non-preset entries.
fn check_filename_available(path: &Path) -> io::Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| invalid_input("The output path has no parent directory"))?;
    let filename = path
        .file_name()
        .ok_or_else(|| invalid_input("The output path has no filename"))?;
    for entry in fs::read_dir(parent)? {
        if entry?.file_name().eq_ignore_ascii_case(filename) {
            return Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                "The destination filename already exists",
            ));
        }
    }
    Ok(())
}

/// Stages a fully flushed regular file in the destination directory and commits with atomic persistence.
/// The final callback rechecks an accepted revision immediately before replacement. New outputs never overwrite.
fn atomic_write(
    path: &Path,
    bytes: &[u8],
    replace: bool,
    before_commit: impl FnOnce() -> io::Result<()>,
) -> io::Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| invalid_input("The output path has no parent directory"))?;
    if !check_directory(parent, false)? {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            "The destination directory does not exist",
        ));
    }
    let exists = check_regular_file_if_present(path)?;
    if !replace && exists {
        return Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "The destination already exists",
        ));
    }
    if replace && !exists {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            "The selected preset no longer exists",
        ));
    }
    if !replace {
        check_filename_available(path)?;
    }
    let mut temporary = tempfile::Builder::new().prefix(TEMPORARY_PREFIX).tempfile_in(parent)?;
    temporary.write_all(bytes)?;
    temporary.flush()?;
    temporary.as_file().sync_all()?;
    before_commit()?;
    if replace {
        temporary.persist(path).map_err(|error| error.error)?;
    } else {
        temporary.persist_noclobber(path).map_err(|error| error.error)?;
    }
    Ok(())
}

/// Rejects Windows reparse points, including directory junctions, as well as ordinary symbolic links.
fn metadata_is_link(metadata: &fs::Metadata) -> bool {
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
        metadata.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0
    }
    #[cfg(not(windows))]
    {
        metadata.file_type().is_symlink()
    }
}

/// Opens final components without following links on supported platforms.
/// Windows handles also deny delete-sharing, keeping the persistent lock object stable while it is held.
fn safe_options() -> OpenOptions {
    let mut options = OpenOptions::new();
    #[cfg(target_os = "macos")]
    {
        use std::os::unix::fs::OpenOptionsExt;
        const O_NOFOLLOW: i32 = 0x100;
        options.custom_flags(O_NOFOLLOW);
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        const FILE_FLAG_OPEN_REPARSE_POINT: u32 = 0x0020_0000;
        const FILE_SHARE_READ: u32 = 1;
        const FILE_SHARE_WRITE: u32 = 2;
        options
            .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT)
            .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE);
    }
    options
}

/// Gives invalid caller input a stable error kind without exposing codec-specific error types to the editor.
fn invalid_input(message: impl Into<Box<dyn std::error::Error + Send + Sync>>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}

/// Distinguishes malformed stored content from invalid caller input and ordinary filesystem errors.
fn invalid_data(message: impl Into<Box<dyn std::error::Error + Send + Sync>>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Uses a canonical temporary root so system temporary-directory aliases are not library symlink components.
    fn isolated() -> (tempfile::TempDir, PresetLibrary) {
        let temporary = tempfile::tempdir().expect("temporary root");
        let path = temporary
            .path()
            .canonicalize()
            .expect("canonical temporary root")
            .join("Presets");
        (temporary, PresetLibrary::new(path))
    }

    /// Gives every sound value a finite independent off-grid value to catch codec rounding or omissions.
    fn sound() -> SoundSnapshot {
        SoundSnapshot {
            values: std::array::from_fn(|index| (index as f64 + 0.123_456_789) / 26.0),
        }
    }

    /// Tests exact floating-point representation, including signed zero, rather than approximate equality.
    fn assert_sound_bits(left: SoundSnapshot, right: SoundSnapshot) {
        assert_eq!(left.values.map(f64::to_bits), right.values.map(f64::to_bits));
    }

    #[test]
    fn construction_and_absent_listing_have_no_filesystem_side_effects() {
        let (_temporary, library) = isolated();
        assert!(!library.directory.exists());
        assert!(library.list().expect("absent listing").is_empty());
        assert!(library.reconnect(&fresh_identity()).expect("absent identity").is_none());
        assert!(!library.directory.exists());
        assert!(PresetLibrary::new("relative").list().is_err());
    }

    #[test]
    fn create_list_update_reconnect_export_import_and_delete_are_detached() {
        let (temporary, library) = isolated();
        let first_sound = sound();
        let first = library.create("Wide", first_sound).expect("create");
        assert!(valid_identity(&first.id));
        assert!(valid_identity(&first.revision));
        assert_ne!(first.id, first.revision);
        assert_sound_bits(first.sound, first_sound);
        let document: serde_json::Value = serde_json::from_slice(&first.fingerprint).expect("JSON");
        assert_eq!(document["version"], 1, "first supported preset format");
        assert_eq!(document["plugin"], BUNDLE_ID);
        assert!(document.get("path").is_none());
        assert!(document.get("comparison").is_none());
        assert_eq!(document["sound"]["values"].as_array().expect("values").len(), 25);

        let second = library
            .create("alpha", SoundSnapshot { values: [0.0; 25] })
            .expect("second");
        let listed = library.list().expect("list");
        assert_eq!(
            listed.iter().map(|preset| preset.name.as_str()).collect::<Vec<_>>(),
            ["alpha", "Wide"]
        );
        let changed = SoundSnapshot { values: [0.375; 25] };
        let updated = library.update(&first, "Renamed", changed).expect("update");
        assert_eq!(updated.id, first.id);
        assert_eq!(updated.path, first.path);
        assert_ne!(updated.revision, first.revision);
        assert_sound_bits(updated.sound, changed);
        let connected = library.reconnect(&first.id).expect("reconnect").expect("found");
        assert_eq!(connected.revision, updated.revision);

        let export = temporary.path().canonicalize().expect("root").join("export.json");
        library.export(&updated, &export).expect("export");
        assert_eq!(fs::read(&export).expect("export bytes"), updated.fingerprint);
        assert_eq!(
            library.export(&updated, &export).expect_err("no overwrite").kind(),
            io::ErrorKind::AlreadyExists
        );
        assert_eq!(
            library.import(&export).expect_err("duplicate import name").kind(),
            io::ErrorKind::AlreadyExists
        );
        let other = PresetLibrary::new(temporary.path().canonicalize().expect("root").join("Other"));
        let imported = other.import(&export).expect("import");
        assert_ne!(imported.id, updated.id);
        assert_ne!(imported.revision, updated.revision);
        assert_eq!(imported.name, updated.name);
        assert_sound_bits(imported.sound, changed);
        assert!(other.delete(&updated).is_err());
        library.delete(&updated).expect("delete");
        assert!(library.reconnect(&first.id).expect("missing").is_none());
        assert_eq!(
            library.list().expect("remaining").first().expect("second").id,
            second.id
        );
    }

    #[test]
    fn stale_revisions_and_whitespace_only_rewrites_cannot_mutate_or_export() {
        let (temporary, library) = isolated();
        let first = library.create("Original", sound()).expect("create");
        let second = library
            .update(&first, "Original", first.sound)
            .expect("same-sound revision");
        assert_ne!(second.revision, first.revision);
        assert!(library.update(&first, "Stale", first.sound).is_err());
        assert!(library.delete(&first).is_err());
        let export = temporary.path().canonicalize().expect("root").join("stale.json");
        assert!(library.export(&first, &export).is_err());
        assert!(!export.exists());
        let mut rewritten = second.fingerprint.clone();
        rewritten.push(b'\n');
        fs::write(&second.path, &rewritten).expect("external whitespace rewrite");
        assert!(library.update(&second, "Stale", second.sound).is_err());
        assert!(library.delete(&second).is_err());
        assert!(library.export(&second, &export).is_err());
        assert_eq!(fs::read(&second.path).expect("unchanged bytes"), rewritten);
        let accepted = library.reconnect(&second.id).expect("reconnect").expect("found");
        assert_eq!(accepted.revision, second.revision);
        library.delete(&accepted).expect("explicit acceptance");
    }

    #[test]
    fn invalid_names_and_nonfinite_or_out_of_domain_sound_do_not_create_storage() {
        let (_temporary, library) = isolated();
        for name in [
            "",
            " ",
            " leading",
            "trailing ",
            "bad/name",
            "bad\\name",
            "CON",
            "lpt1.txt",
            "NUL",
            "x.",
            "..",
            "x\n",
            LOCK_FILENAME,
            ".imagecontrol-rs-preset-collision",
        ] {
            assert_eq!(
                library.create(name, sound()).expect_err(name).kind(),
                io::ErrorKind::InvalidInput
            );
        }
        assert!(library.create(&"x".repeat(121), sound()).is_err());
        for value in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, -0.1, 1.1] {
            assert!(
                library
                    .create("Invalid", SoundSnapshot { values: [value; 25] })
                    .is_err()
            );
        }
        assert!(!library.directory.exists());
        assert!(library.reconnect("../../escape").is_err());
    }

    #[test]
    fn name_collisions_and_tampered_selection_fields_are_rejected() {
        let (_temporary, library) = isolated();
        let first = library.create("Original", sound()).expect("first");
        assert_eq!(
            library.create("ORIGINAL", sound()).expect_err("duplicate").kind(),
            io::ErrorKind::AlreadyExists
        );
        let second = library.create("Second", sound()).expect("second");
        assert!(library.update(&second, "original", sound()).is_err());
        let renamed = library.update(&first, "ORIGINAL", sound()).expect("case-only rename");
        let mut tampered = renamed.clone();
        tampered.name = "Pretend".to_owned();
        assert!(library.delete(&tampered).is_err());
        tampered = renamed;
        tampered.sound = SoundSnapshot { values: [0.75; 25] };
        assert!(library.update(&tampered, "Pretend", sound()).is_err());
        assert_eq!(library.list().expect("intact").len(), 2);
    }

    #[test]
    fn busy_libraries_fail_without_waiting_and_release_on_handle_drop() {
        let (_temporary, library) = isolated();
        let saved = library.create("Saved", sound()).expect("create");
        let lock = library.lock().expect("hold lock");
        let another = library.clone();
        for error in [
            another.list().expect_err("list busy"),
            another.create("New", sound()).expect_err("create busy"),
            another.update(&saved, "Updated", sound()).expect_err("update busy"),
            another.delete(&saved).expect_err("delete busy"),
            another.reconnect(&saved.id).expect_err("reconnect busy"),
        ] {
            assert_eq!(error.kind(), io::ErrorKind::WouldBlock);
        }
        let threaded = std::thread::spawn(move || another.list().expect_err("other thread busy").kind());
        assert_eq!(threaded.join().expect("thread"), io::ErrorKind::WouldBlock);
        drop(lock);
        library.delete(&saved).expect("released lock");
        assert!(library.directory.join(LOCK_FILENAME).is_file());
    }

    #[test]
    fn duplicated_descriptors_do_not_extend_guard_lock_ownership() {
        let (_temporary, library) = isolated();
        let saved = library.create("Saved", sound()).expect("create");
        let lock = library.lock().expect("hold lock");
        // A retained duplicate models the open file description inherited during a concurrent process spawn.
        let inherited = lock.file.try_clone().expect("duplicate locked file");
        assert_eq!(
            library.list().expect_err("guard still owns lock").kind(),
            io::ErrorKind::WouldBlock
        );
        drop(lock);
        library
            .delete(&saved)
            .expect("guard explicitly released lock despite retained descriptor");
        assert!(inherited.metadata().expect("retained descriptor stays open").is_file());
        drop(inherited);
    }

    /// The parent test starts this exact test in another process with an injected temporary library.
    #[test]
    fn subprocess_lock_probe() {
        let Some(directory) = std::env::var_os("IMAGECONTROL_RS_PRESET_LOCK_PROBE") else {
            return;
        };
        let library = PresetLibrary::new(PathBuf::from(directory));
        assert_eq!(
            library.list().expect_err("other process owns lock").kind(),
            io::ErrorKind::WouldBlock
        );
    }

    #[test]
    fn library_lock_is_shared_by_independent_processes() {
        let (_temporary, library) = isolated();
        library.create("Saved", sound()).expect("create");
        let _lock = library.lock().expect("parent owns lock");
        let mut child = std::process::Command::new(std::env::current_exe().expect("test executable"))
            .arg("--exact")
            .arg("user_presets::tests::subprocess_lock_probe")
            .arg("--nocapture")
            .env("IMAGECONTROL_RS_PRESET_LOCK_PROBE", &library.directory)
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()
            .expect("run independent lock probe");
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
        let completed = loop {
            if child.try_wait().expect("poll lock probe").is_some() {
                break true;
            }
            if std::time::Instant::now() >= deadline {
                child.kill().expect("stop timed-out lock probe");
                break false;
            }
            std::thread::sleep(std::time::Duration::from_millis(5));
        };
        let output = child.wait_with_output().expect("collect lock probe");
        assert!(completed, "independent lock probe timed out");
        assert!(
            output.status.success(),
            "child failed: {} {}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(String::from_utf8_lossy(&output.stdout).contains("1 passed"));
    }

    #[test]
    fn malformed_oversized_wrong_product_and_wrong_version_files_are_not_imported() {
        let (temporary, library) = isolated();
        let root = temporary.path().canonicalize().expect("root");
        let source = root.join("source.json");
        fs::write(&source, vec![b' '; MAXIMUM_PRESET_BYTES + 1]).expect("oversized");
        assert_eq!(
            library.import(&source).expect_err("size limit").kind(),
            io::ErrorKind::InvalidData
        );
        for bytes in [b"".as_slice(), b"not json", b"{}"] {
            fs::write(&source, bytes).expect("malformed");
            assert!(library.import(&source).is_err());
        }
        let origin = PresetLibrary::new(root.join("Origin"));
        let saved = origin.create("Source", sound()).expect("source");
        let base: serde_json::Value = serde_json::from_slice(&saved.fingerprint).expect("document");
        for (key, value) in [
            ("version", serde_json::json!(0)),
            ("version", serde_json::json!(PRESET_VERSION + 1)),
            ("plugin", serde_json::json!("com.esgrove.imagecontrol")),
            ("id", serde_json::json!("00000000000000000000000000000000")),
            ("revision", serde_json::json!("../unsafe")),
            ("unexpected", serde_json::json!(true)),
            ("name", serde_json::json!("CON")),
            ("sound", serde_json::json!({"values": [0.5]})),
            ("sound", serde_json::json!({"values": vec![2.0; 25]})),
        ] {
            let mut document = base.clone();
            document[key] = value;
            fs::write(&source, serde_json::to_vec(&document).expect("encode bad document"))
                .expect("write bad document");
            assert!(library.import(&source).is_err(), "accepted {key}");
        }
        assert!(!library.directory.exists());
    }

    #[test]
    fn listing_reports_corruption_and_identity_filename_mismatches() {
        let (_temporary, library) = isolated();
        let saved = library.create("Saved", sound()).expect("create");
        let misplaced = library.directory.join("wrong.json");
        fs::rename(&saved.path, &misplaced).expect("external rename");
        assert_eq!(
            library.list().expect_err("mismatched filename").kind(),
            io::ErrorKind::InvalidData
        );
        fs::rename(&misplaced, &saved.path).expect("return file");
        fs::write(&saved.path, b"corrupt").expect("corrupt");
        assert_eq!(
            library.list().expect_err("malformed entry").kind(),
            io::ErrorKind::InvalidData
        );
        assert_eq!(fs::read(&saved.path).expect("unchanged"), b"corrupt");
    }

    #[test]
    fn atomic_commit_failure_preserves_saved_bytes_and_cleans_temporary_outputs() {
        let (_temporary, library) = isolated();
        let saved = library.create("Saved", sound()).expect("create");
        let error = atomic_write(&saved.path, b"replacement", true, || Err(io::Error::other("conflict")))
            .expect_err("pre-commit conflict");
        assert_eq!(error.to_string(), "conflict");
        assert_eq!(fs::read(&saved.path).expect("saved bytes"), saved.fingerprint);
        assert_eq!(fs::read_dir(&library.directory).expect("entries").count(), 2);

        let destination = library.directory.join("reserved.txt");
        let error = atomic_write(&destination, b"candidate", false, || fs::write(&destination, b"winner"))
            .expect_err("noreplace race");
        assert_eq!(error.kind(), io::ErrorKind::AlreadyExists);
        assert_eq!(fs::read(&destination).expect("race winner"), b"winner");
        assert_eq!(fs::read_dir(&library.directory).expect("entries").count(), 3);
    }

    #[test]
    fn reserved_outputs_and_directories_never_get_replaced() {
        let (temporary, library) = isolated();
        let saved = library.create("Saved", sound()).expect("create");
        let root = temporary.path().canonicalize().expect("root");
        let destination = root.join("Destination.JSON");
        fs::write(&destination, b"keep").expect("reserve output");
        assert_eq!(
            library
                .export(&saved, root.join("destination.json"))
                .expect_err("case collision")
                .kind(),
            io::ErrorKind::AlreadyExists
        );
        assert_eq!(fs::read(&destination).expect("preserved"), b"keep");
        let directory = root.join("directory.json");
        fs::create_dir(&directory).expect("directory");
        assert!(library.export(&saved, &directory).is_err());
        assert!(library.import(&directory).is_err());
        let missing_parent = root.join("absent").join("export.json");
        assert!(library.export(&saved, &missing_parent).is_err());
        assert!(!root.join("absent").exists());
        let locked = PresetLibrary::new(root.join("BadLock"));
        fs::create_dir(&locked.directory).expect("library");
        fs::create_dir(locked.directory.join(LOCK_FILENAME)).expect("lock directory");
        assert!(locked.create("No", sound()).is_err());
    }

    #[test]
    fn numeric_round_trip_keeps_all_accepted_bits_and_noop_updates_get_new_revisions() {
        let (_temporary, library) = isolated();
        let values = std::array::from_fn(|index| match index % 5 {
            0 => -0.0,
            1 => f64::from_bits(1),
            2 => f64::from_bits(1.0_f64.to_bits() - 1),
            3 => 0.123_456_789_012_345_68,
            _ => 1.0,
        });
        let saved = library.create("Exact", SoundSnapshot { values }).expect("create");
        let listed = library.list().expect("list").pop().expect("saved");
        assert_sound_bits(saved.sound, listed.sound);
        let updated = library
            .update(&listed, &listed.name, listed.sound)
            .expect("no-op update");
        assert_ne!(updated.revision, listed.revision);
        assert_sound_bits(updated.sound, saved.sound);
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn symlinked_directories_presets_locks_sources_and_destinations_are_rejected() {
        use std::os::unix::fs::symlink;

        let (temporary, library) = isolated();
        let root = temporary.path().canonicalize().expect("root");
        let saved = library.create("Saved", sound()).expect("create");
        let alias = root.join("Alias");
        symlink(&library.directory, &alias).expect("directory link");
        assert!(PresetLibrary::new(&alias).list().is_err());
        assert!(PresetLibrary::new(alias.join("Child")).create("No", sound()).is_err());
        assert!(!library.directory.join("Child").exists());
        let source = root.join("source.json");
        symlink(&saved.path, &source).expect("source link");
        assert!(library.import(&source).is_err());
        let destination = root.join("destination.json");
        symlink(&saved.path, &destination).expect("destination link");
        assert!(library.export(&saved, &destination).is_err());
        let dangling = root.join("dangling.json");
        symlink(root.join("absent.json"), &dangling).expect("dangling link");
        assert!(library.export(&saved, &dangling).is_err());
        let external = root.join("external.json");
        fs::rename(&saved.path, &external).expect("move saved file");
        symlink(&external, &saved.path).expect("preset link");
        assert!(library.list().is_err());
        assert!(library.update(&saved, "No", sound()).is_err());
        assert!(library.delete(&saved).is_err());
        fs::remove_file(&saved.path).expect("remove preset link");
        fs::rename(&external, &saved.path).expect("restore saved file");
        fs::remove_file(library.directory.join(LOCK_FILENAME)).expect("remove idle lock");
        symlink(&external, library.directory.join(LOCK_FILENAME)).expect("lock link");
        assert!(library.list().is_err());
        assert!(library.delete(&saved).is_err());
        assert!(!external.exists());
        assert_eq!(fs::read(&saved.path).expect("original preserved"), saved.fingerprint);
    }
}
