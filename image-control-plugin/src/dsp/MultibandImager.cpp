/// Implements MultibandImager and its KernelDesignThread.
/// The audio callback buffers the input into partitions, delays it by the kernel half length,
/// runs the lowpass kernels on the mid and side of each partition, and mixes the bands with smoothed gains and pans.
/// New crossovers are designed into a spare kernel slot off the audio thread,
/// and the audio thread switches to that slot with a crossfade over one partition.

#include "dsp/MultibandImager.h"

#include "dsp/BandCorrelation.h"
#include "dsp/CrossoverDesign.h"
#include "dsp/PartitionedConvolver.h"
#include "Parameters.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// How often the design thread checks for moved crossovers, in milliseconds.
constexpr int DESIGN_POLL_MILLISECONDS {20};
/// First quadrature lowpass in the shared kernel slot, after the ordinary lowpasses.
constexpr std::size_t QUADRATURE_FIRST_KERNEL {CROSSOVER_COUNT};
/// Full-band quadrature reference, after the quadrature lowpasses.
constexpr std::size_t QUADRATURE_FULL_KERNEL {CROSSOVER_COUNT * 2};
/// Designs ordinary and quadrature kernels together for one coherent crossover publication.
KernelSpectra designKernelSpectra(const Crossovers& crossovers, const double sampleRate, const int halfLength)
{
    std::vector<std::vector<float>> kernels;
    kernels.reserve(QUADRATURE_FULL_KERNEL + 1);
    for (const float crossover : crossovers) {
        kernels.push_back(designLowpassKernel(static_cast<double>(crossover), sampleRate, halfLength));
    }
    for (const float crossover : crossovers) {
        kernels.push_back(designQuadratureKernel(sampleRate, halfLength, static_cast<double>(crossover)));
    }
    kernels.push_back(designQuadratureKernel(sampleRate, halfLength));
    return {kernels, PARTITION_SIZE};
}

/// Returns whether a smoothed gain has settled at unity.
/// The bit-exact pass-through only needs the gain to have stopped gliding at its target:
/// the gain parameters snap targets near 0 dB to exactly 1, and the neutral path copies the delayed input
/// instead of multiplying by the gain, so this tolerance never reaches the output.
bool isSettledAtUnity(const SmoothedGain& gain)
{
    return !gain.isSmoothing() && juce::approximatelyEqual(gain.getCurrentValue(), 1.0F);
}

/// Returns whether a smoothed pan has settled exactly at the centre.
/// The pan parameters snap targets near the centre to exactly 0,
/// so a settled pan is either exactly centred or deliberately off-centre.
bool isSettledAtCentre(const SmoothedGain& pan)
{
    return !pan.isSmoothing() && juce::exactlyEqual(pan.getCurrentValue(), 0.0F);
}

/// Returns one band of one sample of a signal.
/// The bands are the lowest lowpass, the differences between neighbouring lowpasses,
/// and the delayed signal minus the highest lowpass.
float bandSample(
    const std::array<std::vector<float>, CROSSOVER_COUNT>& lowpassed,
    const std::size_t band,
    const std::size_t sample,
    const float delayed
)
{
    const float upper = band < CROSSOVER_COUNT ? lowpassed[band][sample] : delayed;
    const float lower = band > 0 ? lowpassed[band - 1][sample] : 0.0F;
    return upper - lower;
}

/// Sums the bands of one sample of a signal with their gains.
/// The bands are the lowest lowpass, the differences between neighbouring lowpasses,
/// and the delayed signal minus the highest lowpass.
float sumBands(
    const BandGains& gains,
    const std::array<std::vector<float>, CROSSOVER_COUNT>& lowpassed,
    const std::size_t sample,
    const float delayed
)
{
    const float low = lowpassed[0][sample];
    const float lowMid = lowpassed[1][sample];
    const float highMid = lowpassed[2][sample];
    return gains[0] * low + gains[1] * (lowMid - low) + gains[2] * (highMid - lowMid) + gains[3] * (delayed - highMid);
}

/// Returns the recovered Mid sum only when the quadrature outputs were computed for this partition.
float recoveredMidSum(
    const bool active,
    const BandGains& gains,
    const std::array<std::vector<float>, CROSSOVER_COUNT>& lowpassed,
    const std::span<const float> fullband,
    const std::size_t sample
)
{
    return active ? sumBands(gains, lowpassed, sample, fullband[sample]) : 0.0F;
}

