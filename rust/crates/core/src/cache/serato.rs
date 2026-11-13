//! The Serato tag table of the analysis cache, which keeps what reading a file's Serato Overview tag found.
//!
//! The `serato_tag_cache` table shares its schema and stored values with the C++ application.
//! A row is keyed by path and tag kind.
//! It only holds while the file keeps the stored size and modification time and the parser keeps the stored version.
//! The read/parser version covers duplicate selection and recovered errors independently of the raster blob version,
//! so stale present and missing outcomes are re-read together.
//! Present overviews keep their decoded slices in the `SOVR` representation.
//! Files without an overview, with an invalid one, or in a format without Serato tags keep only their state,
//! so callers can distinguish those results from an uncached file without parsing the tag.
//! The GUI still refreshes display tags in the background and updates the cached state when that read succeeds.

use std::path::Path;
use std::sync::Arc;

use rusqlite::types::ValueRef;
use rusqlite::{OptionalExtension, params};
use serato_tags::{OVERVIEW_PARSER_VERSION, OVERVIEW_TAG_KIND, Overview};

use super::{AnalysisCache, CacheError, Result, now_ms, path_key};
use crate::record::{AnalysisRecord, FileIdentity, file_identity};
use crate::tags::SeratoOverviewState;

/// Schema of the Serato tag table.
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

/// Stored state of a present tag.
const STATE_PRESENT: i64 = 1;

/// Stored state of a missing tag.
const STATE_ABSENT: i64 = 2;

/// Stored state of a file format without Serato tags.
const STATE_UNSUPPORTED: i64 = 3;

/// Stored state of a tag that could not be parsed.
const STATE_INVALID: i64 = 4;

/// Stored source identity observed in `serato_tag_cache` before updating a row.
#[derive(Clone, Copy)]
struct ObservedSeratoIdentity {
    file_size: i64,
    modified_time_ms: i64,
}

impl AnalysisCache {
    /// Returns the cached Serato Overview state of a file, or `None` when the file changed,
    /// the row is from another parser version, or the stored overview is invalid.
    #[must_use]
    pub fn get_serato_overview(&self, path: &Path) -> Option<SeratoOverviewState> {
        let FileIdentity {
            file_size,
            modified_time_ms,
        } = file_identity(path)?;
        self.with_connection(|connection| {
            Ok(connection
                .prepare_cached(
                    "SELECT file_size, modified_time_ms, parser_version, result_state, decoded_data, updated_at_ms
                         FROM serato_tag_cache WHERE file_path = ?1 AND tag_kind = ?2",
                )?
                .query_row(params![path_key(path), OVERVIEW_TAG_KIND], |row| {
                    if row.get::<_, i64>(0)? != i64::try_from(file_size).unwrap_or(-1)
                        || row.get::<_, i64>(1)? != modified_time_ms
                        || row.get::<_, i64>(2)? != i64::from(OVERVIEW_PARSER_VERSION)
                        || row.get::<_, i64>(5)? <= 0
                    {
                        return Ok(None);
                    }
                    let state: i64 = row.get(3)?;
                    if state != STATE_PRESENT && !matches!(row.get_ref(4)?, ValueRef::Null) {
                        return Ok(None);
                    }
                    Ok(match state {
                        STATE_PRESENT => row
                            .get_ref(4)?
                            .as_blob()
                            .ok()
                            .and_then(Overview::deserialize)
                            .map(|overview| SeratoOverviewState::Present(Arc::new(overview))),
                        STATE_ABSENT => Some(SeratoOverviewState::Absent),
                        STATE_UNSUPPORTED => Some(SeratoOverviewState::Unsupported),
                        STATE_INVALID => Some(SeratoOverviewState::Invalid),
                        _ => None,
                    })
                })
                .optional()?)
        })
        .ok()
        .flatten()
        .flatten()
    }

    /// Stores the Serato Overview state of a file for its current size and modification time,
    /// replacing any earlier row for the file.
    ///
    /// # Errors
    ///
    /// Returns an error when the file cannot be read, the overview has no cache representation,
    /// or the database write fails.
    pub fn store_serato_overview(&self, path: &Path, state: &SeratoOverviewState) -> Result<()> {
        let FileIdentity {
            file_size,
            modified_time_ms,
        } = file_identity(path).ok_or_else(|| CacheError::MissingSeratoFile {
            path: path.to_path_buf(),
        })?;
        self.store_serato_overview_for_record(
            &AnalysisRecord {
                path: path.to_path_buf(),
                file_size,
                modified_time_ms,
                ..Default::default()
            },
            state,
        )
        .map(|_| ())
    }

