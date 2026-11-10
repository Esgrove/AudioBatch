/// Implements the per-run free chain pool, scoped leases, and message-thread destruction of plugin instances.

#include "plugins/PluginRunChains.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
/// How long a worker waits for a plugin chain to be released before checking for cancellation again, in milliseconds.
constexpr int CHAIN_WAIT_INTERVAL_MILLISECONDS {100};
}  // namespace

/// Takes ownership of the run's chains, with all of them free when `usable` is true.
PluginRunChains::PluginRunChains(std::vector<PluginChainInstances> chainInstances, const bool usable) :
    chains(std::move(chainInstances))
{
    if (usable) {
        freeIndices.reserve(chains.size());

        for (int chainIndex = 0; std::cmp_less(chainIndex, chains.size()); ++chainIndex) {
            freeIndices.push_back(chainIndex);
        }
    }
}

/// Destroys the plugin instances on the message thread, as VST3 and Audio Unit hosting require.
/// When the last owner is a worker, the instances are handed to the message thread instead.
PluginRunChains::~PluginRunChains()
{
    if (chains.empty() || juce::MessageManager::existsAndIsCurrentThread()) {
        return;
    }

    auto chainsCaptured = std::make_shared<decltype(chains)>(std::move(chains));
    if (juce::MessageManager::callAsync([chainsCaptured] { chainsCaptured->clear(); })) {
        return;
    }

    // Posting fails once the app is quitting and the message loop no longer runs.
    // Destroying the instances here would release plugins off the message thread,
    // so they are left for the operating system to reclaim when the process exits.
    auto leaked = std::make_unique<std::shared_ptr<decltype(chains)>>(std::move(chainsCaptured));
    juce::ignoreUnused(leaked.release());
}

/// Takes a free chain from the pool, waiting for one while keepWaiting returns true.
/// Returns a lease that puts the chain back when it is destroyed,
/// or nothing when keepWaiting returned false before a chain became free.
std::optional<ChainLease> PluginRunChains::acquire(const std::function<bool()>& keepWaiting)
{
    while (keepWaiting()) {
        {
            const juce::ScopedLock scopedLock(lock);

            if (!freeIndices.empty()) {
                const auto chainIndex = freeIndices.back();
                freeIndices.pop_back();
                return std::optional<ChainLease>(std::in_place, *this, chainIndex);
            }
        }

        // More workers than chains is unusual, so polling while waiting costs little.
        available.wait(CHAIN_WAIT_INTERVAL_MILLISECONDS);
    }

    return std::nullopt;
}

/// Returns how many plugin chains the run owns, whether or not they are in use.
int PluginRunChains::getNumChains() const noexcept
{
    return static_cast<int>(chains.size());
}

/// Wakes a worker waiting for a chain, so it notices that its run was cancelled.
void PluginRunChains::wakeWaitingWorkers()
{
    available.signal();
}

/// Returns a chain to the free pool and wakes one worker waiting for a chain.
void PluginRunChains::release(const int chainIndex)
{
    {
        const juce::ScopedLock scopedLock(lock);
        freeIndices.push_back(chainIndex);
    }

    available.signal();
}

/// Takes over a chain that PluginRunChains::acquire removed from the free pool.
ChainLease::ChainLease(PluginRunChains& chains, const int index) noexcept : runChains(chains), chainIndex(index) {}

/// Returns the chain to the free pool of its run.
ChainLease::~ChainLease()
{
    runChains.release(chainIndex);
}

/// Returns a raw pointer view of the leased chain for the service call, which is valid while the lease lives.
std::vector<juce::AudioPluginInstance*> ChainLease::getChainView() const
{
    return runChains.chains[static_cast<std::size_t>(chainIndex)]
        | std::views::transform([](const auto& instance) { return instance.get(); }) | std::ranges::to<std::vector>();
}
}  // namespace audiobatch
