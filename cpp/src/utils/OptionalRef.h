/// Defines optional, non-owning references for dependencies and lookups that may be absent.
/// Engaged references never extend the lifetime of the object they observe.

#pragma once

#include <functional>
#include <optional>

namespace audiobatch::utils
{
/// A nullable reference whose owner must outlive every use of the engaged value.
template<typename T>
using OptionalRef = std::optional<std::reference_wrapper<T>>;
}  // namespace audiobatch::utils
