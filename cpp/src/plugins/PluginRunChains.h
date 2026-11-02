/// Per-run ownership and leasing of pre-instantiated plugin chains.
/// Jobs share PluginRunChains so cancellation never returns a chain to a later run's pool.

#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace audiobatch
{
/// One pre-instantiated chain of plugin instances, in processing order.
using PluginChainInstances = std::vector<std::unique_ptr<juce::AudioPluginInstance>>;

class ChainLease;

/// The plugin chains of one run and the pool of the chains no worker is using.
/// Every job of the run shares ownership, so a job that outlives a cancellation timeout keeps its chain alive
/// and can never return a chain to the free pool of a later run.
class PluginRunChains
{
public:
    PluginRunChains(std::vector<PluginChainInstances> chainInstances, bool usable);

    ~PluginRunChains();

    PluginRunChains(const PluginRunChains&) = delete;
    PluginRunChains& operator=(const PluginRunChains&) = delete;
    PluginRunChains(PluginRunChains&&) = delete;
    PluginRunChains& operator=(PluginRunChains&&) = delete;

    [[nodiscard]] std::optional<ChainLease> acquire(const std::function<bool()>& keepWaiting);

    [[nodiscard]] int getNumChains() const noexcept;

    void wakeWaitingWorkers();

private:
    friend class ChainLease;

    void release(int chainIndex);

    /// The plugin instances of every chain, owned for the whole run.
    /// Keep them before synchronization state so they are destroyed last after the destructor's thread handoff.
    std::vector<PluginChainInstances> chains;
    /// Guards freeIndices.
    juce::CriticalSection lock;
    /// Indices of the chains no worker is using.
    std::vector<int> freeIndices;
    /// Auto-reset event, signaled when a chain goes back to the pool and on cancellation.
    juce::WaitableEvent available {false};
};

/// A chain a worker took from the free pool of a run, which goes back to the pool when the lease is destroyed.
/// The run must outlive the lease.
class ChainLease
{
public:
    ChainLease(PluginRunChains& chains, int index) noexcept;

    ~ChainLease();

    ChainLease(const ChainLease&) = delete;
    ChainLease& operator=(const ChainLease&) = delete;
    ChainLease(ChainLease&&) = delete;
    ChainLease& operator=(ChainLease&&) = delete;

    [[nodiscard]] std::vector<juce::AudioPluginInstance*> getChainView() const;

private:
    /// The run whose pool the chain goes back to.
    PluginRunChains& runChains;
    /// Index of the leased chain in the run.
    int chainIndex;
};
}  // namespace audiobatch