/// Returns one recovered band's Mid contribution, without observing inactive scratch outputs.
float recoveredBandSample(
    const bool active,
    const float gain,
    const std::array<std::vector<float>, CROSSOVER_COUNT>& lowpassed,
    const std::span<const float> fullband,
    const std::size_t band,
    const std::size_t sample
)
{
    return active ? gain * bandSample(lowpassed, band, sample, fullband[sample]) : 0.0F;
}

/// Returns the samples of one channel of a buffer.
std::span<float> channelSamples(juce::AudioBuffer<float>& buffer, const int channel)
{
    return {buffer.getWritePointer(channel), static_cast<std::size_t>(buffer.getNumSamples())};
}
}  // namespace

/// A background thread that calls its work function at a fixed interval until it is stopped.
/// The imager uses it to design new crossover kernels when the crossovers move during real-time playback.
class KernelDesignThread final : public juce::Thread
{
public:
    explicit KernelDesignThread(std::function<void()> task);

    void run() override;

private:
    /// The function called at every poll.
    std::function<void()> work;
};

/// Creates a design thread that calls the work function every DESIGN_POLL_MILLISECONDS once started.
KernelDesignThread::KernelDesignThread(std::function<void()> task) :
    juce::Thread("ImageControl kernel design"),
    work(std::move(task))
{}

/// Calls the work function at a fixed interval until the thread is asked to exit.
void KernelDesignThread::run()
{
    while (!threadShouldExit()) {
        work();
        wait(DESIGN_POLL_MILLISECONDS);
    }
}

/// Creates an imager with every gain at unity, every pan at the centre, and every band heard. It must be prepared
/// before processing.
MultibandImager::MultibandImager()
{
    for (auto& gain : midGainSmoothers) {
        gain.setCurrentAndTargetValue(1.0F);
    }
    for (auto& gain : sideGainSmoothers) {
        gain.setCurrentAndTargetValue(1.0F);
    }
    for (auto& pan : panSmoothers) {
        pan.setCurrentAndTargetValue(0.0F);
    }
    for (auto& solo : soloSmoothers) {
        solo.setCurrentAndTargetValue(1.0F);
    }
    for (auto& width : widthSmoothers) {
        width.setCurrentAndTargetValue(1.0F);
    }
    for (auto& recovery : recoverySmoothers) {
        recovery.setCurrentAndTargetValue(0.0F);
    }
    outputGainSmoother.setCurrentAndTargetValue(1.0F);
    deltaSmoother.setCurrentAndTargetValue(0.0F);
}

/// Stops the design thread before the kernels it writes are destroyed.
MultibandImager::~MultibandImager()
{
    release();
}

/// Returns independent published meter readings, without reading the audio-thread accumulators.
BandCorrelations MultibandImager::getBandCorrelations() const noexcept
{
    return correlation.readings();
}

/// Returns the maximum post-ringing after compensating for the plugin latency.
double MultibandImager::getTailLengthSeconds() const noexcept
{
    return static_cast<double>(halfLength) / sampleRate;
}

/// Returns the crossovers of the kernels the audio thread currently uses.
/// Reads audio thread state, so call it from the audio thread or while processing is stopped.
Crossovers MultibandImager::getActiveCrossovers() const
{
    return slotCrossovers.at(static_cast<std::size_t>(activeSlot));
}

/// Returns the latency of the processing in samples: the kernel half length and one partition.
int MultibandImager::getLatencySamples() const
{
    return halfLength + PARTITION_SIZE;
}

