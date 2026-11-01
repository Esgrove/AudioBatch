//! File-backed metadata regressions with exact per-format cue, grid, colour, and overview expectations.

use audiobatch_core::cache::AnalysisCache;
use audiobatch_core::metadata::copy_metadata;
use audiobatch_core::serato_tags::colouring::{OverviewColour, column_colours, slice_colour};
use audiobatch_core::serato_tags::{BeatGridMarker, Markers, Overview, SeratoData};
use audiobatch_core::tags::{FileTags, SeratoOverviewState, read_display_tags, read_tags};
use base64::{Engine as _, engine::general_purpose::STANDARD};
use lofty::config::WriteOptions;
use lofty::ogg::tag::VorbisComments;
use lofty::prelude::TagExt;
use std::path::{Path, PathBuf};
use tempfile::TempDir;
use test_fixtures::{MetadataFixture, file_sha256, metadata_fixtures};

/// Writes duplicate native comments or raw ID3v2.3 objects without a tag library deduplicating frames.
fn write_serato_file(directory: &TempDir, native: bool, frames: &[(&str, &[u8])]) -> PathBuf {
    if native {
        let path = test_fixtures::copy_fixture(directory, "sine_stereo_44100_16.flac", None);
        let mut comments = VorbisComments::default();
        for (description, payload) in frames {
            let field = audiobatch_core::serato_tags::native::NATIVE_FIELDS
                .iter()
                .find(|(_, name)| name == description)
                .expect("native name")
                .0;
            let mut wrapper = b"application/octet-stream\0\0".to_vec();
            wrapper.extend_from_slice(description.as_bytes());
            wrapper.push(0);
            wrapper.extend_from_slice(payload);
            comments.push(field.to_string(), STANDARD.encode(wrapper));
        }
        comments
            .save_to_path(&path, WriteOptions::default())
            .expect("writes native duplicates");
        return path;
    }
    let path = directory.path().join("duplicates.mp3");
    let source = std::fs::read(test_fixtures::fixture("sine_stereo_44100_320k.mp3")).expect("MP3 fixture");
    let audio = if source.starts_with(b"ID3") {
        let size = source
            .get(6..10)
            .expect("complete fixture header")
            .iter()
            .fold(0_usize, |size, byte| (size << 7) | usize::from(*byte));
        source.get(10 + size..).expect("complete fixture tag")
    } else {
        &source
    };
    let mut body = Vec::new();
    for (description, payload) in frames {
        let mut object = b"\0application/octet-stream\0\0".to_vec();
        object.extend_from_slice(description.as_bytes());
        object.push(0);
        object.extend_from_slice(payload);
        body.extend_from_slice(b"GEOB");
        body.extend_from_slice(&u32::try_from(object.len()).expect("small frame").to_be_bytes());
        body.extend_from_slice(&[0, 0]);
        body.extend_from_slice(&object);
    }
    let length = u32::try_from(body.len()).expect("small ID3 tag");
    let mut bytes = b"ID3\x03\0\0".to_vec();
    for shift in [21, 14, 7, 0] {
        bytes.push(u8::try_from((length >> shift) & 127).expect("seven bits"));
    }
    bytes.extend_from_slice(&body);
    bytes.extend_from_slice(audio);
    std::fs::write(&path, bytes).expect("writes raw duplicate frames");
    path
}

/// Checks both readers and the persistent Overview outcome, including invalid and absent states.
fn check_duplicate_readers(path: &Path, expected: &SeratoData, cache: &AnalysisCache) {
    let full = read_tags(path).expect("full duplicate tags");
    let display = read_display_tags(path).expect("display duplicate tags");
    assert_eq!(full.serato.as_ref(), Some(expected), "{}: full", path.display());
    assert_eq!(display.serato, full.serato, "{}: display", path.display());
    let state = SeratoOverviewState::from_tags(path, &display);
    cache
        .store_serato_overview(path, &state)
        .expect("stores selection outcome");
    assert_eq!(cache.get_serato_overview(path).expect("cached outcome"), state);
}

