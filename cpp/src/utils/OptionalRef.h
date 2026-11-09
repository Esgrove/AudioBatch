/// Defines optional, non-owning references for dependencies and lookups that may be absent.
/// Engaged references never extend the lifetime of the object they observe.

#pragma once

#include <functional>
#include <optional>

namespace audiobatch::utils
{
/// A nullable, non-owning reference whose owner must outlive every use of the engaged value.
///
/// std::optional cannot hold T& directly, so reference_wrapper stores a copyable, rebindable reference instead.
/// std::nullopt represents absence. When engaged, use value->get() to access the original object without copying it.
template<typename T>
using OptionalRef = std::optional<std::reference_wrapper<T>>;
}  // namespace audiobatch::utils