/// Allocates every buffer for a sample rate and a mono or stereo channel count,
/// designs the kernels of the crossovers synchronously, clears the signal history,
/// and starts the design thread.
/// Gains, Width, Recover, pans, and solos jump to their current targets, so processing starts without a glide.
void MultibandImager::prepare(const double newSampleRate, const int newChannelCount, const Crossovers& crossovers)
{
    release();

    sampleRate = newSampleRate;
    channelCount = std::clamp(newChannelCount, 1, 2);
    halfLength = kernelHalfLength(sampleRate);

    const Crossovers ordered = orderCrossovers(crossovers);
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        requestedCrossovers.at(crossover).store(ordered.at(crossover));
    }
    kernelSlots[0] = designKernelSpectra(ordered, sampleRate, halfLength);
    kernelSlots[1] = {};
    slotCrossovers[0] = ordered;
    slotCrossovers[1] = {};
    activeSlot = 0;
    publishedActiveSlot.store(0);
    switchPending.store(false);

    const std::size_t partitions = kernelSlots[0].getPartitionCount();
    midConvolver.prepare(PARTITION_SIZE, partitions);
    sideConvolver.prepare(PARTITION_SIZE, partitions);

    delayLine.setSize(channelCount, halfLength);
    delayedBlock.setSize(channelCount, PARTITION_SIZE);
    inputBlock.setSize(channelCount, PARTITION_SIZE);
    outputBlock.setSize(channelCount, PARTITION_SIZE);
    delayLine.clear();
    delayedBlock.clear();
    inputBlock.clear();
    outputBlock.clear();

    const auto partitionSamples = static_cast<std::size_t>(PARTITION_SIZE);
    for (auto& lowpassed : midLowpassed) {
        lowpassed.assign(partitionSamples, 0.0F);
    }
    for (auto& lowpassed : sideLowpassed) {
        lowpassed.assign(partitionSamples, 0.0F);
    }
    for (auto& lowpassed : quadratureLowpassed) {
        lowpassed.assign(partitionSamples, 0.0F);
    }
    quadratureFull.assign(partitionSamples, 0.0F);
    midInput.assign(partitionSamples, 0.0F);
    sideInput.assign(partitionSamples, 0.0F);
    fadeScratch.assign(partitionSamples, 0.0F);

    for (auto& gain : midGainSmoothers) {
        gain.reset(sampleRate, GAIN_SMOOTHING_SECONDS);
        gain.setCurrentAndTargetValue(gain.getTargetValue());
    }
    for (auto& gain : sideGainSmoothers) {
        gain.reset(sampleRate, GAIN_SMOOTHING_SECONDS);
        gain.setCurrentAndTargetValue(gain.getTargetValue());
    }
    for (auto& pan : panSmoothers) {
        pan.reset(sampleRate, GAIN_SMOOTHING_SECONDS);
        pan.setCurrentAndTargetValue(pan.getTargetValue());
    }
    for (auto& solo : soloSmoothers) {
        solo.reset(sampleRate, GAIN_SMOOTHING_SECONDS);
        solo.setCurrentAndTargetValue(solo.getTargetValue());
    }
    for (auto& width : widthSmoothers) {
        width.reset(sampleRate, GAIN_SMOOTHING_SECONDS);
        width.setCurrentAndTargetValue(width.getTargetValue());
    }
    for (auto& recovery : recoverySmoothers) {
        recovery.reset(sampleRate, GAIN_SMOOTHING_SECONDS);
        recovery.setCurrentAndTargetValue(recovery.getTargetValue());
    }
    outputGainSmoother.reset(sampleRate, GAIN_SMOOTHING_SECONDS);
    outputGainSmoother.setCurrentAndTargetValue(outputGainSmoother.getTargetValue());
    deltaSmoother.reset(sampleRate, GAIN_SMOOTHING_SECONDS);
    deltaSmoother.setCurrentAndTargetValue(deltaSmoother.getTargetValue());

    correlation.prepare(sampleRate);
    reset();

    designThread = std::make_unique<KernelDesignThread>([this] { designRequestedKernels(true); });
    designThread->startThread(juce::Thread::Priority::low);
}

/// Stops the design thread. Processing must not run again before the next prepare.
void MultibandImager::release()
{
    if (designThread != nullptr) {
        designThread->stopThread();
        designThread.reset();
    }
}

