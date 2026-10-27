#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["mutagen==1.47.0"]
# ///
"""
Generates the normalization fixture collection, which tests normalization and output writing in both applications.

Every file is rendered by ffmpeg from a deterministic music-like signal:
a detuned chord with a slow tremolo, a decaying kick drum every half second, and a short high hat between the kicks.
The right channel uses a slightly different chord and a quieter kick, so swapped or mixed channels show up.
The files cover WAV, AIFF, AIFC float, FLAC, MP3, Ogg Vorbis, and AAC and ALAC in M4A,
at 44.1, 48, and 96 kHz, with 16 and 24 bit integer and 32 bit float samples,
from a fifth of a second to 30 seconds long.
The tags range from none to ID3v2.3 and ID3v2.4 frames with pictures and Serato data,
RIFF INFO chunks, Vorbis comments with pictures and native Serato fields, and iTunes atoms.
Edge cases cover silence, a gain above the 40 dB limit, float samples above full scale,
a file already at full scale, intersample peaks, a non-ASCII file name, and two sources sharing one output name.

Run it with uv, which installs the pinned mutagen version that writes the tags:

    uv run fixtures/normalization/generate.py

Requires ffmpeg with the libmp3lame encoder.
"""

import base64
import struct
import subprocess
import tempfile
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from mutagen.aiff import AIFF
from mutagen.flac import FLAC, Picture
from mutagen.id3 import (
    APIC,
    COMM,
    GEOB,
    ID3,
    POPM,
    PRIV,
    TALB,
    TBPM,
    TCOM,
    TCON,
    TDRC,
    TIT2,
    TKEY,
    TPE1,
    TPE2,
    TPOS,
    TRCK,
    TSRC,
    TXXX,
    USLT,
    WXXX,
)
from mutagen.mp4 import MP4, MP4Cover, MP4FreeForm
from mutagen.oggvorbis import OggVorbis
from mutagen.wave import WAVE

DIRECTORY = Path(__file__).resolve().parent

UNICODE_TITLE = "Äänekoski — Tëst 日本語 ✓"

# Chord partials of the left channel as frequency and amplitude pairs. The right channel is detuned by 0.5%.
CHORD = [(220.0, 0.22), (277.18, 0.18), (329.63, 0.15), (880.0, 0.08)]


@dataclass
class Fixture:
    """One file of the collection: its audio, its encoding, and the tags written onto it."""

    name: str
    sample_rate: int
    channels: int
    duration: float
    codec: list[str]
    gain_db: float = -3.0
    signal: str = "music"
    tags: Callable[[Path], None] | None = None
    metadata: dict[str, str] = field(default_factory=dict)


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


def channel_expression(fixture: Fixture, right: bool) -> str:
    """Returns the expression of one channel for the signal kind of the fixture."""
    music = music_expression(fixture.sample_rate, right)
    if fixture.signal == "silence":
        return "0"
    if fixture.signal == "clipped":
        return f"max(-1\\,min(1\\,2.5*{music}))"
    if fixture.signal == "intersample":
        # A quarter sample rate tone 45 degrees off its peaks hides its true peak between the samples.
        return f"0.6*{music}+0.35*sin(2*PI*{fixture.sample_rate / 4}*t+PI/4)"
    return music


def render(fixture: Fixture, output: Path) -> None:
    """Renders the audio of a fixture with ffmpeg."""
    expressions = [channel_expression(fixture, right=index % 2 == 1) for index in range(fixture.channels)]
    source = f"aevalsrc={'|'.join(expressions)}:s={fixture.sample_rate}:d={fixture.duration}"
    metadata = [argument for key, value in fixture.metadata.items() for argument in ("-metadata", f"{key}={value}")]
    command = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "lavfi",
        "-i",
        source,
        "-af",
        f"volume={fixture.gain_db}dB",
        "-map_metadata",
        "-1",
        "-fflags",
        "+bitexact",
        "-flags:a",
        "+bitexact",
        *metadata,
        *fixture.codec,
        str(output),
    ]
    subprocess.run(command, check=True)


def render_cover(path: Path, size: int, pattern: str) -> bytes:
    """Renders a small test pattern image with ffmpeg and returns its bytes."""
    subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            f"{pattern}=size={size}x{size}:rate=1",
            "-frames:v",
            "1",
            "-fflags",
            "+bitexact",
            "-flags",
            "+bitexact",
            str(path),
        ],
        check=True,
    )
    return path.read_bytes()