#[test]
fn duplicate_selection_is_identical_in_native_and_id3_readers() {
    let cache_directory = tempfile::Builder::new()
        .prefix("metadata-cache-")
        .tempdir_in(env!("CARGO_MANIFEST_DIR"))
        .expect("repository cache directory");
    let cache = AnalysisCache::with_path(cache_directory.path().join("analysis.db"));
    let grid = [
        &[1, 0, 0, 0, 0, 1][..],
        &0.25_f32.to_be_bytes(),
        &127.12_f32.to_be_bytes(),
        &[0],
    ]
    .concat();
    let cases = [
        ("Serato Analysis", &[2, 1][..], &[3, 4][..]),
        (
            "Serato Autotags",
            b"\x01\x01127.12\0-3.257\x000.001\0",
            b"\x01\x01128.00\0-2.000\x001.000\0",
        ),
        ("Serato BeatGrid", grid.as_slice(), &[1, 0, 0, 0, 0, 0]),
        (
            "Serato Markers2",
            b"\x01\x01AQFDT0xPUgAAAAAEAASRAA==\0",
            b"\x01\x01AQEA\0",
        ),
        (
            "Serato Overview",
            &[1, 5, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2],
            &[1, 5, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3],
        ),
    ];
    for native in [false, true] {
        for (description, first, second) in cases {
            for payloads in [
                [b"invalid".as_slice(), first, second],
                [first, b"invalid".as_slice(), second],
                [first, second, b"invalid".as_slice()],
                [b"invalid".as_slice(), b"invalid".as_slice(), b"invalid".as_slice()],
            ] {
                let frames = payloads.map(|payload| (description, payload));
                let expected = SeratoData::parse(frames).expect("matching description");
                let directory = tempfile::Builder::new()
                    .prefix("metadata-tags-")
                    .tempdir_in(env!("CARGO_MANIFEST_DIR"))
                    .expect("repository tags directory");
                let path = write_serato_file(&directory, native, &frames);
                check_duplicate_readers(&path, &expected, &cache);
            }
        }
    }
}

#[test]
fn legacy_fallback_survives_native_and_id3_readers() {
    let cache_directory = tempfile::Builder::new()
        .prefix("metadata-cache-")
        .tempdir_in(env!("CARGO_MANIFEST_DIR"))
        .expect("repository cache directory");
    let cache = AnalysisCache::with_path(cache_directory.path().join("analysis.db"));
    let mut legacy = vec![2, 5, 0, 0, 0, 14];
    for slot in 0..14 {
        let mut record = [0; 22];
        record[0] = if slot == 0 { 0 } else { 127 };
        record[20] = if slot < 5 { 1 } else { 3 };
        record[1..5].copy_from_slice(&[0, 0, 3, 116]);
        record[16..20].copy_from_slice(&[0, 72, 104, 86]);
        legacy.extend_from_slice(&record);
    }
    legacy.extend_from_slice(&[7, 124, 0, 1]);
    for native in [false, true] {
        for frames in [
            vec![("Serato Markers_", legacy.as_slice())],
            vec![
                ("Serato Markers2", b"invalid".as_slice()),
                ("Serato Markers_", legacy.as_slice()),
            ],
            vec![
                ("Serato Markers_", legacy.as_slice()),
                ("Serato Markers2", b"\x01\x01AQEA\0".as_slice()),
            ],
            vec![
                ("Serato Markers2", b"\x01\x01AQEA\0".as_slice()),
                ("Serato Markers_", legacy.as_slice()),
            ],
        ] {
            let expected = SeratoData::parse(frames.iter().copied()).expect("marker selection");
            let directory = tempfile::Builder::new()
                .prefix("metadata-legacy-")
                .tempdir_in(env!("CARGO_MANIFEST_DIR"))
                .expect("repository legacy directory");
            let path = write_serato_file(&directory, native, &frames);
            check_duplicate_readers(&path, &expected, &cache);
            if frames.iter().all(|(_, bytes)| *bytes != b"\x01\x01AQEA\0") {
                let cue = expected.cues().next().expect("legacy cue");
                assert_eq!(cue.position, 500);
                assert_eq!(cue.color.to_rgb_u32(), 0x12_34_56);
                assert_eq!(cue.name, "");
                assert_eq!(
                    expected.track_color().expect("legacy track colour").to_rgb_u32(),
                    0xff_00_01
                );
            } else {
                assert_eq!(expected.markers, Vec::<Markers>::new(), "valid empty Markers2 wins");
            }
            assert_eq!(expected.errors, Vec::<String>::new());
        }
    }
}