/// Clears delayed audio and convolution history without changing parameters, kernels, or allocating.
/// Call on the processing thread or while callbacks are stopped. The design thread never accesses these buffers.
void MultibandImager::reset() noexcept
{
    delayLine.clear();
    delayedBlock.clear();
    inputBlock.clear();
    outputBlock.clear();
    midConvolver.reset();
    sideConvolver.reset();
    for (auto& lowpassed : quadratureLowpassed) {
        std::ranges::fill(lowpassed, 0.0F);
    }
    std::ranges::fill(quadratureFull, 0.0F);
    for (auto& width : widthSmoothers) {
        width.setCurrentAndTargetValue(width.getTargetValue());
    }
    for (auto& recovery : recoverySmoothers) {
        recovery.setCurrentAndTargetValue(recovery.getTargetValue());
    }
    blockPosition = 0;
    delayPosition = 0;
    correlation.reset();
    for (auto& gain : midGainSmoothers) {
        gain.setCurrentAndTargetValue(gain.getTargetValue());
    }
    for (auto& gain : sideGainSmoothers) {
        gain.setCurrentAndTargetValue(gain.getTargetValue());
    }
    for (auto& pan : panSmoothers) {
        pan.setCurrentAndTargetValue(pan.getTargetValue());
    }
    for (auto& solo : soloSmoothers) {
        solo.setCurrentAndTargetValue(solo.getTargetValue());
    }
    outputGainSmoother.setCurrentAndTargetValue(outputGainSmoother.getTargetValue());
    deltaSmoother.setCurrentAndTargetValue(deltaSmoother.getTargetValue());
}

/// Enables metering on the audio thread. Re-enabling starts with fresh moments rather than stale audio.
void MultibandImager::setCorrelationEnabled(const bool enabled) noexcept
{
    if (enabled != correlationEnabled) {
        correlation.reset();
        correlationEnabled = enabled;
    }
}

/// Requests new crossover frequencies, which are ordered with orderCrossovers first.
/// During real-time playback the design thread picks the request up,
/// and the audio thread crossfades to the new kernels when they are ready.
/// During offline rendering the calling audio thread designs the kernels itself and the design thread stands by,
/// so the output does not depend on thread timing.
/// The three values are published as one snapshot, so the design thread never mixes two requests.
void MultibandImager::setCrossovers(const Crossovers& crossovers, const bool offline)
{
    offlineRendering.store(offline, std::memory_order_release);
    const Crossovers ordered = orderCrossovers(crossovers);
    const std::uint32_t sequence = requestSequence.load(std::memory_order_relaxed);
    requestSequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        requestedCrossovers[crossover].store(ordered[crossover], std::memory_order_relaxed);
    }
    requestSequence.store(sequence + 2, std::memory_order_release);
    if (offline) {
        designRequestedKernels(false);
    }
}

/// Turns delta mode on or off: with it on, the output is the processed output minus the delayed input,
/// so it carries only what the processing changed.
/// The change glides over GAIN_SMOOTHING_SECONDS, so toggling it does not click.
void MultibandImager::setDelta(const bool delta)
{
    deltaSmoother.setTargetValue(delta ? 1.0F : 0.0F);
}

/// Sets the linear target gains of the mid and side of each band and of the output.
/// The gains glide to their targets over GAIN_SMOOTHING_SECONDS.
void MultibandImager::setGains(const BandGains& midGains, const BandGains& sideGains, const float outputGain)
{
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        midGainSmoothers[band].setTargetValue(midGains[band]);
        sideGainSmoothers[band].setTargetValue(sideGains[band]);
    }
    outputGainSmoother.setTargetValue(outputGain);
}

/// Sets the target pan of each band, from -1 for fully left through 0 for the centre to 1 for fully right.
/// The pans glide to their targets over GAIN_SMOOTHING_SECONDS.
void MultibandImager::setPans(const BandPans& pans)
{
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        panSmoothers[band].setTargetValue(pans[band]);
    }
}

/// Sets independent Width targets without changing either source gain. Zero removes stereo Side.
void MultibandImager::setWidths(const BandWidths& widths)
{
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const float width = std::isfinite(widths[band]) ? std::clamp(widths[band], 0.0F, WIDTH_MAX) : 1.0F;
        widthSmoothers[band].setTargetValue(width);
    }
}

/// Sets the fraction of removed Side waveform to recover into Mid, effective only below unity Width.
void MultibandImager::setRecoveries(const BandRecoveries& recoveries)
{
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const float recovery = std::isfinite(recoveries[band]) ? std::clamp(recoveries[band], 0.0F, 1.0F) : 0.0F;
        recoverySmoothers[band].setTargetValue(recovery);
    }
}

