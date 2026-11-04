//! Independent, bounded spectral artifacts shared by both applications.
//!
//! Reads and writes require current successful measurement geometry without hydrating level histories.
//! Artifact publication checks the captured source around a short transaction and never updates measurements or gains.

use std::path::Path;
use std::sync::Arc;

use rusqlite::{Connection, OptionalExtension, TransactionBehavior, params};

use super::{ANALYSIS_VERSION, AnalysisCache, Result, path_key, read_nonnegative_integer};
use crate::output_file::SourceIdentity;
use crate::record::{AnalysisRecord, file_identity};
use crate::spectrogram::{ALGORITHM_VERSION, SERIALIZATION_VERSION, SpectrogramData};

/// Largest intensity grid plus a bounded allowance for its independently versioned header.
const MAXIMUM_SPECTROGRAM_BYTES: usize = 2 * 1024 * 1024 + 256;
/// SQLite's signed integer representation of the bounded payload limit.
const MAXIMUM_SPECTROGRAM_SQL_BYTES: i64 = 2 * 1024 * 1024 + 256;

/// Shared spectral table, independent of the measurement contract and native waveform layouts.
pub(super) const SCHEMA: &str = "CREATE TABLE IF NOT EXISTS spectrogram_cache (
    file_path TEXT NOT NULL,
    algorithm_version INTEGER NOT NULL,
    serialization_version INTEGER NOT NULL,
    file_size INTEGER NOT NULL,
    modified_time_ms INTEGER NOT NULL,
    sample_rate INTEGER NOT NULL,
    num_channels INTEGER NOT NULL,
    length_in_samples INTEGER NOT NULL,
    producer TEXT NOT NULL,
    spectrogram_data BLOB NOT NULL,
    PRIMARY KEY (file_path, algorithm_version, serialization_version)
);";

/// Excludes oversized blobs in SQLite before the row reader requests their bytes.
const LOOKUP_SQL: &str = "SELECT s.spectrogram_data, s.sample_rate, s.num_channels, s.length_in_samples
    FROM spectrogram_cache AS s JOIN file_analysis AS a ON a.file_path = s.file_path
    WHERE s.file_path = ?1 AND s.algorithm_version = ?2 AND s.serialization_version = ?3
        AND typeof(s.algorithm_version) = 'integer' AND typeof(s.serialization_version) = 'integer'
        AND typeof(s.file_size) = 'integer' AND typeof(s.modified_time_ms) = 'integer'
        AND typeof(s.sample_rate) = 'integer' AND typeof(s.num_channels) = 'integer'
        AND typeof(s.length_in_samples) = 'integer' AND s.producer IN ('cpp', 'rust')
        AND s.file_size = ?4 AND s.modified_time_ms = ?5
        AND typeof(s.spectrogram_data) = 'blob' AND length(s.spectrogram_data) BETWEEN 1 AND ?6
        AND typeof(a.file_size) = 'integer' AND typeof(a.modified_time_ms) = 'integer'
        AND typeof(a.sample_rate) = 'integer' AND typeof(a.channels) = 'integer'
        AND typeof(a.length_in_samples) = 'integer' AND typeof(a.analysis_version) = 'integer'
        AND typeof(a.status) = 'integer' AND a.analysis_version = ?7 AND a.status IN (1, 2)
        AND a.analysis_producer IN ('cpp', 'rust')
        AND a.file_size = s.file_size AND a.modified_time_ms = s.modified_time_ms
        AND a.sample_rate = s.sample_rate AND a.channels = s.num_channels
        AND a.length_in_samples = s.length_in_samples";

/// Publishes only the current artifact revision against matching successful measurement geometry.
const STORE_SQL: &str = "INSERT INTO spectrogram_cache (
        file_path, algorithm_version, serialization_version, file_size, modified_time_ms,
        sample_rate, num_channels, length_in_samples, producer, spectrogram_data
    ) SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 'rust', ?9
    WHERE EXISTS (SELECT 1 FROM file_analysis AS a WHERE a.file_path = ?1
        AND typeof(a.file_size) = 'integer' AND typeof(a.modified_time_ms) = 'integer'
        AND typeof(a.sample_rate) = 'integer' AND typeof(a.channels) = 'integer'
        AND typeof(a.length_in_samples) = 'integer' AND typeof(a.analysis_version) = 'integer'
        AND typeof(a.status) = 'integer' AND a.analysis_version = ?10 AND a.status IN (1, 2)
        AND a.analysis_producer IN ('cpp', 'rust')
        AND a.file_size = ?4 AND a.modified_time_ms = ?5
        AND a.sample_rate = ?6 AND a.channels = ?7 AND a.length_in_samples = ?8)
    ON CONFLICT(file_path, algorithm_version, serialization_version) DO UPDATE SET
        file_size = excluded.file_size, modified_time_ms = excluded.modified_time_ms,
        sample_rate = excluded.sample_rate, num_channels = excluded.num_channels,
        length_in_samples = excluded.length_in_samples, producer = excluded.producer,
        spectrogram_data = excluded.spectrogram_data";

