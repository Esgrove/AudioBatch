//! Atomic additive migrations for shared measurements, spectral artifacts, and independent native previews.

use rusqlite::{Connection, TransactionBehavior};

use super::{Result, add_missing_columns, serato, spectrogram};

/// Shared columns absent from older C++ databases, added without dropping gains or tag results.
const ADDED_COLUMNS: &[(&str, &str)] = &[
    ("waveform_data", "BLOB"),
    ("waveform_version", "INTEGER NOT NULL DEFAULT 0"),
    ("true_peak_left", "REAL NOT NULL DEFAULT 0"),
    ("true_peak_right", "REAL NOT NULL DEFAULT 0"),
    ("overall_true_peak", "REAL NOT NULL DEFAULT 0"),
    ("max_short_term_lufs", "REAL NOT NULL DEFAULT -1000"),
    ("integrated_lufs", "REAL NOT NULL DEFAULT -1000"),
    ("custom_gain_db", "REAL NOT NULL DEFAULT 0"),
    ("has_custom_gain", "INTEGER NOT NULL DEFAULT 0"),
    ("short_term_loudness", "BLOB"),
    ("overload_history", "BLOB"),
    ("analysis_producer", "TEXT NOT NULL DEFAULT 'cpp'"),
    ("plr_db", "REAL"),
    ("psr_db", "REAL"),
    ("crest_factor_db", "REAL"),
    ("dc_offsets", "BLOB"),
    ("clip_counts", "BLOB"),
    ("stereo_correlation", "REAL"),
];

/// Measurement UPSERT, preserving the other application's compatible native waveform entirely inside SQLite.
pub(super) const STORE_ANALYSIS_SQL: &str = "INSERT INTO file_analysis (
    file_path, file_name, format_name, file_size, modified_time_ms, length_in_samples,
    duration_seconds, peak_left, peak_right, overall_peak, true_peak_left, true_peak_right,
    overall_true_peak, max_short_term_lufs, integrated_lufs, sample_rate, channels, bits_per_sample,
    status, error_message, short_term_loudness, analysis_version,
    updated_at_ms, custom_gain_db, has_custom_gain, overload_history, analysis_producer,
    plr_db, psr_db, crest_factor_db, dc_offsets, clip_counts, stereo_correlation
) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19,
          ?20, ?21, ?22, ?23, ?24, ?25, ?26, ?27, ?28, ?29, ?30, ?31, ?32, ?33)
ON CONFLICT(file_path) DO UPDATE SET
    file_name = excluded.file_name, format_name = excluded.format_name,
    file_size = excluded.file_size, modified_time_ms = excluded.modified_time_ms,
    length_in_samples = excluded.length_in_samples, duration_seconds = excluded.duration_seconds,
    peak_left = excluded.peak_left, peak_right = excluded.peak_right, overall_peak = excluded.overall_peak,
    true_peak_left = excluded.true_peak_left, true_peak_right = excluded.true_peak_right,
    overall_true_peak = excluded.overall_true_peak, max_short_term_lufs = excluded.max_short_term_lufs,
    integrated_lufs = excluded.integrated_lufs, sample_rate = excluded.sample_rate,
    plr_db = excluded.plr_db, psr_db = excluded.psr_db, crest_factor_db = excluded.crest_factor_db,
    dc_offsets = excluded.dc_offsets, clip_counts = excluded.clip_counts,
    stereo_correlation = excluded.stereo_correlation,
    channels = excluded.channels, bits_per_sample = excluded.bits_per_sample,
    status = excluded.status, error_message = excluded.error_message,
    short_term_loudness = excluded.short_term_loudness, overload_history = excluded.overload_history,
    analysis_version = excluded.analysis_version, analysis_producer = excluded.analysis_producer,
    updated_at_ms = excluded.updated_at_ms,
    custom_gain_db = CASE WHEN file_analysis.file_size = excluded.file_size
        AND file_analysis.modified_time_ms = excluded.modified_time_ms
        THEN file_analysis.custom_gain_db ELSE excluded.custom_gain_db END,
    has_custom_gain = CASE WHEN file_analysis.file_size = excluded.file_size
        AND file_analysis.modified_time_ms = excluded.modified_time_ms
        THEN file_analysis.has_custom_gain ELSE excluded.has_custom_gain END,
    waveform_data = CASE WHEN file_analysis.file_size = excluded.file_size
        AND file_analysis.modified_time_ms = excluded.modified_time_ms
        AND file_analysis.sample_rate = excluded.sample_rate AND file_analysis.channels = excluded.channels
        AND file_analysis.length_in_samples = excluded.length_in_samples
        THEN file_analysis.waveform_data ELSE NULL END,
    waveform_version = CASE WHEN file_analysis.file_size = excluded.file_size
        AND file_analysis.modified_time_ms = excluded.modified_time_ms
        AND file_analysis.sample_rate = excluded.sample_rate AND file_analysis.channels = excluded.channels
        AND file_analysis.length_in_samples = excluded.length_in_samples
        THEN file_analysis.waveform_version ELSE 0 END
