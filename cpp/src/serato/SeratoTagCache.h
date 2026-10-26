/// Serato tag results kept in the analysis cache database.
/// Declares getCachedTag and storeCachedTag, which keep each tag kind of a file with its own parser version,
/// so the preview can show the Serato data of an unchanged file without reading its tags again.
/// The table lives in the AnalysisCache database, which also removes the rows of changed and missing files.

#pragma once

#include "analysis/AnalysisCache.h"
#include "serato/SeratoTagTypes.h"
#include "utils/FileUtils.h"

#include <optional>

namespace audiobatch::serato
{
[[nodiscard]] std::optional<TagReadResult> getCachedTag(
    AnalysisCache& cache,
    const utils::FileIdentity& identity,
    TagKind kind = TagKind::overview
);

[[nodiscard]] bool storeCachedTag(
    AnalysisCache& cache,
    const utils::FileIdentity& identity,
    const TagReadResult& result
);
}  // namespace audiobatch::serato