impl AnalysisCache {
    /// Loads only the current spectral artifact, independently of measurement histories and native waveforms.
    /// Stale identities, unsupported measurement contracts, and corrupt optional data are artifact misses.
    #[must_use]
    pub fn get_spectrogram(&self, path: &Path) -> Option<Arc<SpectrogramData>> {
        let identity = SourceIdentity::capture(path).ok()?;
        let (file_size, modified_time_ms) = file_identity(path)?;
        let file_size = i64::try_from(file_size).ok()?;
        let result = self
            .with_connection(|connection| {
                Ok(connection
                    .prepare_cached(LOOKUP_SQL)?
                    .query_row(
                        params![
                            path_key(path),
                            ALGORITHM_VERSION,
                            SERIALIZATION_VERSION,
                            file_size,
                            modified_time_ms,
                            MAXIMUM_SPECTROGRAM_SQL_BYTES,
                            ANALYSIS_VERSION,
                        ],
                        |row| {
                            let data = row.get_ref(0)?.as_blob().ok().and_then(SpectrogramData::from_bytes);
                            let sample_rate = row.get(1)?;
                            let channels = row.get(2)?;
                            let length = read_nonnegative_integer(row, 3)?;
                            Ok(data
                                .filter(|data| data.matches_stream(sample_rate, length, channels))
                                .map(Arc::new))
                        },
                    )
                    .optional()?
                    .flatten())
            })
            .ok()
            .flatten();
        identity.validate(path).ok()?;
        result
    }

    /// Publishes a spectral artifact without modifying gains, measurement provenance, or required histories.
    /// Missing or changed sources and incompatible successful measurement geometry return false.
    /// The final source check remains inside the short transaction so stale publication rolls back.
    ///
    /// # Errors
    ///
    /// Returns an error when the optional artifact's database transaction fails.
    pub fn store_spectrogram(&self, record: &AnalysisRecord, data: &SpectrogramData) -> Result<bool> {
        if !record.path.is_absolute() || !record.matches_file_on_disk() {
            return Ok(false);
        }
        let Ok(identity) = SourceIdentity::capture(&record.path) else {
            return Ok(false);
        };
        let Some(bytes) = encoded_artifact(record, data) else {
            return Ok(false);
        };
        self.with_connection(|connection| {
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            if !record.matches_file_on_disk() || identity.validate(&record.path).is_err() {
                return Ok(false);
            }
            let stored = store_encoded(&transaction, record, data, &bytes)?;
            if !stored || !record.matches_file_on_disk() || identity.validate(&record.path).is_err() {
                return Ok(false);
            }
            transaction.commit()?;
            Ok(true)
        })
    }
}

/// Publishes a valid artifact using a caller-owned connection or short transaction.
/// The caller must protect publication with source checks and roll back if its final identity check fails.
#[expect(
    dead_code,
    reason = "Reserved for workers that already own a short artifact transaction"
)]
pub(super) fn store(
    connection: &Connection,
    record: &AnalysisRecord,
    data: &SpectrogramData,
) -> rusqlite::Result<bool> {
    let Some(bytes) = encoded_artifact(record, data) else {
        return Ok(false);
    };
    store_encoded(connection, record, data, &bytes)
}

/// Removes incompatible revisions only while the just-published measurement identity and geometry still win.
/// Callers run this best effort after committing required measurements so cleanup cannot roll them back.
pub(super) fn prune_incompatible(connection: &Connection, record: &AnalysisRecord) -> rusqlite::Result<()> {
    connection.execute(
        "DELETE FROM spectrogram_cache WHERE file_path = ?1
            AND (file_size != ?2 OR modified_time_ms != ?3 OR sample_rate != ?4
                OR num_channels != ?5 OR length_in_samples != ?6)
            AND EXISTS (SELECT 1 FROM file_analysis AS a WHERE a.file_path = ?1
                AND a.file_size = ?2 AND a.modified_time_ms = ?3 AND a.sample_rate = ?4
                AND a.channels = ?5 AND a.length_in_samples = ?6)",
        params![
            path_key(&record.path),
            i64::try_from(record.file_size).unwrap_or(-1),
            record.modified_time_ms,
            record.sample_rate,
            record.channels,
            i64::try_from(record.length_in_samples).unwrap_or(-1),
        ],
    )?;
    Ok(())
}

/// Serializes valid stream-matching data before taking the database write lock.
fn encoded_artifact(record: &AnalysisRecord, data: &SpectrogramData) -> Option<Vec<u8>> {
    if !data.matches_stream(record.sample_rate, record.length_in_samples, record.channels) {
        return None;
    }
    let bytes = data.to_bytes();
    (!bytes.is_empty() && bytes.len() <= MAXIMUM_SPECTROGRAM_BYTES).then_some(bytes)
}

/// Stores a prepared artifact without reading or rewriting any other cache payload.
fn store_encoded(
    connection: &Connection,
    record: &AnalysisRecord,
    data: &SpectrogramData,
    bytes: &[u8],
) -> rusqlite::Result<bool> {
    Ok(connection.execute(
        STORE_SQL,
        params![
            path_key(&record.path),
            ALGORITHM_VERSION,
            SERIALIZATION_VERSION,
            i64::try_from(record.file_size).unwrap_or(-1),
            record.modified_time_ms,
            data.sample_rate(),
            data.channels(),
            i64::try_from(data.length_in_frames()).unwrap_or(-1),
            bytes,
            ANALYSIS_VERSION,
        ],
    )? > 0)
}