    /// Publishes parsed metadata only for the captured source version, without replacing newer tag work.
    /// Negative parser outcomes are complete results and remain independent of measurement validity.
    ///
    /// # Errors
    ///
    /// Returns an error for an invalid overview or a failed database write.
    pub fn store_serato_overview_for_record(
        &self,
        record: &AnalysisRecord,
        state: &SeratoOverviewState,
    ) -> Result<bool> {
        let observed_source = self.with_connection(|connection| {
            Ok(connection
                .prepare_cached(
                    "SELECT file_size, modified_time_ms FROM serato_tag_cache WHERE file_path = ?1 AND tag_kind = ?2",
                )?
                .query_row(params![path_key(&record.path), OVERVIEW_TAG_KIND], |row| {
                    Ok(ObservedSeratoIdentity {
                        file_size: row.get(0)?,
                        modified_time_ms: row.get(1)?,
                    })
                })
                .optional()?)
        })?;
        if !record.matches_file_on_disk() {
            return Ok(false);
        }
        let path = &record.path;
        let file_size = record.file_size;
        let modified_time_ms = record.modified_time_ms;
        let (code, data) = match state {
            SeratoOverviewState::Present(overview) => {
                let data = overview.serialize();
                if data.is_empty() {
                    return Err(CacheError::InvalidSeratoOverview {
                        overview: Arc::clone(overview),
                    });
                }
                (STATE_PRESENT, Some(data))
            }
            SeratoOverviewState::Absent => (STATE_ABSENT, None),
            SeratoOverviewState::Unsupported => (STATE_UNSUPPORTED, None),
            SeratoOverviewState::Invalid => (STATE_INVALID, None),
        };
        self.with_connection(|connection| {
            let updated = connection
                .prepare_cached(
                    "INSERT INTO serato_tag_cache (
                        file_path, tag_kind, file_size, modified_time_ms, parser_version, result_state,
                        decoded_data, updated_at_ms
                    ) SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8
                    WHERE NOT EXISTS (SELECT 1 FROM file_analysis WHERE file_path = ?1
                        AND (file_size != ?3 OR modified_time_ms != ?4))
                    ON CONFLICT(file_path, tag_kind) DO UPDATE SET
                        file_size = excluded.file_size,
                        modified_time_ms = excluded.modified_time_ms,
                        parser_version = excluded.parser_version,
                        result_state = excluded.result_state,
                        decoded_data = excluded.decoded_data,
                        updated_at_ms = excluded.updated_at_ms
                    WHERE ((serato_tag_cache.file_size = excluded.file_size
                            AND serato_tag_cache.modified_time_ms = excluded.modified_time_ms)
                        OR (?9 AND serato_tag_cache.file_size = ?10 AND serato_tag_cache.modified_time_ms = ?11))
                        AND ((serato_tag_cache.file_size != excluded.file_size
                            OR serato_tag_cache.modified_time_ms != excluded.modified_time_ms)
                            OR serato_tag_cache.parser_version <= excluded.parser_version)",
                )
                .and_then(|mut statement| {
                    statement.execute(params![
                        path_key(path),
                        OVERVIEW_TAG_KIND,
                        i64::try_from(file_size).unwrap_or(i64::MAX),
                        modified_time_ms,
                        OVERVIEW_PARSER_VERSION,
                        code,
                        data,
                        now_ms(),
                        observed_source.is_some(),
                        observed_source.map_or(0, |identity| identity.file_size),
                        observed_source.map_or(0, |identity| identity.modified_time_ms),
                    ])
                })
                .map_err(CacheError::StoreSerato)?;
            Ok(updated > 0)
        })
    }
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

    /// Creates a cache and a placeholder audio file in a temporary directory.
    fn setup() -> Setup {
        let directory = TempDir::new().expect("temp dir");
        let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
        let path = directory.path().join("song.mp3");
        std::fs::write(&path, b"audio").expect("writes file");
        Setup { directory, cache, path }
    }

    /// Returns a present Serato Overview with a repeating colour pattern.
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
    fn old_invalid_native_overview_is_reparsed_and_replaced() {
        let setup = setup();
        let path = test_fixtures::metadata_fixtures()
            .into_iter()
            .find(|case| case.file == "dmt_drum_127_26A.flac")
            .expect("real FLAC case")
            .path();
        setup
            .cache
            .store_serato_overview(&path, &SeratoOverviewState::Invalid)
            .expect("old invalid state");
        setup
            .cache
            .with_connection(|connection| Ok(connection.execute("UPDATE serato_tag_cache SET parser_version = 1", [])?))
            .expect("marks pre-repair parser");
        assert_eq!(setup.cache.get_serato_overview(&path), None);
        let tags = crate::tags::read_display_tags(&path).expect("repaired native decoder");
        let state = SeratoOverviewState::from_tags(&path, &tags);
        assert!(matches!(state, SeratoOverviewState::Present(_)));
        setup
            .cache
            .store_serato_overview(&path, &state)
            .expect("replaces old invalid row");
        assert_eq!(setup.cache.get_serato_overview(&path), Some(state));
    }

    #[test]
    fn version_two_outcomes_miss_and_current_read_replaces_them() {
        let setup = setup();
        let path = test_fixtures::metadata_fixtures()
            .into_iter()
            .find(|case| case.file == "dmt_drum_127_26A.flac")
            .expect("real FLAC case")
            .path();
        let tags = crate::tags::read_display_tags(&path).expect("current native reader");
        let refreshed = SeratoOverviewState::from_tags(&path, &tags);
        assert!(matches!(refreshed, SeratoOverviewState::Present(_)));
        for stale in [
            present(),
            SeratoOverviewState::Invalid,
            SeratoOverviewState::Absent,
            SeratoOverviewState::Unsupported,
        ] {
            setup
                .cache
                .store_serato_overview(&path, &stale)
                .expect("seeds old outcome");
            setup
                .cache
                .with_connection(|connection| {
                    Ok(connection.execute("UPDATE serato_tag_cache SET parser_version = 2", [])?)
                })
                .expect("marks old duplicate-selection semantics");
            assert!(
                setup.cache.get_serato_overview(&path).is_none(),
                "stale version-two outcome"
            );
            setup
                .cache
                .store_serato_overview(&path, &refreshed)
                .expect("refreshes outcome");
            assert_eq!(setup.cache.get_serato_overview(&path), Some(refreshed.clone()));
        }
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
