#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Generates the long audio files the Rust benchmarks decode and analyze, into fixtures/benchmark/output.

The files are 60 seconds long, so a benchmark measures steady throughput instead of setup costs.
They are not committed, since they take about a gigabyte, so run this script once before benchmarking.
Every rate the true peak oversampling treats differently gets a music-like stereo file in WAV and FLAC
at 16 and 24 bits, and 44.1 and 48 kHz also get MP3, AAC, and Ogg Vorbis, the rates those are used at.
A mono set covers the dual mono path, white noise is the worst case for FLAC decoding,
and a long fade into silence produces the subnormal values that slow down filters on some machines.
Files that already exist are kept, so run with --force to render them again.

Usage: uv run fixtures/benchmark/generate.py [--force]
"""

import argparse
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

OUTPUT_DIRECTORY = Path(__file__).resolve().parent / "output"
DURATION_SECONDS = 60
GAIN_DB = -3.0

# The music-like signal of fixtures/normalization/generate.py, copied since importing that script renders its covers.
# Chord partials of the left channel as frequency and amplitude pairs. The right channel is detuned by 0.5%.
CHORD = [(220.0, 0.22), (277.18, 0.18), (329.63, 0.15), (880.0, 0.08)]

# Rates around every change of the true peak oversampling: 4x below 96 kHz, 2x below 192 kHz, then none.
LOSSLESS_RATES = [44_100, 48_000, 88_200, 96_000, 192_000]
LOSSY_RATES = [44_100, 48_000]

LOSSLESS_CODECS = {
    "16.wav": ["-c:a", "pcm_s16le"],
    "24.wav": ["-c:a", "pcm_s24le"],
    "16.flac": ["-c:a", "flac", "-sample_fmt", "s16"],
    "24.flac": ["-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24"],
}
LOSSY_CODECS = {
    "320k.mp3": ["-c:a", "libmp3lame", "-b:a", "320k"],
    "256k.m4a": ["-c:a", "aac", "-b:a", "256k"],
    # The native encoder, which every ffmpeg build has, unlike libvorbis.
    "q5.ogg": ["-c:a", "vorbis", "-strict", "experimental", "-q:a", "5"],
}


@dataclass
class Source:
    """One rendered signal, which every codec of the set encodes."""

    signal: str
    sample_rate: int
    channels: int
    codecs: dict[str, list[str]]

    def stem(self) -> str:
        """Returns the file name without the codec suffix."""
        layout = "mono" if self.channels == 1 else "stereo"
        return f"{self.signal}_{layout}_{self.sample_rate}_"


def music_expression(sample_rate: int, right: bool) -> str:
    """Returns the aevalsrc expression of one channel of the music-like signal, scaled to peak near full scale."""
    detune = 1.005 if right else 1.0
    kick_level = 0.4 if right else 0.5
    chord = "+".join(f"{amplitude}*sin(2*PI*{frequency * detune:.3f}*t)" for frequency, amplitude in CHORD)
    tremolo = "(0.65+0.35*sin(2*PI*1.5*t))"
    kick = f"{kick_level}*exp(-14*mod(t\\,0.5))*sin(2*PI*55*t)"
    hat_frequency = min(6000, sample_rate // 8)
    hat = f"0.05*exp(-40*mod(t+0.25\\,0.5))*sin(2*PI*{hat_frequency}*t)"
    return f"0.8*(({chord})*{tremolo}+{kick}+{hat})"


def channel_expression(signal: str, sample_rate: int, right: bool) -> str:
    """Returns the expression of one channel for the signal kind."""
    music = music_expression(sample_rate, right)
    if signal == "noise":
        return "0.5*(random(0)*2-1)" if not right else "0.5*(random(1)*2-1)"
    if signal == "fade":
        # Ten seconds of music, then an exponential fade far below the 16 bit floor, into subnormal floats.
        return f"{music}*if(lt(t\\,10)\\,1\\,exp(-1.5*(t-10)))"
    return music


def sources() -> list[Source]:
    """Returns every signal of the set with the codecs that encode it."""
    result = []
    for rate in LOSSLESS_RATES:
        codecs = dict(LOSSLESS_CODECS)
        if rate in LOSSY_RATES:
            codecs |= LOSSY_CODECS
        result.append(Source("music", rate, 2, codecs))
    # The native Vorbis encoder only writes stereo, so the mono set has no Ogg file.
    mono_codecs = {suffix: codec for suffix, codec in LOSSY_CODECS.items() if not suffix.endswith(".ogg")}
    result.extend(Source("music", rate, 1, {**LOSSLESS_CODECS, **mono_codecs}) for rate in LOSSY_RATES)
    result.append(Source("noise", 44_100, 2, LOSSLESS_CODECS))
    result.append(Source("fade", 44_100, 2, {"24.wav": LOSSLESS_CODECS["24.wav"]}))
    return result


def ffmpeg(*arguments: str) -> None:
    """Runs ffmpeg quietly with bit exact output."""
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *arguments]
    subprocess.run(command, check=True)


def render_master(source: Source, master: Path) -> None:
    """Renders the signal once as 32 bit float, which every codec then encodes."""
    expressions = [
        channel_expression(source.signal, source.sample_rate, right=index == 1) for index in range(source.channels)
    ]
    lavfi = f"aevalsrc={'|'.join(expressions)}:s={source.sample_rate}:d={DURATION_SECONDS}"
    ffmpeg("-f", "lavfi", "-i", lavfi, "-af", f"volume={GAIN_DB}dB", "-c:a", "pcm_f32le", str(master))


def main() -> None:
    """Renders every missing file of the set."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-f", "--force", action="store_true", help="Render the files that already exist again")
    arguments = parser.parse_args()

    OUTPUT_DIRECTORY.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory() as work_directory:
        for source in sources():
            outputs = {suffix: OUTPUT_DIRECTORY / f"{source.stem()}{suffix}" for suffix in source.codecs}
            missing = {suffix: path for suffix, path in outputs.items() if arguments.force or not path.exists()}
            if not missing:
                continue
            master = Path(work_directory) / f"{source.stem()}master.wav"
            render_master(source, master)
            for suffix, path in missing.items():
                print(f"Writing {path.name}")
                ffmpeg(
                    "-i",
                    str(master),
                    "-map_metadata",
                    "-1",
                    "-fflags",
                    "+bitexact",
                    "-flags:a",
                    "+bitexact",
                    *source.codecs[suffix],
                    str(path),
                )
            master.unlink()


if __name__ == "__main__":
    main()