WHERE NOT (excluded.status = 3 AND file_analysis.status IN (1, 2)
    AND file_analysis.analysis_version = excluded.analysis_version
    AND file_analysis.analysis_producer IN ('cpp', 'rust')
    AND file_analysis.file_size = excluded.file_size
    AND file_analysis.modified_time_ms = excluded.modified_time_ms)";

/// Creates shared and native cache layouts under one write lock, making simultaneous startup retryable.
/// Provenance-free legacy schemas are identified conservatively, and older measurements refill source statistics.
pub(super) fn initialize(connection: &mut Connection) -> Result<()> {
    let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
    let columns: Vec<(String, bool)> = transaction
        .prepare("PRAGMA table_info(file_analysis)")?
        .query_map([], |row| Ok((row.get(1)?, row.get::<_, i64>(3)? != 0)))?
        .collect::<rusqlite::Result<_>>()?;
    let known_cpp = columns.iter().any(|(name, _)| name == "has_custom_gain")
        && columns
            .iter()
            .any(|(name, nonnull)| name == "custom_gain_db" && *nonnull)
        && columns.iter().any(|(name, _)| name == "overload_history")
        && !columns.iter().any(|(name, _)| name == "analysis_producer");
    transaction.execute_batch(
        "CREATE TABLE IF NOT EXISTS file_analysis (
            file_path TEXT PRIMARY KEY, file_name TEXT NOT NULL, format_name TEXT,
            file_size INTEGER NOT NULL, modified_time_ms INTEGER NOT NULL,
            length_in_samples INTEGER NOT NULL, duration_seconds REAL NOT NULL,
            peak_left REAL NOT NULL, peak_right REAL NOT NULL, overall_peak REAL NOT NULL,
            true_peak_left REAL NOT NULL DEFAULT 0, true_peak_right REAL NOT NULL DEFAULT 0,
            overall_true_peak REAL NOT NULL DEFAULT 0,
            max_short_term_lufs REAL NOT NULL DEFAULT -1000, integrated_lufs REAL NOT NULL DEFAULT -1000,
            short_term_loudness BLOB, overload_history BLOB,
            sample_rate INTEGER NOT NULL, channels INTEGER NOT NULL, bits_per_sample INTEGER NOT NULL,
            custom_gain_db REAL NOT NULL DEFAULT 0, has_custom_gain INTEGER NOT NULL DEFAULT 0,
            status INTEGER NOT NULL, error_message TEXT, waveform_data BLOB,
            waveform_version INTEGER NOT NULL DEFAULT 0,
            analysis_version INTEGER NOT NULL, analysis_producer TEXT NOT NULL DEFAULT 'cpp',
            updated_at_ms INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS rust_waveform_cache (
            file_path TEXT PRIMARY KEY, file_size INTEGER NOT NULL, modified_time_ms INTEGER NOT NULL,
            sample_rate INTEGER NOT NULL, num_channels INTEGER NOT NULL,
            duration_seconds REAL NOT NULL, waveform_version INTEGER NOT NULL, waveform_data BLOB NOT NULL
        );",
    )?;
    if !columns.is_empty() && !columns.iter().any(|(name, _)| name == "analysis_producer") {
        transaction.execute(
            "UPDATE file_analysis SET analysis_version = 0 WHERE analysis_version != ?1",
            [if known_cpp { 12 } else { -1 }],
        )?;
    }
    add_missing_columns(&transaction, "file_analysis", ADDED_COLUMNS)?;
    // Older measurements lack required source statistics and must be refilled without deleting gains or previews.
    transaction.execute_batch(serato::SCHEMA)?;
    transaction.execute_batch(spectrogram::SCHEMA)?;
    // Native AAC reconstruction changes the old Rust MP4 previews, not source identity, gains, tags, or PCM previews.
    transaction.execute_batch(
        "DELETE FROM rust_waveform_cache WHERE file_path IN (
            SELECT file_path FROM file_analysis WHERE analysis_version < 15
                AND format_name = 'M4A file' AND bits_per_sample = 16);
         DELETE FROM spectrogram_cache WHERE producer = 'rust' AND file_path IN (
            SELECT file_path FROM file_analysis WHERE analysis_version < 15
                AND format_name = 'M4A file' AND bits_per_sample = 16);",
    )?;
    transaction.commit()?;
    Ok(())
}