WORK_DIRECTORY = Path(tempfile.mkdtemp())
FRONT_COVER = render_cover(WORK_DIRECTORY / "front.jpg", 96, "testsrc2")
BACK_COVER = render_cover(WORK_DIRECTORY / "back.png", 48, "smptebars")


def serato_markers() -> bytes:
    """Returns a Serato Markers2 payload with the BPM lock, the track color, three cue points, and a saved loop."""
    entries = bytearray([0x01, 0x01])

    def entry(name: str, body: bytes) -> None:
        entries.extend(name.encode() + b"\x00" + struct.pack(">I", len(body)) + body)

    entry("BPMLOCK", b"\x01")
    entry("COLOR", b"\x00\xff\x99\xff")
    cues = [(0, 500, b"\xcc\x00\x00", "Intro"), (1, 1500, b"\xcc\x88\x00", "Drop ✓"), (2, 2750, b"\x00\x00\xcc", "")]
    for index, position, color, name in cues:
        entry(
            "CUE",
            b"\x00"
            + bytes([index])
            + struct.pack(">I", position)
            + b"\x00"
            + color
            + b"\x00\x00"
            + name.encode()
            + b"\x00",
        )
    loop = (
        b"\x00\x00"
        + struct.pack(">II", 1000, 2000)
        + b"\xff\xff\xff\xff"
        + b"\x00\x27\xaa\xe1"
        + b"\x00"
        + b"Loop"
        + b"\x00"
    )
    entry("LOOP", loop)
    entries.append(0x00)
    encoded = base64.b64encode(bytes(entries)).rstrip(b"=")
    lines = b"\n".join(encoded[start : start + 72] for start in range(0, len(encoded), 72))
    payload = b"\x01\x01" + lines + b"\x00"
    return payload.ljust(470, b"\x00")


def serato_payloads() -> dict[str, bytes]:
    """Returns the payload of every Serato tag by its GEOB description."""
    beatgrid = (
        b"\x01\x00" + struct.pack(">I", 2) + struct.pack(">fI", 0.05, 4) + struct.pack(">ff", 0.55, 128.0) + b"\x00"
    )
    overview = b"\x01\x05" + b"".join(bytes([0, 1] + [30 + (index % 6)] * 12 + [0, 0]) for index in range(240))
    return {
        "Serato Analysis": b"\x02\x01",
        "Serato Autotags": b"\x01\x01" + b"128.00\x00" + b"-3.257\x00" + b"0.000\x00",
        "Serato BeatGrid": beatgrid,
        "Serato Markers2": serato_markers(),
        "Serato Overview": overview,
    }


NATIVE_SERATO_FIELDS = {
    "Serato Analysis": "SERATO_ANALYSIS",
    "Serato Autotags": "SERATO_AUTOGAIN",
    "Serato BeatGrid": "SERATO_BEATGRID",
    "Serato Markers2": "SERATO_MARKERS_V2",
    "Serato Overview": "SERATO_OVERVIEW",
}


def native_serato_values() -> dict[str, str]:
    """Returns the Vorbis comment fields Serato writes to FLAC files, base64 in lines of 72 characters."""
    values = {}
    for description, payload in serato_payloads().items():
        wrapper = b"application/octet-stream\x00\x00" + description.encode() + b"\x00" + payload
        encoded = base64.b64encode(wrapper).decode("ascii")
        values[NATIVE_SERATO_FIELDS[description]] = "\n".join(
            encoded[start : start + 72] for start in range(0, len(encoded), 72)
        )
    return values


def id3_frames(tag: ID3, title: str, serato: bool, pictures: bool) -> None:
    """Adds the standard frames, optionally with two pictures and the Serato GEOB frames."""
    tag.add(TIT2(encoding=3, text=title))
    tag.add(TPE1(encoding=3, text=["AudioBatch", "Fixture Ensemble"]))
    tag.add(TPE2(encoding=3, text="AudioBatch Album Artist"))
    tag.add(TALB(encoding=3, text="Normalization Fixtures"))
    tag.add(TCON(encoding=3, text="Electronic"))
    tag.add(TDRC(encoding=3, text="2026-10-27"))
    tag.add(TRCK(encoding=3, text="3/12"))
    tag.add(TPOS(encoding=3, text="1/2"))
    tag.add(TBPM(encoding=3, text="128"))
    tag.add(TKEY(encoding=3, text="Am"))
    tag.add(TCOM(encoding=3, text="Composer Name"))
    tag.add(TSRC(encoding=3, text="FIXXX2600001"))
    tag.add(COMM(encoding=3, lang="eng", desc="", text="Normalization fixture comment"))
    tag.add(COMM(encoding=3, lang="fin", desc="note", text="Kommentti ääkkösillä"))
    tag.add(USLT(encoding=3, lang="eng", desc="", text="First line\nSecond line"))
    tag.add(TXXX(encoding=3, desc="MOOD", text="Energetic"))
    tag.add(TXXX(encoding=3, desc="CATALOGNUMBER", text="AB-001"))
    tag.add(WXXX(encoding=3, desc="homepage", url="https://example.com/audiobatch"))
    tag.add(POPM(email="fixture@example.com", rating=196, count=7))
    tag.add(PRIV(owner="com.example.audiobatch", data=b"\x00\x01private\xff"))
    if pictures:
        tag.add(APIC(encoding=3, mime="image/jpeg", type=3, desc="Front", data=FRONT_COVER))
        tag.add(APIC(encoding=3, mime="image/png", type=4, desc="Back", data=BACK_COVER))
    if serato:
        for description, payload in serato_payloads().items():
            tag.add(GEOB(encoding=0, mime="application/octet-stream", filename="", desc=description, data=payload))