/// Checks one labelled fixture through both production tag-reading modes and the overview cache.
fn check_fixture(name: &str) {
    let case = metadata_fixtures()
        .into_iter()
        .find(|case| case.file == name)
        .expect("named metadata expectation");
    let path = case.path();
    let full = read_tags(&path).expect("reads full real metadata");
    let display = read_display_tags(&path).expect("reads display real metadata");
    check_ordinary_tags(&full, &case);
    check_ordinary_tags(&display, &case);
    assert_eq!(full.serato, display.serato, "{name}: full/display Serato data");
    let serato = full.serato.as_ref().expect("real file has Serato tags");
    check_serato(serato, &case);
    check_overview(serato.overview.as_ref().expect("real overview"), &case);
    let state = SeratoOverviewState::from_tags(&path, &display);
    let overview = state.overview().expect("valid real overview");
    let decoded = Overview::deserialize(&overview.serialize()).expect("decodes real overview cache blob");
    check_overview(&decoded, &case);
    let directory = tempfile::tempdir().expect("temporary cache");
    let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
    cache
        .store_serato_overview(&path, &state)
        .expect("stores real overview");
    let cached = cache.get_serato_overview(&path).expect("reads real overview cache");
    assert_eq!(cached, state, "{name}: overview cache");
    check_overview(cached.overview().expect("cached overview is present").as_ref(), &case);
    let destination = directory.path().join("copied.aif");
    std::fs::copy(
        test_fixtures::fixtures_directory().join("sine_stereo_44100_16.aif"),
        &destination,
    )
    .expect("temporary AIFF destination");
    copy_metadata(&path, &destination).expect("copies real Serato metadata");
    let copied = read_tags(&destination).expect("reads copied metadata");
    check_ordinary_tags(&copied, &case);
    let copied_serato = copied.serato.as_ref().expect("copied Serato tags");
    check_serato(copied_serato, &case);
    assert_eq!(
        copied_serato.overview, None,
        "{name}: stale overview is deliberately removed"
    );
    assert_eq!(
        file_sha256(&path).expect("hashes fixture after reads"),
        case.sha256,
        "{name}"
    );
}

/// Checks ordinary tags independently of Serato's analysed BPM and analysis-version bytes.
fn check_ordinary_tags(tags: &FileTags, case: &MetadataFixture) {
    let name = &case.file;
    assert_eq!(tags.title.as_deref(), Some(case.title.as_str()), "{name}: title");
    assert_eq!(tags.artist.as_deref(), Some(case.artist.as_str()), "{name}: artist");
    assert_eq!(tags.album.as_deref(), Some(case.album.as_str()), "{name}: album");
    assert_eq!(tags.genre.as_deref(), Some(case.genre.as_str()), "{name}: genre");
    assert_eq!(tags.comment.as_deref(), Some(case.comment.as_str()), "{name}: comment");
    assert_eq!(tags.year, case.year, "{name}: year");
    assert_eq!(tags.bpm.as_deref(), Some(case.bpm.as_str()), "{name}: ordinary BPM");
    assert_eq!(tags.key.as_deref(), Some(case.key.as_str()), "{name}: musical key");
}

/// Checks all exposed Serato kinds and every cue and saved loop in original tag order.
fn check_serato(data: &SeratoData, case: &MetadataFixture) {
    let name = &case.file;
    assert!(data.errors.is_empty(), "{name}: {:?}", data.errors);
    assert_eq!(
        data.analysis
            .as_ref()
            .map(|version| [version.major_version, version.minor_version]),
        case.analysis,
        "{name}: analysis version"
    );
    let autotags = data.autotags.as_ref().expect("real autotags");
    for (actual, expected) in [autotags.bpm, autotags.auto_gain, autotags.gain]
        .into_iter()
        .zip(case.autotags)
    {
        assert!(
            actual.to_bits() == expected.to_bits(),
            "{name}: autotag {actual} != {expected}"
        );
    }
    assert_eq!(
        data.bpm().map(f64::to_bits),
        Some(case.autotags[0].to_bits()),
        "{name}: final analysed BPM"
    );
    assert_eq!(
        data.track_color().map(audiobatch_core::serato_tags::Color::to_rgb_u32),
        Some(case.track_rgb),
        "{name}"
    );
    let locks: Vec<_> = data
        .markers
        .iter()
        .filter_map(|marker| match marker {
            Markers::BpmLock(lock) => Some(lock.enabled),
            _ => None,
        })
        .collect();
    assert_eq!(locks, [case.bpm_locked], "{name}: explicit BPM lock");
    let cues: Vec<_> = data.cues().collect();
    assert_eq!(cues.len(), case.cues.len(), "{name}: cue count");
    for (actual, expected) in cues.iter().zip(&case.cues) {
        assert_eq!(actual.index, expected.index, "{name}: cue slot");
        assert_eq!(actual.position, expected.position_ms, "{name}: cue position");
        assert_eq!(actual.color.to_rgb_u32(), expected.rgb, "{name}: cue colour");
        assert_eq!(actual.name, expected.name, "{name}: exact source cue label");
    }
    let loops: Vec<_> = data
        .markers
        .iter()
        .filter_map(|marker| match marker {
            Markers::Loop(saved_loop) => Some(saved_loop),
            _ => None,
        })
        .collect();
    assert_eq!(loops.len(), case.loops.len(), "{name}: loop count");
    for (actual, expected) in loops.iter().zip(&case.loops) {
        assert_eq!(actual.index, expected.index, "{name}: loop slot");
        assert_eq!(
            i64::from(actual.start_position),
            i64::from(expected.start_ms),
            "{name}: loop start"
        );
        assert_eq!(
            i64::from(actual.end_position),
            i64::from(expected.end_ms),
            "{name}: loop end"
        );
        assert_eq!(actual.color.to_rgb_u32(), expected.rgb, "{name}: loop colour");
        assert_eq!(actual.locked, expected.locked, "{name}: loop lock");
        assert_eq!(actual.name, expected.name, "{name}: loop label");
    }
    assert_eq!(
        data.markers.len(),
        2 + case.cues.len() + case.loops.len(),
        "{name}: marker records"
    );
    check_beatgrid(data, case);
}

