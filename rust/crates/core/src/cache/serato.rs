//! The Serato tag table of the analysis cache, which keeps what reading a file's Serato Overview tag found.
//!
//! Ports the `serato_tag_cache` table of the C++ `AnalysisCache` with the same schema and stored values.
//! A row is keyed by path and tag kind.
//! It only holds while the file keeps the stored size and modification time and the parser keeps the stored version.
//! Present overviews keep their decoded slices in the C++ `SOVR` representation.
//! Files without an overview, with an invalid one, or in a format without Serato tags keep only their state,
//! so the tag is not read again until the file changes.

use std::path::Path;
use std::sync::Arc;

use anyhow::{Context, Result, anyhow};
use rusqlite::{Connection, OptionalExtension, params};
use serato_tags::{OVERVIEW_PARSER_VERSION, OVERVIEW_TAG_KIND, Overview};

use super::{AnalysisCache, now_ms, path_key};
use crate::record::file_identity;
use crate::tags::SeratoOverviewState;

/// Schema of the Serato tag table, the same as in the C++ cache.
pub(super) const SCHEMA: &str = "CREATE TABLE IF NOT EXISTS serato_tag_cache (
    file_path TEXT NOT NULL,
    tag_kind INTEGER NOT NULL,
    file_size INTEGER NOT NULL,
    modified_time_ms INTEGER NOT NULL,
    parser_version INTEGER NOT NULL,
    result_state INTEGER NOT NULL,
    decoded_data BLOB,
    updated_at_ms INTEGER NOT NULL,
    PRIMARY KEY (file_path, tag_kind)
);";

/// Stored state of a present tag, the value of the C++ `ReadStatus::present`.
const STATE_PRESENT: i64 = 1;

/// Stored state of a missing tag.
const STATE_ABSENT: i64 = 2;

/// Stored state of a file format without Serato tags.
const STATE_UNSUPPORTED: i64 = 3;

/// Stored state of a tag that could not be parsed.
const STATE_INVALID: i64 = 4;

impl AnalysisCache {
    /// Returns the cached Serato Overview state of a file, or `None` when the file changed,
    /// the row is from another parser version, or the stored overview is invalid.
    #[must_use]
    pub fn get_serato_overview(&self, path: &Path) -> Option<SeratoOverviewState> {
        let (file_size, modified_time_ms) = file_identity(path)?;
        let (stored_size, stored_modified_time_ms, parser_version, state, data) = self
            .with_connection(|connection| {
                Ok(connection
                    .query_row(
                        "SELECT file_size, modified_time_ms, parser_version, result_state, decoded_data
                         FROM serato_tag_cache WHERE file_path = ?1 AND tag_kind = ?2",
                        params![path_key(path), OVERVIEW_TAG_KIND],
                        |row| {
                            Ok((
                                row.get::<_, i64>(0)?,
                                row.get::<_, i64>(1)?,
                                row.get::<_, i64>(2)?,
                                row.get::<_, i64>(3)?,
                                row.get::<_, Option<Vec<u8>>>(4)?,
                            ))
                        },
                    )
                    .optional()?)
            })
            .ok()??;
        if stored_size != i64::try_from(file_size).ok()?
            || stored_modified_time_ms != modified_time_ms
            || parser_version != i64::from(OVERVIEW_PARSER_VERSION)
        {
            return None;
        }
        match state {
            STATE_PRESENT => Some(SeratoOverviewState::Present(Arc::new(Overview::deserialize(&data?)?))),
            STATE_ABSENT => Some(SeratoOverviewState::Absent),
            STATE_UNSUPPORTED => Some(SeratoOverviewState::Unsupported),
            STATE_INVALID => Some(SeratoOverviewState::Invalid),
            _ => None,
        }
    }

    /// Stores the Serato Overview state of a file for its current size and modification time,
    /// replacing any earlier row for the file.
    ///
    /// # Errors
    ///
    /// Returns an error when the file cannot be read, the overview has no cache representation,
    /// or the database write fails.
    pub fn store_serato_overview(&self, path: &Path, state: &SeratoOverviewState) -> Result<()> {
        let (file_size, modified_time_ms) = file_identity(path)
            .ok_or_else(|| anyhow!("Cannot cache Serato tags of missing file {}", path.display()))?;
        let (code, data) = match state {
            SeratoOverviewState::Present(overview) => {
                let data = overview.serialize();
                if data.is_empty() {
                    return Err(anyhow!("Cannot cache a Serato overview with {overview}"));
                }
                (STATE_PRESENT, Some(data))
            }
            SeratoOverviewState::Absent => (STATE_ABSENT, None),
            SeratoOverviewState::Unsupported => (STATE_UNSUPPORTED, None),
            SeratoOverviewState::Invalid => (STATE_INVALID, None),
        };
        self.with_connection(|connection| {
            connection
                .execute(
                    "INSERT INTO serato_tag_cache (
                        file_path, tag_kind, file_size, modified_time_ms, parser_version, result_state,
                        decoded_data, updated_at_ms
                    ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
                    ON CONFLICT(file_path, tag_kind) DO UPDATE SET
                        file_size = excluded.file_size,
                        modified_time_ms = excluded.modified_time_ms,
                        parser_version = excluded.parser_version,
                        result_state = excluded.result_state,
                        decoded_data = excluded.decoded_data,
                        updated_at_ms = excluded.updated_at_ms",
                    params![
                        path_key(path),
                        OVERVIEW_TAG_KIND,
                        i64::try_from(file_size).unwrap_or(i64::MAX),
                        modified_time_ms,
                        OVERVIEW_PARSER_VERSION,
                        code,
                        data,
                        now_ms(),
                    ],
                )
                .context("SQLite could not store the Serato tags")?;
            Ok(())
        })
    }
}

