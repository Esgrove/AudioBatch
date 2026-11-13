//! Independent, bounded Rust waveform persistence with captured-source publication predicates.

use std::path::Path;
use std::sync::Arc;

use rusqlite::{Connection, OptionalExtension, params};

use super::{AnalysisCache, Result, path_key, read_nonnegative_integer};
use crate::record::{AnalysisRecord, file_identity};
use crate::waveform::{SERIALIZATION_VERSION, WaveformOverview};

impl AnalysisCache {
    /// Loads only a matching Rust preview, without hydrating measurements or the C++ waveform.
    /// Corrupt previews miss independently of the measurement cache.
    #[must_use]
    pub fn get_waveform(&self, path: &Path) -> Option<Arc<WaveformOverview>> {
        let identity = file_identity(path)?;
        self.with_connection(|connection| {
            Ok(connection
                .prepare_cached(
                    "SELECT preview.waveform_data, analysis.sample_rate, analysis.channels, analysis.length_in_samples
                 FROM rust_waveform_cache AS preview
                 JOIN file_analysis AS analysis ON analysis.file_path = preview.file_path
                 WHERE preview.file_path = ?1 AND preview.file_size = ?2 AND preview.modified_time_ms = ?3
                    AND preview.waveform_version = ?4
                    AND analysis.file_size = preview.file_size AND analysis.modified_time_ms = preview.modified_time_ms
                    AND analysis.sample_rate = preview.sample_rate AND analysis.channels = preview.num_channels
                    AND analysis.duration_seconds = preview.duration_seconds",
                )?
                .query_row(
                    params![
                        path_key(path),
                        i64::try_from(identity.file_size).unwrap_or(-1),
                        identity.modified_time_ms,
                        SERIALIZATION_VERSION
                    ],
                    |row| {
                        let waveform = row.get_ref(0)?.as_blob().ok().and_then(WaveformOverview::from_bytes);
                        let rate: u32 = row.get(1)?;
                        let channels: u32 = row.get(2)?;
                        let length = read_nonnegative_integer(row, 3)?;
                        Ok(waveform
                            .filter(|waveform| {
                                waveform.sample_rate() == rate
                                    && waveform.channels() == channels
                                    && waveform.length_in_frames() == length
                            })
                            .map(Arc::new))
                    },
                )
                .optional()?
                .flatten())
        })
        .ok()
        .flatten()
    }

    /// Publishes a native waveform only when the captured source and stored measurement geometry still match.
    /// This never writes gain, provenance, histories, or the C++ waveform columns.
    ///
    /// # Errors
    ///
    /// Returns an error when the database write fails.
    pub fn store_waveform(&self, record: &AnalysisRecord, waveform: &WaveformOverview) -> Result<bool> {
        if !record.matches_file_on_disk() {
            return Ok(false);
        }
        self.with_connection(|connection| Ok(store(connection, record, waveform)?))
    }
}

/// Publishes a bounded, valid waveform using the caller's connection or measurement transaction.
pub(super) fn store(
    connection: &Connection,
    record: &AnalysisRecord,
    waveform: &WaveformOverview,
) -> rusqlite::Result<bool> {
    if waveform.sample_rate() != record.sample_rate
        || waveform.channels() != record.channels
        || waveform.length_in_frames() != record.length_in_samples
    {
        return Ok(false);
    }
    let data = waveform.to_bytes();
    if WaveformOverview::from_bytes(&data).is_none() {
        return Ok(false);
    }
    Ok(connection
        .prepare_cached(
            "INSERT INTO rust_waveform_cache (file_path, file_size, modified_time_ms, sample_rate, num_channels,
            waveform_version, waveform_data, duration_seconds)
         SELECT ?1, ?2, ?3, ?4, ?5, ?7, ?8, ?9
         WHERE EXISTS (SELECT 1 FROM file_analysis WHERE file_path = ?1 AND file_size = ?2
            AND modified_time_ms = ?3 AND sample_rate = ?4 AND channels = ?5 AND length_in_samples = ?6
            AND status IN (1, 2))
         ON CONFLICT(file_path) DO UPDATE SET file_size = excluded.file_size,
            modified_time_ms = excluded.modified_time_ms, sample_rate = excluded.sample_rate,
            num_channels = excluded.num_channels,
            waveform_version = excluded.waveform_version, waveform_data = excluded.waveform_data,
            duration_seconds = excluded.duration_seconds",
        )?
        .execute(params![
            path_key(&record.path),
            i64::try_from(record.file_size).unwrap_or(-1),
            record.modified_time_ms,
            record.sample_rate,
            record.channels,
            i64::try_from(record.length_in_samples).unwrap_or(-1),
            SERIALIZATION_VERSION,
            data,
            record.duration_seconds
        ])?
        > 0)
}

/// Removes obsolete previews only when the caller's measurement identity won publication.
pub(super) fn prune_incompatible(connection: &Connection, record: &AnalysisRecord) -> rusqlite::Result<()> {
    connection
        .prepare_cached(
            "DELETE FROM rust_waveform_cache WHERE file_path = ?1
         AND (file_size != ?2 OR modified_time_ms != ?3 OR sample_rate != ?4 OR num_channels != ?5 OR duration_seconds != ?7)
         AND EXISTS (SELECT 1 FROM file_analysis WHERE file_path = ?1 AND file_size = ?2 AND modified_time_ms = ?3
            AND sample_rate = ?4 AND channels = ?5 AND length_in_samples = ?6)",
        )?
        .execute(params![
            path_key(&record.path),
            i64::try_from(record.file_size).unwrap_or(-1),
            record.modified_time_ms,
            record.sample_rate,
            record.channels,
            i64::try_from(record.length_in_samples).unwrap_or(-1),
            record.duration_seconds
        ])?;
    Ok(())
}