/// Checks the dedicated grid, without treating similarly named custom text tags as grid data.
fn check_beatgrid(data: &SeratoData, case: &MetadataFixture) {
    let name = &case.file;
    assert_eq!(
        data.beatgrid.is_some(),
        case.beatgrid.is_some(),
        "{name}: grid presence"
    );
    if let Some(expected) = &case.beatgrid {
        let actual = data.beatgrid.as_ref().expect("dedicated grid");
        assert_eq!(
            actual.marker_count as usize,
            expected.len(),
            "{name}: grid header count"
        );
        assert_eq!(actual.markers.len(), expected.len(), "{name}: grid anchors");
        for (actual, expected) in actual.markers.iter().zip(expected) {
            match actual {
                BeatGridMarker::Terminal { position, bpm } => {
                    assert_eq!(
                        f64::from(*position),
                        expected.position_seconds,
                        "{name}: terminal position"
                    );
                    assert_eq!(Some(f64::from(*bpm)), expected.bpm, "{name}: terminal BPM");
                    assert_eq!(expected.beats_until_next, 0);
                }
                BeatGridMarker::NonTerminal {
                    position,
                    beats_till_next,
                } => {
                    assert_eq!(
                        f64::from(*position),
                        expected.position_seconds,
                        "{name}: anchor position"
                    );
                    assert_eq!(*beats_till_next, expected.beats_until_next, "{name}: beat count");
                    assert_eq!(expected.bpm, None);
                }
            }
        }
    }
}

/// Verifies every overview cell with a checksum, plus raw sampled slices and independently decoded colours.
fn check_overview(overview: &Overview, case: &MetadataFixture) {
    let name = &case.file;
    let slices = overview.slices();
    assert_eq!(slices.len(), case.overview.slices, "{name}: overview slices");
    let checksum = slices
        .iter()
        .flatten()
        .fold(14_695_981_039_346_656_037_u64, |checksum, byte| {
            (checksum ^ u64::from(*byte)).wrapping_mul(1_099_511_628_211)
        });
    assert_eq!(
        format!("{checksum:016x}"),
        case.overview.fnv1a64,
        "{name}: every overview byte"
    );
    for sample in &case.overview.samples {
        let actual = *slices.get(sample.index).expect("sample lies within real raster");
        assert_eq!(actual, sample.cells, "{name}: overview slice {}", sample.index);
        let rgb = slice_colour(&actual).map(OverviewColour::to_rgb_u32);
        assert_eq!(rgb, sample.rgb, "{name}: exact final RGB at slice {}", sample.index);
        assert_eq!(rgb.is_some(), sample.rgb.is_some(), "{name}: foreground presence");
        let isolated = Overview::from_slices(vec![actual]);
        let columns = column_colours(&isolated, 0.0, 1.0, 1);
        assert_eq!(
            columns
                .into_iter()
                .map(|colour| colour.map(OverviewColour::to_rgb_u32))
                .collect::<Vec<_>>(),
            [sample.rgb],
            "{name}: final waveform column RGB at slice {}",
            sample.index
        );
    }
}

#[test]
fn dmt_aiff_metadata() {
    check_fixture("dmt_drum_127_26A.aif");
}
#[test]
fn dmt_flac_metadata() {
    check_fixture("dmt_drum_127_26A.flac");
}
#[test]
fn dmt_mp3_metadata() {
    check_fixture("dmt_drum_127_26A.mp3");
}
#[test]
fn rk6_aiff_metadata() {
    check_fixture("RK6_beat_loop_01_126_c.aif");
}
#[test]
fn rk6_flac_metadata() {
    check_fixture("RK6_beat_loop_01_126_c.flac");
}
#[test]
fn rk6_mp3_metadata() {
    check_fixture("RK6_beat_loop_01_126_c.mp3");
}
#[test]
fn synth_aiff_metadata() {
    check_fixture("shs_ot_synth_127_mallet_F.aif");
}
#[test]
fn synth_flac_metadata() {
    check_fixture("shs_ot_synth_127_mallet_F.flac");
}
#[test]
fn synth_mp3_metadata() {
    check_fixture("shs_ot_synth_127_mallet_F.mp3");
}