/// Removes the Serato tag rows of a file, and returns how many were removed.
pub(super) fn remove_rows(connection: &Connection, path: &Path) -> rusqlite::Result<usize> {
    connection.execute(
        "DELETE FROM serato_tag_cache WHERE file_path = ?1",
        params![path_key(path)],
    )
}

#[cfg(test)]
mod test_serato_cache {
    use super::*;

    use serato_tags::BAND_COUNT;
    use tempfile::TempDir;

    /// A cache and a file to store Serato rows for, in a temporary directory.
    struct Setup {
        directory: TempDir,
        cache: AnalysisCache,
        path: std::path::PathBuf,
    }

    fn setup() -> Setup {
        let directory = TempDir::new().expect("temp dir");
        let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
        let path = directory.path().join("song.mp3");
        std::fs::write(&path, b"audio").expect("writes file");
        Setup { directory, cache, path }
    }

    fn present() -> SeratoOverviewState {
        let slices = (0..240).map(|slice| [(slice % 214 + 2) as u8; BAND_COUNT]).collect();
        SeratoOverviewState::Present(Arc::new(Overview::from_slices(slices)))
    }

    #[test]
    fn every_state_round_trips() {
        let setup = setup();
        assert_eq!(setup.cache.get_serato_overview(&setup.path), None);
        for state in [
            present(),
            SeratoOverviewState::Absent,
            SeratoOverviewState::Unsupported,
            SeratoOverviewState::Invalid,
        ] {
            setup.cache.store_serato_overview(&setup.path, &state).expect("stores");
            assert_eq!(setup.cache.get_serato_overview(&setup.path), Some(state));
        }
    }

    #[test]
    fn changed_file_misses() {
        let setup = setup();
        setup
            .cache
            .store_serato_overview(&setup.path, &present())
            .expect("stores");
        std::fs::write(&setup.path, b"changed audio").expect("rewrites file");
        assert_eq!(setup.cache.get_serato_overview(&setup.path), None);
    }

    #[test]
    fn other_parser_version_and_corrupt_data_miss() {
        let setup = setup();
        setup
            .cache
            .store_serato_overview(&setup.path, &present())
            .expect("stores");
        let update = |sql: &str| {
            setup
                .cache
                .with_connection(|connection| Ok(connection.execute(sql, [])?))
                .expect("updates");
        };
        update("UPDATE serato_tag_cache SET decoded_data = x'00'");
        assert_eq!(setup.cache.get_serato_overview(&setup.path), None);
        setup
            .cache
            .store_serato_overview(&setup.path, &present())
            .expect("stores");
        update("UPDATE serato_tag_cache SET parser_version = parser_version + 1");
        assert_eq!(setup.cache.get_serato_overview(&setup.path), None);
    }

    #[test]
    fn missing_file_and_empty_overview_are_not_stored() {
        let setup = setup();
        let missing = setup.directory.path().join("missing.mp3");
        assert!(setup.cache.store_serato_overview(&missing, &present()).is_err());
        let empty = SeratoOverviewState::Present(Arc::new(Overview::default()));
        assert!(setup.cache.store_serato_overview(&setup.path, &empty).is_err());
        assert_eq!(setup.cache.get_serato_overview(&setup.path), None);
    }

    #[test]
    fn remove_deletes_only_the_serato_row_of_the_file() {
        let setup = setup();
        let other = setup.directory.path().join("other.aiff");
        std::fs::write(&other, b"audio").expect("writes file");
        setup
            .cache
            .store_serato_overview(&setup.path, &present())
            .expect("stores");
        setup
            .cache
            .store_serato_overview(&other, &SeratoOverviewState::Absent)
            .expect("stores");
        assert!(setup.cache.remove_analysis(&setup.path).expect("removes"));
        assert!(!setup.cache.remove_analysis(&setup.path).expect("second remove"));
        assert_eq!(setup.cache.get_serato_overview(&setup.path), None);
        assert_eq!(
            setup.cache.get_serato_overview(&other),
            Some(SeratoOverviewState::Absent)
        );
    }
}
