#!/bin/bash
set -eo pipefail

# Regenerates the audio fixtures used by the tests and the CI smoke test.
# Every tone is a one second 1 kHz sine with a peak amplitude of 0.5, which is -6.02 dBFS.
# Stereo tones carry the same signal on both channels and measure -6.02 LUFS integrated.
# Mono tones are analyzed as dual mono and therefore measure the same loudness.
# The files are committed so the tests do not depend on ffmpeg.
# Requires ffmpeg with the libmp3lame encoder.

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$DIR"

TONE="0.5*sin(2*PI*1000*t)"

# Renders the tone with the given sample rate and channel count, followed by the output options and file name.
# Stereo tones evaluate the expression once per channel,
# because letting ffmpeg upmix a mono source attenuates each channel by 3 dB.
tone() {
    local sample_rate="$1"
    local channels="$2"
    shift 2
    local expression="$TONE"
    if [ "$channels" -eq 2 ]; then
        expression="${TONE}|${TONE}"
    fi
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i "aevalsrc=${expression}:s=${sample_rate}:d=1" \
        -map_metadata -1 -fflags +bitexact -flags:a +bitexact "$@"
}

tone 44100 2 -c:a pcm_s16le sine_stereo_44100_16.wav
tone 48000 2 -c:a pcm_s24le sine_stereo_48000_24.wav
tone 44100 1 -c:a pcm_s16le sine_mono_44100_16.wav
tone 48000 1 -c:a pcm_s24le sine_mono_48000_24.wav
tone 44100 2 -c:a pcm_f32le sine_stereo_44100_32f.wav
tone 96000 2 -c:a pcm_s16le sine_stereo_96000_16.wav
tone 24000 1 -c:a pcm_s16le sine_mono_24000_16.wav
tone 24000 1 -c:a pcm_s16be sine_mono_24000_16.aif
tone 44100 2 -c:a pcm_s16be sine_stereo_44100_16.aif
tone 48000 1 -c:a pcm_s24be sine_mono_48000_24.aif
tone 44100 2 -c:a flac -sample_fmt s16 sine_stereo_44100_16.flac
tone 48000 2 -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 sine_stereo_48000_24.flac
tone 44100 2 -c:a libmp3lame -b:a 320k sine_stereo_44100_320k.mp3
tone 44100 2 -c:a libmp3lame -b:a 192k -joint_stereo 1 sine_joint_stereo_44100_192k.mp3
tone 44100 1 -c:a libmp3lame -b:a 128k sine_mono_44100_128k.mp3
tone 48000 2 -c:a vorbis -strict experimental -q:a 6 sine_stereo_48000.ogg

# Tagged copy for metadata round-trip tests.
tone 44100 2 -c:a flac -sample_fmt s16 \
    -metadata title="Test Tone" -metadata artist="AudioBatch" -metadata album="Fixtures" \
    -metadata comment="Custom comment" tagged_stereo_44100_16.flac

# Digital silence, which has no peak and no measurable loudness.
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "anullsrc=r=44100:cl=stereo:d=1" \
    -map_metadata -1 -fflags +bitexact -flags:a +bitexact -c:a pcm_s16le silence_stereo_44100_16.wav

# A file with an audio extension that does not contain audio.
printf 'not audio\n' > broken.wav

ls -l "$DIR"