def id3_tagger(version: int, title: str, serato: bool = False, pictures: bool = True, v1: int = 0):
    """Returns a tagger that writes an ID3v2 tag of the given version onto an MP3, AIFF, or WAV file."""

    def tag_file(path: Path) -> None:
        if path.suffix == ".mp3":
            tag = ID3()
        else:
            audio = AIFF(path) if path.suffix in (".aif", ".aiff", ".aifc") else WAVE(path)
            audio.add_tags()
            tag = audio.tags
        id3_frames(tag, title, serato, pictures)
        # Saving as ID3v2.3 does not convert the frames, so the date moves from TDRC to TYER and TDAT here.
        if version == 3:
            tag.update_to_v23()
        if path.suffix == ".mp3":
            tag.save(path, v2_version=version, v1=v1)
        else:
            tag.save(path, v2_version=version)

    return tag_file


def vorbis_fields(title: str) -> dict[str, list[str]]:
    """Returns the standard Vorbis comment fields with a few multi-valued and custom ones."""
    return {
        "TITLE": [title],
        "ARTIST": ["AudioBatch", "Fixture Ensemble"],
        "ALBUMARTIST": ["AudioBatch Album Artist"],
        "ALBUM": ["Normalization Fixtures"],
        "GENRE": ["Electronic"],
        "DATE": ["2026-10-27"],
        "TRACKNUMBER": ["3"],
        "TRACKTOTAL": ["12"],
        "DISCNUMBER": ["1"],
        "BPM": ["128"],
        "INITIALKEY": ["Am"],
        "COMPOSER": ["Composer Name"],
        "ISRC": ["FIXXX2600001"],
        "COMMENT": ["Normalization fixture comment"],
        "LYRICS": ["First line\nSecond line"],
        "MOOD": ["Energetic"],
        "CATALOGNUMBER": ["AB-001"],
        "AUDIOBATCH_CUSTOM": ["Custom value", "Second custom value"],
    }


def flac_picture(data: bytes, mime: str, picture_type: int, description: str, size: int) -> Picture:
    """Returns a FLAC picture block for a cover image."""
    picture = Picture()
    picture.data = data
    picture.mime = mime
    picture.type = picture_type
    picture.desc = description
    picture.width = size
    picture.height = size
    picture.depth = 24
    return picture


def flac_tagger(title: str, serato: bool = False, pictures: bool = True):
    """Returns a tagger that writes Vorbis comments, picture blocks, and optionally native Serato fields."""

    def tag_file(path: Path) -> None:
        audio = FLAC(path)
        for key, values in vorbis_fields(title).items():
            audio[key] = values
        if serato:
            for key, value in native_serato_values().items():
                audio[key] = value
        if pictures:
            audio.add_picture(flac_picture(FRONT_COVER, "image/jpeg", 3, "Front", 96))
            audio.add_picture(flac_picture(BACK_COVER, "image/png", 4, "Back", 48))
        audio.save()

    return tag_file


def ogg_tagger(title: str):
    """Returns a tagger that writes Vorbis comments with a front cover in METADATA_BLOCK_PICTURE."""

    def tag_file(path: Path) -> None:
        audio = OggVorbis(path)
        for key, values in vorbis_fields(title).items():
            audio[key] = values
        picture = flac_picture(FRONT_COVER, "image/jpeg", 3, "Front", 96)
        audio["METADATA_BLOCK_PICTURE"] = [base64.b64encode(picture.write()).decode("ascii")]
        audio.save()

    return tag_file


