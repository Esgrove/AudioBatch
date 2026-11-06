/// Declares the parameters of the ImageControl plugin:
/// the three crossover frequencies, the mid and side gain of each of the four bands, and the output gain.
/// The ids, ranges, and defaults live here so the processor, the editor, and the tests share one definition.
/// Also declares the crossover ordering rule that both the DSP and the editor apply,
/// and the names of the editor settings saved with the plugin state.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstddef>

namespace esgrove::image_control
{
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

/// Hz in one kHz, for formatting frequencies.
constexpr float HERTZ_PER_KILOHERTZ {1000.0F};

/// Id of the output gain parameter.
constexpr auto OUTPUT_GAIN_ID {"output_gain"};
/// Id of the bypass parameter, which hosts also use as the plugin's own bypass.
constexpr auto BYPASS_ID {"bypass"};

/// Name of the editor setting that selects the analyzer tap: true for the output, false for the input.
constexpr auto ANALYZE_OUTPUT_PROPERTY {"analyzeOutput"};
/// Name of the editor setting that holds the spectrogram persistence in seconds.
constexpr auto PERSISTENCE_PROPERTY {"persistence"};
/// Name of the editor setting that holds the spectrogram focus, the level difference at the edges, in dB.
constexpr auto FOCUS_PROPERTY {"focus"};
/// Name of the editor setting that selects phase colours instead of frequency colours.
constexpr auto PHASE_COLOURS_PROPERTY {"phaseColours"};
/// Name of the editor setting that selects the mel frequency scale instead of the logarithmic one.
constexpr auto MEL_SCALE_PROPERTY {"melScale"};
/// Name of the editor setting that shows the display refresh rate.
constexpr auto SHOW_FPS_PROPERTY {"showFps"};
/// Name of the editor setting that holds the editor width in pixels.
constexpr auto EDITOR_WIDTH_PROPERTY {"editorWidth"};
/// Name of the editor setting that holds the editor height in pixels.
constexpr auto EDITOR_HEIGHT_PROPERTY {"editorHeight"};

/// Crossover frequencies ordered from the lowest to the highest, in Hz.
using Crossovers = std::array<float, CROSSOVER_COUNT>;
/// One linear gain per band, from the lowest band to the highest.
using BandGains = std::array<float, BAND_COUNT>;

[[nodiscard]] juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
[[nodiscard]] juce::String crossoverParameterId(std::size_t crossover);
[[nodiscard]] juce::String midGainParameterId(std::size_t band);
[[nodiscard]] juce::String sideGainParameterId(std::size_t band);
[[nodiscard]] juce::String bandName(std::size_t band);
[[nodiscard]] juce::Identifier linkPropertyName(std::size_t band);
[[nodiscard]] juce::String formatFrequency(float frequency);

[[nodiscard]] Crossovers orderCrossovers(const Crossovers& requested);

[[nodiscard]] float bandGainFromDecibels(float decibels);
[[nodiscard]] float outputGainFromDecibels(float decibels);
}  // namespace esgrove::image_control