/// Sets how much of each band is heard, from 0 for a band muted by the solos of other bands to 1 for a heard band,
/// as soloGains returns them. The changes glide over GAIN_SMOOTHING_SECONDS, so soloing a band does not click.
void MultibandImager::setSolos(const BandGains& audible)
{
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        soloSmoothers[band].setTargetValue(audible[band]);
    }
}

/// Processes a block in place.
/// The input goes into the current partition and the output comes from the previous one,
/// so any block size works, and a whole partition is processed whenever one fills up.
/// Channels beyond the prepared channel count are cleared.
void MultibandImager::process(juce::AudioBuffer<float>& buffer)
{
    const int channels = std::min(buffer.getNumChannels(), channelCount);
    const int samples = buffer.getNumSamples();
    int done = 0;
    while (done < samples) {
        const int chunk = std::min(samples - done, PARTITION_SIZE - blockPosition);
        for (int channel = 0; channel < channels; ++channel) {
            inputBlock.copyFrom(channel, blockPosition, buffer, channel, done, chunk);
            buffer.copyFrom(channel, done, outputBlock, channel, blockPosition, chunk);
        }
        blockPosition += chunk;
        done += chunk;
        if (blockPosition == PARTITION_SIZE) {
            processPartition();
            blockPosition = 0;
        }
    }
    for (int channel = channels; channel < buffer.getNumChannels(); ++channel) {
        buffer.clear(channel, 0, samples);
    }
}

/// Returns the requested crossovers as one consistent snapshot.
/// The audio thread is the only writer and marks a write in progress with an odd sequence,
/// so a reader retries until it read all three values between the same two even sequence values.
Crossovers MultibandImager::readRequestedCrossovers() const
{
    Crossovers requested {};
    while (true) {
        const std::uint32_t before = requestSequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
            requested[crossover] = requestedCrossovers[crossover].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (requestSequence.load(std::memory_order_relaxed) == before) {
            return requested;
        }
    }
}

/// Returns whether every pan has settled exactly at the centre, so the bands need no panning.
bool MultibandImager::isCentred() const
{
    return std::ranges::all_of(panSmoothers, isSettledAtCentre);
}

/// Returns whether every band is heard in full, so no solo mutes or fades any band.
bool MultibandImager::isUnsoloed() const
{
    return std::ranges::all_of(soloSmoothers, isSettledAtUnity);
}

/// Returns whether the delta mix has settled exactly at a target, 0 for off or 1 for on.
bool MultibandImager::isDeltaSettled(const float target) const
{
    return !deltaSmoother.isSmoothing() && juce::exactlyEqual(deltaSmoother.getCurrentValue(), target);
}

/// Returns whether gains and stereo Width are neutral, every pan centred, and every band heard.
/// Recover has no contribution at settled unity Width, so its amount does not prevent exact pass-through.
bool MultibandImager::isNeutral() const
{
    return std::ranges::all_of(midGainSmoothers, isSettledAtUnity)
        && std::ranges::all_of(sideGainSmoothers, isSettledAtUnity) && isSettledAtUnity(outputGainSmoother)
        && (channelCount == 1 || std::ranges::all_of(widthSmoothers, isSettledAtUnity)) && isCentred() && isUnsoloed();
}

/// Reports whether any current or target ramp can inject recovered Side during this partition.
/// Side input history is retained even while these extra convolution outputs are not needed.
bool MultibandImager::isRecoveryActive() const
{
    if (channelCount != 2) {
        return false;
    }
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const auto& width = widthSmoothers[band];
        const auto& recovery = recoverySmoothers[band];
        if (std::min(width.getCurrentValue(), width.getTargetValue()) < 1.0F
            && std::max(recovery.getCurrentValue(), recovery.getTargetValue()) > 0.0F)
        {
            return true;
        }
    }
    return false;
}