def mp4_tagger(title: str):
    """Returns a tagger that writes iTunes atoms, a cover, and freeform fields onto an M4A file."""

    def tag_file(path: Path) -> None:
        audio = MP4(path)
        audio["\xa9nam"] = [title]
        audio["\xa9ART"] = ["AudioBatch"]
        audio["aART"] = ["AudioBatch Album Artist"]
        audio["\xa9alb"] = ["Normalization Fixtures"]
        audio["\xa9gen"] = ["Electronic"]
        audio["\xa9day"] = ["2026-10-27"]
        audio["trkn"] = [(3, 12)]
        audio["disk"] = [(1, 2)]
        audio["tmpo"] = [128]
        audio["\xa9wrt"] = ["Composer Name"]
        audio["\xa9cmt"] = ["Normalization fixture comment"]
        audio["\xa9lyr"] = ["First line\nSecond line"]
        audio["covr"] = [MP4Cover(FRONT_COVER, imageformat=MP4Cover.FORMAT_JPEG)]
        audio["----:com.apple.iTunes:MOOD"] = [MP4FreeForm(b"Energetic")]
        audio["----:com.apple.iTunes:initialkey"] = [MP4FreeForm(b"Am")]
        audio.save()

    return tag_file


WAV_16 = ["-c:a", "pcm_s16le"]
WAV_24 = ["-c:a", "pcm_s24le"]
WAV_32F = ["-c:a", "pcm_f32le"]
AIFF_16 = ["-c:a", "pcm_s16be"]
AIFF_24 = ["-c:a", "pcm_s24be"]
AIFF_32F = ["-c:a", "pcm_f32be"]
FLAC_16 = ["-c:a", "flac", "-sample_fmt", "s16"]
FLAC_24 = ["-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24"]
NO_ID3 = ["-id3v2_version", "0", "-write_id3v1", "0"]

