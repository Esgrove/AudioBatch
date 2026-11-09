/// Declares the parameters of the ImageControl plugin:
/// the three crossover frequencies, the mid and side gain, the pan, and the solo of each of the four bands,
/// the independent Width and Recover controls, and the output gain, bypass, Delta, and Mono controls.
/// The ids, ranges, and defaults live here so the processor, the editor, and the tests share one definition.
/// Also declares the crossover ordering rule that both the DSP and the editor apply,
/// and the names of the editor settings saved with the plugin state.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstddef>
#include <functional>
#include <optional>

namespace esgrove::image_control
{
/// A nullable, non-owning reference whose owner must outlive every use of the engaged value.
///
/// std::optional cannot hold T& directly, so reference_wrapper stores a copyable, rebindable reference instead.
/// std::nullopt represents absence. When engaged, use value->get() to access the original object without copying it.
template<typename T>
using OptionalRef = std::optional<std::reference_wrapper<T>>;

/// Number of frequency bands the signal is split into.
constexpr std::size_t BAND_COUNT {4};
/// Number of crossover frequencies between the bands.
constexpr std::size_t CROSSOVER_COUNT {BAND_COUNT - 1};

/// Lowest frequency a crossover can be set to, and the left edge of the spectrogram, in Hz.
constexpr float MIN_FREQUENCY {20.0F};
/// Highest frequency a crossover can be set to, and the right edge of the spectrogram, in Hz.
constexpr float MAX_FREQUENCY {20'000.0F};
/// Default crossover frequencies from the lowest to the highest, in Hz.
constexpr std::array<float, CROSSOVER_COUNT> DEFAULT_CROSSOVERS {100.0F, 500.0F, 5000.0F};
/// Smallest ratio between neighbouring crossovers, one third of an octave.
constexpr float MIN_CROSSOVER_RATIO {1.259'921'05F};

/// Lowest band gain in dB, which turns the mid or side signal of the band off.
constexpr float BAND_GAIN_MIN_DB {-60.0F};
/// Highest band gain in dB.
constexpr float BAND_GAIN_MAX_DB {12.0F};
/// Lowest output gain in dB.
constexpr float OUTPUT_GAIN_MIN_DB {-24.0F};
/// Highest output gain in dB.
constexpr float OUTPUT_GAIN_MAX_DB {12.0F};
/// Pan parameter value that moves a band fully into the right channel, in percent.
/// The negative of it moves the band fully into the left channel, and 0 is the centre.
constexpr float PAN_MAX_PERCENT {100.0F};
/// Highest independent side multiplier, which doubles the trimmed side at its maximum.
constexpr float WIDTH_MAX {2.0F};
/// Highest recovery amount in host-facing percent, scaling the full removed Side waveform into the quadrature path.
constexpr float RECOVER_MAX_PERCENT {100.0F};

/// Hz in one kHz, for formatting frequencies.
constexpr float HERTZ_PER_KILOHERTZ {1000.0F};

/// Id of the output gain parameter.
constexpr auto OUTPUT_GAIN_ID {"output_gain"};
/// Id of the bypass parameter, which hosts also use as the plugin's own bypass.
constexpr auto BYPASS_ID {"bypass"};
/// Id of the delta parameter, which outputs only what the processing changes: the output minus the input.
constexpr auto DELTA_ID {"delta"};
/// Id of the mono parameter, which folds the final processed output equally into both channels.
constexpr auto MONO_ID {"mono"};

/// Name of the editor setting that selects the analyzer tap: true for the output, false for the input.
constexpr auto ANALYZE_OUTPUT_PROPERTY {"analyzeOutput"};
/// Name of the editor setting that holds particle-fade and correlation trend/range persistence in seconds.
constexpr auto PERSISTENCE_PROPERTY {"persistence"};
/// Name of the editor setting that holds the pan midpoint in dB:
/// the level difference between the channels drawn half way from the centre to the edge.
constexpr auto PAN_MIDPOINT_PROPERTY {"panMidpoint"};
/// Name of the editor setting that holds the pan edge in dB:
/// the level difference between the channels drawn nearly at the edge.
constexpr auto PAN_EDGE_PROPERTY {"panEdge"};
/// Name of the linear focus setting of older saved states, which the pan midpoint and edge replace.
constexpr auto LEGACY_FOCUS_PROPERTY {"focus"};
/// Name of the editor setting that selects phase colours instead of frequency colours.
constexpr auto PHASE_COLOURS_PROPERTY {"phaseColours"};
/// Name of the editor setting that selects the mel frequency scale instead of the logarithmic one.
constexpr auto MEL_SCALE_PROPERTY {"melScale"};
/// Name of the editor setting that holds the spectrogram contrast in percent:
/// how strongly loud content is emphasised over quiet content.
constexpr auto CONTRAST_PROPERTY {"contrast"};
/// Name of the editor setting that draws the spectrogram as stacked translucent layers with outlines
/// instead of individual particles.
constexpr auto SOLID_RENDERING_PROPERTY {"solidRendering"};
/// Default spectrogram contrast, in percent.
constexpr double DEFAULT_CONTRAST_PERCENT {60.0};
/// Highest spectrogram contrast, in percent.
/// The lowest is 0, which maps levels to brightness linearly in dB below the extra contrast of the highs.
constexpr double MAX_CONTRAST_PERCENT {100.0};
/// Name of the editor setting that shows the display refresh rate.
constexpr auto SHOW_FPS_PROPERTY {"showFps"};
/// Name of the editor setting that shows the correlation meter on each band.
constexpr auto SHOW_BAND_CORRELATION_PROPERTY {"showBandCorrelation"};
/// Name of the editor setting that shows smoothed trend numbers instead of live band correlation numbers.
constexpr auto SMOOTH_CORRELATION_READOUT_PROPERTY {"smoothCorrelationReadout"};
/// Name of the editor setting that stops analyzing and drawing the spectrogram, while the audio is still processed.
constexpr auto BYPASS_RENDERING_PROPERTY {"bypassRendering"};
/// Name of the editor setting that hides the spectrogram, leaving only the controls.
constexpr auto HIDE_SPECTROGRAM_PROPERTY {"hideSpectrogram"};
/// Shortest particle-fade and correlation-trend time constant, also the shortest correlation range window, in seconds.
constexpr double MIN_PERSISTENCE_SECONDS {0.05};
/// Longest particle-fade and correlation-trend time constant, also the longest correlation range window, in seconds.
constexpr double MAX_PERSISTENCE_SECONDS {3.0};
/// Default particle-fade and correlation-trend time constant, also the default correlation range window, in seconds.
constexpr double DEFAULT_PERSISTENCE_SECONDS {0.3};
/// Default level difference drawn half way from the centre to the edge of the spectrogram, in dB.
constexpr double DEFAULT_PAN_MIDPOINT_DB {12.0};
/// Smallest pan midpoint, in dB.
constexpr double MIN_PAN_MIDPOINT_DB {3.0};
/// Largest pan midpoint, in dB.
constexpr double MAX_PAN_MIDPOINT_DB {24.0};
/// Default level difference from which content is drawn at the edge of the spectrogram, in dB.
constexpr double DEFAULT_PAN_EDGE_DB {32.0};
/// Smallest pan edge, in dB.
constexpr double MIN_PAN_EDGE_DB {12.0};
/// Largest pan edge, in dB.
constexpr double MAX_PAN_EDGE_DB {60.0};
/// Smallest distance between the pan midpoint and the pan edge, in dB, which keeps the curve rising.
constexpr double MIN_PAN_CURVE_GAP_DB {3.0};
/// Step of the pan midpoint and edge settings, in dB.
constexpr double PAN_CURVE_STEP_DB {1.0};
/// Name of the editor setting that holds the editor width in pixels.
constexpr auto EDITOR_WIDTH_PROPERTY {"editorWidth"};
/// Name of the editor setting that holds the editor height in pixels.
constexpr auto EDITOR_HEIGHT_PROPERTY {"editorHeight"};
/// Name of the editor setting that holds the height of the band strip section in pixels.
constexpr auto BAND_SECTION_HEIGHT_PROPERTY {"bandSectionHeight"};

/// Crossover frequencies ordered from the lowest to the highest, in Hz.
using Crossovers = std::array<float, CROSSOVER_COUNT>;
/// One linear gain per band, from the lowest band to the highest.
using BandGains = std::array<float, BAND_COUNT>;
/// One pan per band, from the lowest band to the highest:
/// -1 moves the band fully into the left channel, 0 leaves it in place, and 1 moves it fully into the right channel.
using BandPans = std::array<float, BAND_COUNT>;
/// Independent side multipliers per band, from mono at 0 through unchanged at 1 to doubled at WIDTH_MAX.
using BandWidths = std::array<float, BAND_COUNT>;
/// Linear recovery amounts per band, from 0 to 1, applied to the Side waveform amplitude removed by narrowing.
using BandRecoveries = std::array<float, BAND_COUNT>;
/// Whether each band is soloed, from the lowest band to the highest.
using BandSolos = std::array<bool, BAND_COUNT>;

[[nodiscard]] juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
[[nodiscard]] juce::Identifier linkPropertyName(std::size_t band);
[[nodiscard]] juce::NormalisableRange<float> crossoverRange();
[[nodiscard]] juce::NormalisableRange<float> gainRange(float minDecibels, float maxDecibels);
[[nodiscard]] juce::NormalisableRange<float> panRange();

[[nodiscard]] juce::String bandName(std::size_t band);
[[nodiscard]] juce::String crossoverParameterId(std::size_t crossover);
[[nodiscard]] juce::String formatFrequency(float frequency);
[[nodiscard]] juce::String formatPan(float percent);
[[nodiscard]] juce::String midGainParameterId(std::size_t band);
[[nodiscard]] juce::String panParameterId(std::size_t band);
[[nodiscard]] juce::String recoverParameterId(std::size_t band);
[[nodiscard]] juce::String sideGainParameterId(std::size_t band);
[[nodiscard]] juce::String soloParameterId(std::size_t band);
[[nodiscard]] juce::String widthParameterId(std::size_t band);

[[nodiscard]] BandGains soloGains(const BandSolos& soloed);
[[nodiscard]] Crossovers orderCrossovers(const Crossovers& requested);
[[nodiscard]] bool sameCrossovers(const Crossovers& first, const Crossovers& second);

[[nodiscard]] float bandGainFromDecibels(float decibels);
[[nodiscard]] float outputGainFromDecibels(float decibels);
[[nodiscard]] float panFromPercent(float percent);
[[nodiscard]] float panPercentFromText(const juce::String& text);
[[nodiscard]] float plainValue(const juce::RangedAudioParameter& parameter);
[[nodiscard]] int sampleRateDoublings(double sampleRate, double referenceSampleRate);

void setParameterInGesture(juce::RangedAudioParameter& parameter, float value);
}  // namespace esgrove::image_control