/// Designs kernels for the requested crossovers into the spare slot when they differ from the active ones
/// and no designed slot is waiting to be switched to.
/// Runs on the design thread during real-time playback, and on the audio thread during offline rendering,
/// when the design thread leaves it alone.
void MultibandImager::designRequestedKernels(const bool fromDesignThread)
{
    const std::scoped_lock lock(designMutex);
    if (fromDesignThread && offlineRendering.load(std::memory_order_acquire)) {
        return;
    }
    if (switchPending.load(std::memory_order_acquire)) {
        return;
    }

    const Crossovers requested = readRequestedCrossovers();
    const auto active = static_cast<std::size_t>(publishedActiveSlot.load(std::memory_order_acquire));
    if (sameCrossovers(requested, slotCrossovers.at(active))) {
        return;
    }

    const std::size_t spare = 1 - active;
    kernelSlots.at(spare) = designKernelSpectra(requested, sampleRate, halfLength);
    slotCrossovers.at(spare) = requested;
    switchPending.store(true, std::memory_order_release);
}

/// Convolves one ordinary or quadrature output, crossfading both coherent slots before publication.
void MultibandImager::convolveSignal(
    PartitionedConvolver& convolver,
    const std::size_t kernel,
    const std::span<float> output,
    const bool crossfade
)
{
    convolver.convolve(kernelSlots[static_cast<std::size_t>(activeSlot)], kernel, output);
    if (crossfade) {
        convolver.convolve(kernelSlots[static_cast<std::size_t>(1 - activeSlot)], kernel, fadeScratch);
        constexpr float FADE_STEP {1.0F / static_cast<float>(PARTITION_SIZE)};
        for (std::size_t sample = 0; sample < output.size(); ++sample) {
            const float fade = static_cast<float>(sample + 1) * FADE_STEP;
            output[sample] += (fadeScratch[sample] - output[sample]) * fade;
        }
    }
}

/// Runs a set of ordinary or quadrature lowpasses from the same raw input history.
void MultibandImager::lowpassSignal(
    PartitionedConvolver& convolver,
    const std::span<std::vector<float>> outputs,
    const bool crossfade,
    const std::size_t firstKernel
)
{
    for (std::size_t kernel = 0; kernel < outputs.size(); ++kernel) {
        convolveSignal(convolver, firstKernel + kernel, outputs[kernel], crossfade);
    }
}

