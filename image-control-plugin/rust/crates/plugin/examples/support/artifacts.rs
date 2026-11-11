//! Explicit build-artifact authorization shared by native examples and packaged integration contracts.
//! Canonical paths prevent a symlink within an allowed tree from authorizing a different location.

use std::io;
use std::path::{Path, PathBuf};

/// Authorizes a selected artifact inside this repository or an explicitly supplied Cargo build root.
/// Never discovers a plugin from installed locations or treats the selected plugin itself as authorization.
pub fn checked_artifact(path: &Path) -> io::Result<PathBuf> {
    let repository = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../..");
    let target = std::env::var_os("CARGO_TARGET_DIR").filter(|value| !value.is_empty());
    checked_artifact_in(path, &repository, target.as_deref().map(Path::new))
}

/// Checks containment only after resolving both roots and the selected artifact against filesystem reality.
fn checked_artifact_in(path: &Path, repository: &Path, target: Option<&Path>) -> io::Result<PathBuf> {
    if !path.is_absolute() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "build artifact must be absolute",
        ));
    }
    let path = path.canonicalize()?;
    let repository = repository.canonicalize()?;
    if path.starts_with(repository) {
        return Ok(path);
    }
    if let Some(target) = target {
        let target = target.canonicalize()?;
        if path.starts_with(target) {
            return Ok(path);
        }
    }
    Err(io::Error::new(
        io::ErrorKind::PermissionDenied,
        "artifact must be inside the repository or the explicit CARGO_TARGET_DIR build root",
    ))
}

#[cfg(test)]
mod tests {
    use std::path::Path;

    use super::checked_artifact_in;

    /// A custom artifact root is authorized only by the caller's explicit target selection.
    #[test]
    fn external_artifacts_require_explicit_target_root() {
        let repository = tempfile::tempdir().expect("repository");
        let target = tempfile::tempdir().expect("custom target");
        let bundle = target.path().join("ImageControl RS.vst3");
        std::fs::create_dir(&bundle).expect("bundle");
        assert!(checked_artifact_in(&bundle, repository.path(), None).is_err());
        assert_eq!(
            checked_artifact_in(&bundle, repository.path(), Some(target.path())).expect("authorized build"),
            bundle.canonicalize().expect("bundle identity")
        );
        let unrelated = tempfile::tempdir().expect("unrelated location");
        assert!(checked_artifact_in(unrelated.path(), repository.path(), Some(target.path())).is_err());
        assert!(checked_artifact_in(Path::new("relative.vst3"), repository.path(), Some(target.path())).is_err());
    }

    /// Repository artifacts need no override, and an unrelated or missing override never disables that route.
    #[test]
    fn repository_artifacts_are_authorized_without_creating_paths() {
        let repository = tempfile::tempdir().expect("repository");
        let bundle = repository.path().join("ImageControl RS.vst3");
        std::fs::create_dir(&bundle).expect("bundle");
        let missing = repository.path().join("missing target");
        assert!(checked_artifact_in(&bundle, repository.path(), Some(&missing)).is_ok());
        assert!(!missing.exists());
    }

    /// A path-looking descendant cannot escape either authorized root through a filesystem link.
    #[cfg(target_os = "macos")]
    #[test]
    fn canonicalization_rejects_repository_and_target_symlink_escapes() {
        let repository = tempfile::tempdir().expect("repository");
        let target = tempfile::tempdir().expect("custom target");
        let unrelated = tempfile::tempdir().expect("outside artifact");
        for root in [repository.path(), target.path()] {
            let link = root.join("escape.vst3");
            std::os::unix::fs::symlink(unrelated.path(), &link).expect("symlink");
            assert!(checked_artifact_in(&link, repository.path(), Some(target.path())).is_err());
        }
    }
}