FIXTURES = [
    # WAV
    Fixture("wav_stereo_44100_16_untagged_5s.wav", 44100, 2, 5, WAV_16, gain_db=-6),
    Fixture(
        "wav_stereo_48000_24_id3v24_6s.wav",
        48000,
        2,
        6,
        WAV_24,
        gain_db=-9,
        tags=id3_tagger(4, "WAV ID3v2.4 Tags"),
    ),
    Fixture(
        "wav_stereo_96000_24_riff_info_3s.wav",
        96000,
        2,
        3,
        WAV_24,
        gain_db=-4,
        metadata={"title": "RIFF INFO Title", "artist": "AudioBatch", "comment": "RIFF INFO comment", "genre": "Test"},
    ),
    Fixture("wav_stereo_44100_32f_above_full_scale_4s.wav", 44100, 2, 4, WAV_32F, gain_db=4),
    Fixture("wav_stereo_96000_32f_2s.wav", 96000, 2, 2, WAV_32F, gain_db=-12),
    Fixture("wav_mono_48000_16_quiet_8s.wav", 48000, 1, 8, WAV_16, gain_db=-30),
    # AIFF
    Fixture(
        "aiff_stereo_44100_16_id3v23_unicode_6s.aif",
        44100,
        2,
        6,
        AIFF_16,
        gain_db=-7,
        tags=id3_tagger(3, UNICODE_TITLE),
    ),
    Fixture(
        "aiff_stereo_48000_24_serato_8s.aif",
        48000,
        2,
        8,
        AIFF_24,
        gain_db=-5,
        tags=id3_tagger(4, "AIFF Serato", serato=True),
    ),
    Fixture("aiff_stereo_96000_24_untagged_2s.aif", 96000, 2, 2, AIFF_24, gain_db=-10),
    Fixture(
        "aiff_stereo_44100_32f_tagged_6s.aif",
        44100,
        2,
        6,
        AIFF_32F,
        gain_db=-8,
        tags=id3_tagger(4, "AIFC Float Tags", pictures=False),
    ),
    Fixture("aiff_mono_96000_32f_1s.aifc", 96000, 1, 1, AIFF_32F, gain_db=-2),
    Fixture("aiff_mono_44100_24_full_scale_3s.aif", 44100, 1, 3, AIFF_24, signal="clipped", gain_db=0),
    # FLAC
    Fixture(
        "flac_stereo_44100_16_vorbis_pictures_30s.flac",
        44100,
        2,
        30,
        FLAC_16,
        gain_db=-6,
        tags=flac_tagger("FLAC Thirty Seconds"),
    ),
    Fixture(
        "flac_stereo_48000_24_native_serato_12s.flac",
        48000,
        2,
        12,
        FLAC_24,
        gain_db=-4,
        tags=flac_tagger("FLAC Native Serato", serato=True),
    ),
    Fixture("flac_stereo_96000_24_untagged_5s.flac", 96000, 2, 5, FLAC_24, gain_db=-11),
    Fixture(
        "flac_mono_44100_16_unicode_10s.flac",
        44100,
        1,
        10,
        FLAC_16,
        gain_db=-14,
        tags=flac_tagger(UNICODE_TITLE, pictures=False),
    ),
    Fixture("flac_stereo_48000_24_intersample_4s.flac", 48000, 2, 4, FLAC_24, signal="intersample", gain_db=-1),
    # MP3
    Fixture(
        "mp3_stereo_44100_320k_id3v24_serato_30s.mp3",
        44100,
        2,
        30,
        [*NO_ID3, "-c:a", "libmp3lame", "-b:a", "320k"],
        gain_db=-5,
        tags=id3_tagger(4, "MP3 Serato", serato=True),
    ),
    Fixture(
        "mp3_stereo_48000_vbr_id3v23_v1_20s.mp3",
        48000,
        2,
        20,
        [*NO_ID3, "-c:a", "libmp3lame", "-q:a", "2"],
        gain_db=-8,
        tags=id3_tagger(3, "MP3 VBR ID3v2.3", v1=2),
    ),
    Fixture(
        "mp3_mono_44100_128k_untagged_3s.mp3",
        44100,
        1,
        3,
        [*NO_ID3, "-c:a", "libmp3lame", "-b:a", "128k"],
        gain_db=-10,
    ),
    Fixture(
        "mp3_stereo_44100_192k_no_xing_header_5s.mp3",
        44100,
        2,
        5,
        [*NO_ID3, "-write_xing", "0", "-c:a", "libmp3lame", "-b:a", "192k"],
        gain_db=-6,
    ),
    # Ogg Vorbis
    Fixture(
        "ogg_stereo_44100_q5_tagged_15s.ogg",
        44100,
        2,
        15,
        ["-c:a", "vorbis", "-strict", "experimental", "-q:a", "5"],
        gain_db=-7,
        tags=ogg_tagger("Ogg Vorbis Tags"),
    ),
    Fixture(
        "ogg_stereo_48000_q3_untagged_10s.ogg",
        48000,
        2,
        10,
        ["-c:a", "vorbis", "-strict", "experimental", "-q:a", "3"],
        gain_db=-12,
    ),
    # M4A
    Fixture(
        "m4a_stereo_44100_aac_256k_tagged_20s.m4a",
        44100,
        2,
        20,
        ["-c:a", "aac", "-b:a", "256k"],
        gain_db=-6,
        tags=mp4_tagger("M4A AAC Tags"),
    ),
    Fixture(
        "m4a_stereo_48000_alac_24_tagged_6s.m4a",
        48000,
        2,
        6,
        ["-c:a", "alac", "-sample_fmt", "s32p"],
        gain_db=-9,
        tags=mp4_tagger("M4A ALAC Tags"),
    ),
    # Edge cases
    Fixture("edge_silence_stereo_44100_16_3s.wav", 44100, 2, 3, WAV_16, signal="silence"),
    Fixture("edge_gain_above_limit_stereo_44100_16_2s.wav", 44100, 2, 2, WAV_16, gain_db=-48),
    Fixture("edge_short_stereo_44100_16_0.2s.wav", 44100, 2, 0.2, WAV_16, gain_db=-6),
    Fixture("edge_loud_clipped_stereo_44100_16_5s.wav", 44100, 2, 5, WAV_16, signal="clipped", gain_db=0),
    Fixture("edge_ääni_ö_stereo_48000_16_4s.flac", 48000, 2, 4, FLAC_16, gain_db=-9),
    Fixture("edge_shared_output_44100_16_2s.wav", 44100, 2, 2, WAV_16, gain_db=-6),
    Fixture("edge_shared_output_44100_16_2s.flac", 44100, 2, 2, FLAC_16, gain_db=-8),
]


def main() -> None:
    for fixture in FIXTURES:
        output = DIRECTORY / fixture.name
        render(fixture, output)
        if fixture.tags is not None:
            fixture.tags(output)
        print(f"{output.stat().st_size / 1024:8.0f} KB  {fixture.name}")
    total = sum((DIRECTORY / fixture.name).stat().st_size for fixture in FIXTURES)
    print(f"{total / 1024 / 1024:8.1f} MB  total in {len(FIXTURES)} files")


if __name__ == "__main__":
    main()