/// Mixes bands with source trims, independent Width, optional quadrature recovery, Pan, and Solo.
/// Recovery uses the pre-Width Side source gain, so Side off also silences its recovered contribution.
/// A stereo signal is joined back from its mid and side, and a mono signal is its mid, which has no pan.
/// While every pan is centred, the mid and side bands are summed first and joined once,
/// and otherwise each band is joined into its own left and right and panned before the bands are summed.
/// While solos mute some bands, the gains of each band are scaled by how much of it is heard,
/// and delta subtracts only the heard bands of the delayed input,
/// so it carries what the processing changed in the soloed bands.
/// Without solos the delayed input itself is subtracted, which keeps delta at neutral settings exactly silent.
void MultibandImager::mixPartition(const int channels)
{
    const auto delayedLeft = channelSamples(delayedBlock, 0);
    const auto outputLeft = channelSamples(outputBlock, 0);
    const bool stereo = channels == 2;
    const auto delayedRight = channelSamples(delayedBlock, stereo ? 1 : 0);
    const auto outputRight = channelSamples(outputBlock, stereo ? 1 : 0);
    const bool centred = isCentred();
    const bool unsoloed = isUnsoloed();
    const bool recovering = isRecoveryActive();

    BandGains midGains {};
    BandGains sideGains {};
    BandGains recoveryGains {};
    BandGains solos {};
    BandPans pans {};
    for (std::size_t sample = 0; sample < outputLeft.size(); ++sample) {
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            midGains[band] = midGainSmoothers[band].getNextValue();
            const float sourceSideGain = sideGainSmoothers[band].getNextValue();
            const float width = widthSmoothers[band].getNextValue();
            const float recovery = recoverySmoothers[band].getNextValue();
            sideGains[band] = sourceSideGain * width;
            recoveryGains[band] = sourceSideGain * recovery * std::max(0.0F, 1.0F - width);
            pans[band] = panSmoothers[band].getNextValue();
            solos[band] = soloSmoothers[band].getNextValue();
        }
        if (!unsoloed) {
            for (std::size_t band = 0; band < BAND_COUNT; ++band) {
                midGains[band] *= solos[band];
                sideGains[band] *= solos[band];
                recoveryGains[band] *= solos[band];
            }
        }
        const float outputGain = outputGainSmoother.getNextValue();
        const float delta = deltaSmoother.getNextValue();
        if (stereo) {
            const float delayedMid = (delayedLeft[sample] + delayedRight[sample]) * 0.5F;
            const float delayedSide = (delayedLeft[sample] - delayedRight[sample]) * 0.5F;
            // What delta subtracts: the delayed input, or with solos only its heard bands.
            float dryLeft = delayedLeft[sample];
            float dryRight = delayedRight[sample];
            if (!unsoloed) {
                const float dryMid = sumBands(solos, midLowpassed, sample, delayedMid);
                const float drySide = sumBands(solos, sideLowpassed, sample, delayedSide);
                dryLeft = dryMid + drySide;
                dryRight = dryMid - drySide;
            }
            if (centred) {
                const float recoveredMid
                    = recoveredMidSum(recovering, recoveryGains, quadratureLowpassed, quadratureFull, sample);
                const float mid = sumBands(midGains, midLowpassed, sample, delayedMid) + recoveredMid;
                const float side = sumBands(sideGains, sideLowpassed, sample, delayedSide);
                outputLeft[sample] = (mid + side) * outputGain - delta * dryLeft;
                outputRight[sample] = (mid - side) * outputGain - delta * dryRight;
                continue;
            }
            float left = 0.0F;
            float right = 0.0F;
            for (std::size_t band = 0; band < BAND_COUNT; ++band) {
                const float recoveredMid = recoveredBandSample(
                    recovering, recoveryGains[band], quadratureLowpassed, quadratureFull, band, sample
                );
                const float mid = midGains[band] * bandSample(midLowpassed, band, sample, delayedMid) + recoveredMid;
                const float side = sideGains[band] * bandSample(sideLowpassed, band, sample, delayedSide);
                const float bandLeft = mid + side;
                const float bandRight = mid - side;
                // Panning right by p keeps 1 - p of the left in the left and moves p of it to the right,
                // and panning left mirrors that, so only one of the two amounts is ever nonzero.
                const float toRight = std::max(pans[band], 0.0F);
                const float toLeft = std::max(-pans[band], 0.0F);
                left += bandLeft * (1.0F - toRight) + bandRight * toLeft;
                right += bandRight * (1.0F - toLeft) + bandLeft * toRight;
            }
            outputLeft[sample] = left * outputGain - delta * dryLeft;
            outputRight[sample] = right * outputGain - delta * dryRight;
        } else {
            const float dry
                = unsoloed ? delayedLeft[sample] : sumBands(solos, midLowpassed, sample, delayedLeft[sample]);
            outputLeft[sample]
                = sumBands(midGains, midLowpassed, sample, delayedLeft[sample]) * outputGain - delta * dry;
        }
    }
}

/// Measures the existing band signals with copies of the control glides, leaving audio arithmetic unchanged.
/// This runs only with visible meters and adds no filter bank, allocations, or cross-thread locks.
void MultibandImager::updateCorrelationPartition()
{
    auto midGains = midGainSmoothers;
    auto sideGains = sideGainSmoothers;
    auto widths = widthSmoothers;
    auto recoveries = recoverySmoothers;
    const bool recovering = isRecoveryActive();
    auto pans = panSmoothers;
    auto solos = soloSmoothers;
    const bool stereo = channelCount == 2;
    const auto delayedLeft = channelSamples(delayedBlock, 0);
    const auto delayedRight = channelSamples(delayedBlock, stereo ? 1 : 0);
    for (std::size_t sample = 0; sample < delayedLeft.size(); ++sample) {
        const float delayedMid = stereo ? (delayedLeft[sample] + delayedRight[sample]) * 0.5F : delayedLeft[sample];
        const float delayedSide = stereo ? (delayedLeft[sample] - delayedRight[sample]) * 0.5F : 0.0F;
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            const float audible = solos[band].getNextValue();
            float mid = midGains[band].getNextValue() * audible * bandSample(midLowpassed, band, sample, delayedMid);
            const float sideGain = sideGains[band].getNextValue();
            const float width = widths[band].getNextValue();
            const float recovery = recoveries[band].getNextValue();
            if (recovering) {
                mid += sideGain * audible * recovery * std::max(0.0F, 1.0F - width)
                    * bandSample(quadratureLowpassed, band, sample, quadratureFull[sample]);
            }
            const float pan = pans[band].getNextValue();
            const float side
                = stereo ? sideGain * width * audible * bandSample(sideLowpassed, band, sample, delayedSide) : 0.0F;
            const float left = mid + side;
            const float right = mid - side;
            const float toRight = stereo ? std::max(pan, 0.0F) : 0.0F;
            const float toLeft = stereo ? std::max(-pan, 0.0F) : 0.0F;
            correlation.push(band, left * (1.0F - toRight) + right * toLeft, right * (1.0F - toLeft) + left * toRight);
        }
    }
    correlation.publish();
}

