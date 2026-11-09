/// Declares MultibandImager, the audio processing of ImageControl, and the KernelDesignThread it owns.
/// The imager splits stereo into Mid/Side and four linear-phase bands, then applies source trims and Width/Recover.
/// It reconstructs and pans each band before summing.
/// The bands sum exactly to the delayed input, so neutral settings pass the input through, only delayed.

#pragma once

#include "dsp/BandCorrelation.h"
#include "dsp/CrossoverDesign.h"
#include "dsp/PartitionedConvolver.h"
#include "Parameters.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace esgrove::image_control
{
/// Samples per convolution partition.
/// The imager processes whole partitions, so this adds to the latency.
constexpr int PARTITION_SIZE {256};
/// Time gains, Width, Recover, Pan, Solo, and Delta take to glide to a new value, in seconds.
constexpr double GAIN_SMOOTHING_SECONDS {0.02};

/// The background thread that designs kernels while the crossovers move during real-time playback.
/// Defined in MultibandImager.cpp, its only user.
class KernelDesignThread;

/// Linear gain or pan that glides to its target over GAIN_SMOOTHING_SECONDS.
using SmoothedGain = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

/// Multiband mid/side processing with linear-phase crossovers.
///
/// Mid and side each go through three lowpass kernels.
/// The bands are the lowest lowpass, the differences between neighbouring lowpasses,
/// and the delayed signal minus the highest lowpass, so they sum to the delayed signal whatever the kernels are.
/// The mid and side of each band are scaled by source gains, then Width independently scales Side.
/// Optional Recover mixes a quadrature version of the narrowed Side into Mid before reconstruction.
/// Direct finite FIR recovery shares raw Side history and crossover delay, without changing normal Mid phase.
/// Each band is then panned, after its gains, so a band narrowed to mono can still be moved to one side.
/// The pan moves the band toward one channel without discarding the other:
/// panning right by p scales left by 1 - p and adds p times left to right, and panning left mirrors that.
/// The bands are summed back and scaled by the output gain.
/// Solos mute the bands that are not soloed, unless no band or every band is soloed.
///
/// The latency is the kernel half length plus one partition.
/// While every gain and Width is at unity, every pan at the centre, and no band muted by a solo,
/// the output is the input delayed by the latency, bit for bit.
/// In delta mode the delayed input is subtracted from the output, so it carries only what the processing changed,
/// and neutral settings give exact silence.
/// With solos, delta subtracts only the soloed bands of the delayed input.
///
/// prepare, release, and the destructor run on the message thread.
/// Control setters and process run on the audio thread
/// and never allocate during real-time playback.
/// During non-real-time rendering the audio thread designs kernels itself and the design thread stands by,
/// so the output never depends on thread timing.
class MultibandImager
{
public:
    MultibandImager();

    ~MultibandImager();

    MultibandImager(const MultibandImager&) = delete;
    MultibandImager(MultibandImager&&) = delete;
    MultibandImager& operator=(const MultibandImager&) = delete;
    MultibandImager& operator=(MultibandImager&&) = delete;

    [[nodiscard]] BandCorrelations getBandCorrelations() const noexcept;
    [[nodiscard]] Crossovers getActiveCrossovers() const;

    [[nodiscard]] double getTailLengthSeconds() const noexcept;
    [[nodiscard]] int getLatencySamples() const;

    void prepare(double newSampleRate, int newChannelCount, const Crossovers& crossovers);
    void process(juce::AudioBuffer<float>& buffer);
    void release();
    void reset() noexcept;
    void setCorrelationEnabled(bool enabled) noexcept;
    void setCrossovers(const Crossovers& crossovers, bool offline);
    void setDelta(bool delta);
    void setGains(const BandGains& midGains, const BandGains& sideGains, float outputGain);
    void setPans(const BandPans& pans);
    void setRecoveries(const BandRecoveries& recoveries);
    void setSolos(const BandGains& audible);
    void setWidths(const BandWidths& widths);

private:
    [[nodiscard]] Crossovers readRequestedCrossovers() const;

    [[nodiscard]] bool isCentred() const;
    [[nodiscard]] bool isDeltaSettled(float target) const;
    [[nodiscard]] bool isNeutral() const;
    [[nodiscard]] bool isRecoveryActive() const;
    [[nodiscard]] bool isUnsoloed() const;

    void convolvePartition(bool stereo, bool crossfade);
    void convolveSignal(PartitionedConvolver& convolver, std::size_t kernel, std::span<float> output, bool crossfade);
    void designRequestedKernels(bool fromDesignThread);
    void lowpassSignal(
        PartitionedConvolver& convolver,
        std::span<std::vector<float>> outputs,
        bool crossfade,
        std::size_t firstKernel = 0
    );
    void mixPartition(int channels);
    void processPartition();
    void updateCorrelationPartition();

    /// Audio-thread-owned correlation history with atomically published independent readings.
    BandCorrelation correlation;

    /// Ring buffer per channel that delays the raw input by the kernel half length.
    juce::AudioBuffer<float> delayLine;
    /// The delayed input of the current partition.
    juce::AudioBuffer<float> delayedBlock;
    /// The input partition being filled by process.
    juce::AudioBuffer<float> inputBlock;
    /// The output partition being read by process.
    juce::AudioBuffer<float> outputBlock;

    /// Convolver of the mid signal.
    PartitionedConvolver midConvolver;
    /// Convolver of the side signal.
    PartitionedConvolver sideConvolver;

    /// Designs kernels during real-time playback. Created in prepare and stopped in release.
    std::unique_ptr<KernelDesignThread> designThread;

    /// The active kernels and the spare slot new kernels are designed into.
    std::array<KernelSpectra, 2> kernelSlots;
    /// Crossovers each kernel slot was designed for.
    std::array<Crossovers, 2> slotCrossovers {};
    /// Crossovers last requested by the host, ordered, read as one snapshot through requestSequence.
    std::array<std::atomic<float>, CROSSOVER_COUNT> requestedCrossovers {};

    /// Glides of the mid gain of each band.
    std::array<SmoothedGain, BAND_COUNT> midGainSmoothers;
    /// Glides of the side gain of each band.
    std::array<SmoothedGain, BAND_COUNT> sideGainSmoothers;
    /// Glides of the pan of each band, from -1 for fully left to 1 for fully right.
    std::array<SmoothedGain, BAND_COUNT> panSmoothers;
    /// Glides of how much of each band is heard, 1 unless the solos of other bands mute it.
    std::array<SmoothedGain, BAND_COUNT> soloSmoothers;
    /// Independent Side multipliers, with unity retaining the gain-trimmed stereo image.
    std::array<SmoothedGain, BAND_COUNT> widthSmoothers;
    /// Amount of narrowed Side to phase-rotate into Mid, from zero to one.
    std::array<SmoothedGain, BAND_COUNT> recoverySmoothers;
    /// Glide of the output gain.
    SmoothedGain outputGainSmoother;
    /// Glide of the delta mix, from 0 for the processed output to 1 for the output minus the delayed input.
    SmoothedGain deltaSmoother;

    /// Outputs of the three lowpass kernels for the mid signal of the current partition.
    std::array<std::vector<float>, CROSSOVER_COUNT> midLowpassed;
    /// Outputs of the three lowpass kernels for the side signal of the current partition.
    std::array<std::vector<float>, CROSSOVER_COUNT> sideLowpassed;
    /// Quadrature Side lowpasses, sharing the ordinary crossover delay and raw Side history.
    std::array<std::vector<float>, CROSSOVER_COUNT> quadratureLowpassed;
    /// Full-band quadrature Side reference for the highest recovery band.
    std::vector<float> quadratureFull;
    /// The mid signal of the current input partition.
    std::vector<float> midInput;
    /// The side signal of the current input partition.
    std::vector<float> sideInput;
    /// The output of the new kernels during a crossfade.
    std::vector<float> fadeScratch;

    /// Serializes kernel design between the design thread and non-real-time rendering.
    std::mutex designMutex;

    /// Kernel slot the audio thread uses, published for the design thread.
    std::atomic<int> publishedActiveSlot {0};
    /// Even while requestedCrossovers holds a complete request, and odd while the audio thread writes one.
    std::atomic<std::uint32_t> requestSequence {0};
    /// Whether the host renders offline, so the design thread leaves kernel design to the audio thread.
    std::atomic<bool> offlineRendering {false};
    /// Set by a designer when the spare slot holds new kernels, and cleared by the audio thread after switching.
    std::atomic<bool> switchPending {false};

    /// Whether this processing pass computes band correlation, set only by the audio thread.
    bool correlationEnabled {false};
    /// Sample rate the imager was prepared for.
    double sampleRate {REFERENCE_SAMPLE_RATE};
    /// Kernel slot the audio thread uses.
    int activeSlot {0};
    /// Position in the current input and output partitions.
    int blockPosition {0};
    /// Number of channels the imager was prepared for, one or two.
    int channelCount {2};
    /// Position of the next sample in the delay line.
    int delayPosition {0};
    /// Kernel half length, which is also the delay of the kernels, in samples.
    int halfLength {REFERENCE_HALF_LENGTH};
};
}  // namespace esgrove::image_control
