/// Declares MultibandImager, the audio processing of ImageControl, and the KernelDesignThread it owns.
/// The imager splits a stereo signal into mid and side, splits both into four linear-phase bands,
/// scales the mid and side of each band, and joins them back into left and right.
/// The bands sum exactly to the delayed input, so neutral settings pass the input through, only delayed.

#pragma once

#include "dsp/CrossoverDesign.h"
#include "dsp/PartitionedConvolver.h"
#include "Parameters.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace esgrove::image_control
{
/// Samples per convolution partition.
/// The imager processes whole partitions, so this adds to the latency.
constexpr int PARTITION_SIZE {256};
/// Time the band and output gains take to glide to a new value, in seconds.
constexpr double GAIN_SMOOTHING_SECONDS {0.02};

/// A background thread that calls its work function at a fixed interval until it is stopped.
/// The imager uses it to design new crossover kernels when the crossovers move during real-time playback.
class KernelDesignThread final : public juce::Thread
{
public:
    explicit KernelDesignThread(std::function<void()> task);

    void run() override;

private:
    std::function<void()> work;
};

/// Linear gain that glides to its target over GAIN_SMOOTHING_SECONDS.
using SmoothedGain = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

/// Multiband mid/side processing with linear-phase crossovers.
///
/// Mid and side each go through three lowpass kernels.
/// The bands are the lowest lowpass, the differences between neighbouring lowpasses,
/// and the delayed signal minus the highest lowpass, so they sum to the delayed signal whatever the kernels are.
/// Each band is scaled by its gain and the bands are summed back.
///
/// The latency is the kernel half length plus one partition.
/// While every gain is at unity, the output is the input delayed by the latency, bit for bit.
///
/// prepare, release, and the destructor run on the message thread.
/// setGains, setCrossovers, and process run on the audio thread and never allocate,
/// except that setCrossovers designs kernels synchronously during non-real-time rendering.
class MultibandImager
{
public:
    MultibandImager();

    ~MultibandImager();

    MultibandImager(const MultibandImager&) = delete;
    MultibandImager(MultibandImager&&) = delete;
    MultibandImager& operator=(const MultibandImager&) = delete;
    MultibandImager& operator=(MultibandImager&&) = delete;

    [[nodiscard]] Crossovers getActiveCrossovers() const;

    [[nodiscard]] int getLatencySamples() const;

    void prepare(double newSampleRate, int newChannelCount, const Crossovers& crossovers);
    void process(juce::AudioBuffer<float>& buffer);
    void release();
    void setCrossovers(const Crossovers& crossovers, bool designNow);
    void setGains(const BandGains& midGains, const BandGains& sideGains, float outputGain);

private:
    [[nodiscard]] bool isNeutral() const;

    void designRequestedKernels();
    void lowpassSignal(PartitionedConvolver& convolver, std::span<std::vector<float>> outputs, bool crossfade);
    void mixPartition(int channels);
    void processPartition();

    juce::AudioBuffer<float> delayLine;
    juce::AudioBuffer<float> delayedBlock;
    juce::AudioBuffer<float> inputBlock;
    juce::AudioBuffer<float> outputBlock;

    PartitionedConvolver midConvolver;
    PartitionedConvolver sideConvolver;

    std::unique_ptr<KernelDesignThread> designThread;

    std::array<KernelSpectra, 2> kernelSlots;
    /// Crossovers each kernel slot was designed for.
    std::array<Crossovers, 2> slotCrossovers {};
    /// Crossovers last requested by the host, ordered.
    std::array<std::atomic<float>, CROSSOVER_COUNT> requestedCrossovers {};

    std::array<SmoothedGain, BAND_COUNT> midGainSmoothers;
    std::array<SmoothedGain, BAND_COUNT> sideGainSmoothers;
    SmoothedGain outputGainSmoother;

    /// Outputs of the three lowpass kernels for the mid signal of the current partition.
    std::array<std::vector<float>, CROSSOVER_COUNT> midLowpassed;
    /// Outputs of the three lowpass kernels for the side signal of the current partition.
    std::array<std::vector<float>, CROSSOVER_COUNT> sideLowpassed;
    /// The mid and side of the current input partition, and the new kernel output during a crossfade.
    std::vector<float> midInput;
    std::vector<float> sideInput;
    std::vector<float> fadeScratch;

    /// Serializes kernel design between the design thread and non-real-time rendering.
    std::mutex designMutex;

    /// Kernel slot the audio thread uses, published for the design thread.
    std::atomic<int> publishedActiveSlot {0};
    /// Set by a designer when the spare slot holds new kernels, and cleared by the audio thread after switching.
    std::atomic<bool> switchPending {false};

    double sampleRate {REFERENCE_SAMPLE_RATE};
    int activeSlot {0};
    int blockPosition {0};
    int channelCount {2};
    int delayPosition {0};
    int halfLength {REFERENCE_HALF_LENGTH};
};
}  // namespace esgrove::image_control
