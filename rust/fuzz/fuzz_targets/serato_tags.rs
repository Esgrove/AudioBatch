//! libFuzzer target for the Serato tag payload parsers and the Overview cache representation.
//!
//! The input layout is the same as the C++ `SeratoTagFuzzer`, so the two replay the same corpus.
//! The first byte selects the tag kind and the marker layout, and the remaining bytes are the payload.
//! A parsed Overview must serialize, deserialize, and serialize again to the same cache bytes.

#![no_main]

use libfuzzer_sys::fuzz_target;
use serato_tags::native::decode_tag_base64;
use serato_tags::{AnalysisVersion, AutoTags, BeatGrid, Markers, Overview};

/// Number of tag kinds the selector byte chooses from, numbered from one like the shared cache tag kinds.
const TAG_KIND_COUNT: u8 = 6;

/// Selector bit that chooses the legacy marker layout instead of Markers2.
const LEGACY_MARKERS_BIT: u8 = 0x80;

/// Selector bits that choose the tag kind.
const TAG_KIND_BITS: u8 = 0x7f;

/// The tag kinds of the shared `serato_tag_cache` table, in their stored order.
#[derive(Clone, Copy)]
enum TagKind {
    Overview = 1,
    Analysis = 2,
    Autotags = 3,
    BeatGrid = 4,
    Markers = 5,
    TrackInfo = 6,
}

impl TagKind {
    /// Returns the kind that a selector byte chooses, the same way the C++ harness does.
    const fn from_selector(selector: u8) -> Self {
        match (selector & TAG_KIND_BITS) % TAG_KIND_COUNT + 1 {
            1 => Self::Overview,
            2 => Self::Analysis,
            3 => Self::Autotags,
            4 => Self::BeatGrid,
            5 => Self::Markers,
            _ => Self::TrackInfo,
        }
    }
}

fuzz_target!(|input: &[u8]| {
    let Some((&selector, payload)) = input.split_first() else {
        return;
    };
    let legacy_markers = selector & LEGACY_MARKERS_BIT != 0;
    let _ = decode_tag_base64(payload);
    // The cache reader also sees bytes that did not come from the writer, such as a corrupted database.
    let _ = Overview::deserialize(payload);
    match TagKind::from_selector(selector) {
        TagKind::Overview => check_overview_round_trip(payload),
        TagKind::Analysis => {
            let _ = AnalysisVersion::parse(payload);
        }
        TagKind::Autotags => {
            let _ = AutoTags::parse(payload);
        }
        TagKind::BeatGrid => {
            let _ = BeatGrid::parse(payload).map(|grid| grid.bpm());
        }
        TagKind::Markers if legacy_markers => {
            let _ = Markers::parse_legacy(payload);
        }
        TagKind::Markers => {
            let _ = Markers::parse(payload);
        }
        // The track information comes from the standard tags, so its payload has no Serato parser.
        TagKind::TrackInfo => {}
    }
});

/// Requires a parsed Overview to keep its slices through the cache representation.
fn check_overview_round_trip(payload: &[u8]) {
    let Ok(overview) = Overview::parse(payload) else {
        return;
    };
    let serialized = overview.serialize();
    if serialized.is_empty() {
        return;
    }
    let restored = Overview::deserialize(&serialized);
    assert!(
        restored
            .as_ref()
            .is_some_and(|restored| restored.serialize() == serialized),
        "the Overview cache representation does not round trip"
    );
}
