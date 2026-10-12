/// Stable Serato tag identifiers and read outcomes for independent per-kind caching.
/// Only Overview is implemented. Transient failures are separate from stable negative results.

#pragma once

#include "SeratoOverview.h"

namespace audiobatch::serato
{
/// Persisted kind identifiers remain stable when additional parsers are introduced.
enum class TagKind : std::uint32_t { overview = 1 };

/// Present carries owned data. Absent, unsupported and invalid are stable negative outcomes.
/// I/O failure and cancellation must not be cached as missing tags.
enum class ReadStatus : std::uint8_t {
    present = 1,
    absent = 2,
    unsupported = 3,
    invalid = 4,
    ioError = 5,
    cancelled = 6
};

/// Reports one requested kind. Only present results contain an Overview.
struct TagReadResult {
    TagKind kind = TagKind::overview;
    ReadStatus status = ReadStatus::absent;
    std::optional<Overview> overview;
};
}  // namespace audiobatch::serato