/// Computes ordinary and active recovery outputs from the current raw signal histories.
void MultibandImager::convolvePartition(const bool stereo, const bool crossfade)
{
    lowpassSignal(midConvolver, midLowpassed, crossfade);
    if (stereo) {
        lowpassSignal(sideConvolver, sideLowpassed, crossfade);
        if (isRecoveryActive()) {
            lowpassSignal(sideConvolver, quadratureLowpassed, crossfade, QUADRATURE_FIRST_KERNEL);
            convolveSignal(sideConvolver, QUADRATURE_FULL_KERNEL, quadratureFull, crossfade);
        }
    }
}

/// Processes one full input partition into the output partition.
/// The raw input is delayed by the kernel half length to line up with the kernel outputs,
/// and while every gain is at unity, that delayed input is the output even during a kernel crossfade.
void MultibandImager::processPartition()
{
    const auto ringMask = static_cast<std::size_t>(halfLength - 1);
    std::size_t position = 0;
    for (int channel = 0; channel < channelCount; ++channel) {
        const auto ring = channelSamples(delayLine, channel);
        const auto input = channelSamples(inputBlock, channel);
        const auto delayed = channelSamples(delayedBlock, channel);
        position = static_cast<std::size_t>(delayPosition);
        for (std::size_t sample = 0; sample < input.size(); ++sample) {
            delayed[sample] = ring[position];
            ring[position] = input[sample];
            position = (position + 1) & ringMask;
        }
    }
    delayPosition = static_cast<int>(position);

    const bool stereo = channelCount == 2;
    const auto left = channelSamples(inputBlock, 0);
    if (stereo) {
        const auto right = channelSamples(inputBlock, 1);
        for (std::size_t sample = 0; sample < left.size(); ++sample) {
            midInput[sample] = (left[sample] + right[sample]) * 0.5F;
            sideInput[sample] = (left[sample] - right[sample]) * 0.5F;
        }
    } else {
        std::ranges::copy(left, midInput.begin());
    }

    midConvolver.pushInput(midInput);
    if (stereo) {
        sideConvolver.pushInput(sideInput);
    }

    const bool crossfade = switchPending.load(std::memory_order_acquire);
    convolvePartition(stereo, crossfade);
    if (crossfade) {
        activeSlot = 1 - activeSlot;
        publishedActiveSlot.store(activeSlot, std::memory_order_release);
        switchPending.store(false, std::memory_order_release);
        // Offline, a request that arrived while this switch was pending is designed right away,
        // so it takes effect at the next partition no matter how the host splits its blocks.
        if (offlineRendering.load(std::memory_order_relaxed)) {
            designRequestedKernels(false);
        }
    }

    // At neutral settings the processed output is the delayed input itself,
    // so the output is that copy, or exact silence in delta mode.
    if (correlationEnabled) {
        updateCorrelationPartition();
    }
    if (isNeutral() && (isDeltaSettled(0.0F) || isDeltaSettled(1.0F))) {
        // Inactive recovery still follows automation while the exact-copy path avoids arithmetic.
        for (auto& recovery : recoverySmoothers) {
            recovery.skip(PARTITION_SIZE);
        }
        for (auto& width : widthSmoothers) {
            width.skip(PARTITION_SIZE);
        }
        if (isDeltaSettled(0.0F)) {
            for (int channel = 0; channel < channelCount; ++channel) {
                outputBlock.copyFrom(channel, 0, delayedBlock, channel, 0, PARTITION_SIZE);
            }
            return;
        }
        outputBlock.clear();
        return;
    }
    mixPartition(channelCount);
}
}  // namespace esgrove::image_control
