/// Stable Serato tag identifiers and read outcomes for independent per-kind caching.
/// Transient failures are separate from stable negative results.

#pragma once

#include "SeratoOverview.h"

#include <juce_graphics/juce_graphics.h>

#include <algorithm>

namespace audiobatch::serato
{
/// Persisted kind identifiers remain stable when additional parsers are introduced.
/// Cache encoding writes explicit 32-bit integers independently of the in-memory enum size.
enum class TagKind : std::uint8_t {
    overview = 1,
    analysis = 2,
    autotags = 3,
    beatGrid = 4,
    markers = 5,
    trackInfo = 6,
};

/// Present carries owned data. Absent, unsupported and invalid are stable negative outcomes.
/// I/O failure and cancellation must not be cached as missing tags.
enum class ReadStatus : std::uint8_t {
    present = 1,
    absent = 2,
    unsupported = 3,
    invalid = 4,
    ioError = 5,
    cancelled = 6,
};

inline constexpr std::array allTagKinds {
    TagKind::overview,
    TagKind::analysis,
    TagKind::autotags,
    TagKind::beatGrid,
    TagKind::markers,
    TagKind::trackInfo,
};

/// Reports the independent numeric cache schema version for a supported kind.
[[nodiscard]] constexpr std::uint32_t parserVersion(TagKind kind)
{
    return kind == TagKind::overview ? overviewParserVersion : 1;
}

/// Rejects unrecognized persisted kind identifiers.
[[nodiscard]] constexpr bool supportsTagKind(TagKind kind)
{
    return std::ranges::any_of(allTagKinds, [kind](const auto supported) { return kind == supported; });
}

/// Serato's analysis engine version, not the musical key of the track.
struct Analysis {
    std::uint8_t majorVersion = 0;
    std::uint8_t minorVersion = 0;
};

/// Serato tempo and gain adjustments in beats per minute and decibels.
struct Autotags {
    double bpm = 0;
    double autoGain = 0;
    double gain = 0;
};

/// A grid anchor in seconds. The final anchor has BPM instead of beats until the next anchor.
struct BeatGridMarker {
    double positionSeconds = 0;
    std::uint32_t beatsUntilNext = 0;
    std::optional<double> bpm;
};

/// Owned ordered grid anchors, including the terminal BPM anchor.
struct BeatGrid {
    std::vector<BeatGridMarker> markers;
};

/// An owned hot cue with its original zero-based index and millisecond position.
struct CuePoint {
    std::uint32_t positionMilliseconds = 0;
    std::uint8_t index = 0;
    juce::Colour colour;
    juce::String name;
};

/// An owned saved loop and its lock state.
struct Loop {
    std::uint32_t startMilliseconds = 0;
    std::uint32_t endMilliseconds = 0;
    std::uint8_t index = 0;
    juce::Colour colour;
    bool locked = false;
    juce::String name;
};

/// Marker records retain absent colour and BPM lock separately from explicit values.
struct Markers {
    std::vector<CuePoint> cues;
    std::vector<Loop> loops;
    std::optional<juce::Colour> colour;
    std::optional<bool> bpmLocked;
};

/// Standard BPM and INITIALKEY metadata, independent of Serato's analysis version.
struct TrackInfo {
    std::optional<double> bpm;
    juce::String key;
};

/// Owned independent tag values for delivery from a worker to the preview UI.
struct SeratoData {
    std::optional<Overview> overview;
    std::optional<Analysis> analysis;
    std::optional<Autotags> autotags;
    std::optional<BeatGrid> beatGrid;
    std::optional<Markers> markers;
    std::optional<TrackInfo> trackInfo;
};

/// Reports one requested kind. Only present results contain the matching owned value.
struct TagReadResult {
    TagKind kind = TagKind::overview;
    ReadStatus status = ReadStatus::absent;
    std::optional<Overview> overview = std::nullopt;
    std::optional<Analysis> analysis = std::nullopt;
    std::optional<Autotags> autotags = std::nullopt;
    std::optional<BeatGrid> beatGrid = std::nullopt;
    std::optional<Markers> markers = std::nullopt;
    std::optional<TrackInfo> trackInfo = std::nullopt;
};
}  // namespace audiobatch::serato