# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Generate and validate the version-1 shared ImageControl GPU benchmark workload.

The exact binary layout is little endian: <8sIIff, then frame_count * particles_per_frame records of <8f.
The 24-byte header contains b"ICRB0001", frame_count, particles_per_frame, step_seconds, persistence_seconds.
Each 32-byte particle is x, y, spread, red, green, blue, size, emphasis, without padding or frame delimiters.
Frame records are contiguous and equal sized. Runners cycle absolute frame indices modulo frame_count,
including warmup, and continue that sequence for measurement. No runtime RNG or native float regeneration is needed.
The deterministic integer generator writes identical frames, allowing a compact one-frame default input.
The sidecar manifest records exact byte SHA-256 and FNV-1a identities, generator parameters, and binary metadata.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from dataclasses import asdict, dataclass
from pathlib import Path

MAGIC = b"ICRB0001"
HEADER = struct.Struct("<8sIIff")
PARTICLE = struct.Struct("<8f")
STANDARD_PARTICLE_COUNTS = (0, 4096, 16_384, 65_536)
MAX_PARTICLES = 65_536
MAX_INPUT_FRAMES = 240
MAX_WORKLOAD_BYTES = 256 * 1024 * 1024
DEFAULT_SEED = 0x1C202601
DEFAULT_STEP = 1.0 / 60.0
MAX_STEP = 0.1
MAX_STORED_STEP = struct.unpack("<f", struct.pack("<f", MAX_STEP))[0]
DEFAULT_PERSISTENCE = 1.0
MIN_PERSISTENCE = 0.05
MAX_PERSISTENCE = 3.0
UINT32_MAX = 0xFFFFFFFF
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
UINT64_MASK = 0xFFFFFFFFFFFFFFFF


@dataclass(frozen=True)
class WorkloadHeader:
    """Validated binary fields, preserving the stored float32 parameter values."""

    frame_count: int
    particles_per_frame: int
    step_seconds: float
    persistence_seconds: float


def require(condition: bool, message: str) -> None:
    """Fail with one descriptive contract error instead of silently repairing malformed evidence."""
    if not condition:
        raise ValueError(message)


def write_json(path: Path, value: object) -> None:
    """Write standard JSON without NaN or Infinity and with a stable trailing newline."""
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def canonical_hash(value: object) -> str:
    """Hash a manifest object using sorted compact UTF-8 JSON, independently of pretty-print whitespace."""
    data = json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def fnv1a64(data: bytes) -> str:
    """Provide the simple exact input identity shared by both dependency-free native readers."""
    result = FNV_OFFSET
    for byte in data:
        result = ((result ^ byte) * FNV_PRIME) & UINT64_MASK
    return f"{result:016x}"


def validate_parameters(header: WorkloadHeader) -> None:
    """Bound storage and reject invalid renderer parameters before packing or allocating."""
    require(type(header.frame_count) is int and 1 <= header.frame_count <= MAX_INPUT_FRAMES, "invalid frame_count")
    require(
        type(header.particles_per_frame) is int and 0 <= header.particles_per_frame <= MAX_PARTICLES,
        "invalid particles_per_frame",
    )
    for name, value in (("step_seconds", header.step_seconds), ("persistence_seconds", header.persistence_seconds)):
        require(type(value) in (int, float) and math.isfinite(value) and value > 0, f"invalid {name}")
    require(header.step_seconds <= MAX_STORED_STEP, "step_seconds exceeds the shared 0.1-second production limit")
    require(
        MIN_PERSISTENCE <= header.persistence_seconds <= MAX_PERSISTENCE,
        "persistence_seconds must be between 0.05 and 3.0",
    )
    for name, value in (("step_seconds", header.step_seconds), ("persistence_seconds", header.persistence_seconds)):
        stored = struct.unpack("<f", struct.pack("<f", value))[0]
        require(math.isfinite(stored) and stored > 0, f"invalid float32 {name}")
    size = HEADER.size + header.frame_count * header.particles_per_frame * PARTICLE.size
    require(size <= MAX_WORKLOAD_BYTES, "workload exceeds storage limit")


def generate_bytes(header: WorkloadHeader, seed: int = DEFAULT_SEED) -> bytes:
    """Derive particle channels from an unsigned LCG and exact powers-of-two fractions, not a library RNG."""
    validate_parameters(header)
    require(header.step_seconds <= MAX_STEP, "requested step_seconds exceeds the shared 0.1-second production limit")
    require(type(seed) is int and 0 <= seed <= UINT32_MAX, "seed must be an unsigned 32-bit integer")
    state = seed
    records = bytearray()
    for _ in range(header.particles_per_frame):
        channels = []
        for _ in range(8):
            state = (1_664_525 * state + 1_013_904_223) & UINT32_MAX
            channels.append((state >> 8) * (1.0 / (1 << 24)))
        x, y, spread, red, green, blue, size, emphasis = channels
        records.extend(PARTICLE.pack(x, y, (1 + int(spread * 16)) / 4096, red, green, blue, 0.5 + size * 0.5, emphasis))
    return (
        HEADER.pack(
            MAGIC, header.frame_count, header.particles_per_frame, header.step_seconds, header.persistence_seconds
        )
        + bytes(records) * header.frame_count
    )


def decode_bytes(data: bytes) -> WorkloadHeader:
    """Validate exact length, little-endian metadata, and every finite particle before a native process runs."""
    require(len(data) >= HEADER.size, "truncated workload header")
    magic, frames, particles, step, persistence = HEADER.unpack_from(data)
    require(magic == MAGIC, "unsupported workload magic or version")
    header = WorkloadHeader(frames, particles, step, persistence)
    validate_parameters(header)
    expected = HEADER.size + frames * particles * PARTICLE.size
    require(len(data) == expected, "workload length does not match frame records")
    for particle in PARTICLE.iter_unpack(memoryview(data)[HEADER.size :]):
        require(all(math.isfinite(value) for value in particle), "non-finite particle channel")
        require(all(0 <= value <= 1 for value in particle), "particle channel outside normalized range")
        require(particle[6] > 0, "particle size must be positive")
    return header


def write_workload(path: Path, header: WorkloadHeader, seed: int = DEFAULT_SEED) -> dict[str, object]:
    """Write bounded binary evidence and a self-hashed sidecar manifest, never touching installed plugins."""
    data = generate_bytes(header, seed)
    stored = decode_bytes(data)
    manifest = {
        "schema_version": 1,
        "generator": "imagecontrol-render-lcg32-v1",
        "seed": seed,
        "header": asdict(stored),
        "layout": {"magic": MAGIC.decode("ascii"), "header_bytes": HEADER.size, "particle_bytes": PARTICLE.size},
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "fnv1a64": fnv1a64(data),
    }
    manifest["manifest_sha256"] = canonical_hash(manifest)
    path.write_bytes(data)
    write_json(path.with_suffix(path.suffix + ".json"), manifest)
    return manifest


def parser() -> argparse.ArgumentParser:
    """Expose no-side-effect help and paired short/long options for standalone workload generation."""
    result = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    result.add_argument("-o", "--output", type=Path, required=True, help="new workload binary path and .json sidecar")
    result.add_argument("-p", "--particles", type=int, default=4096, help="particles per frame, 0 through 65536")
    result.add_argument("-f", "--frames", type=int, default=1, help="stored frames, cycled by native runners")
    result.add_argument("-s", "--seed", type=int, default=DEFAULT_SEED, help="unsigned 32-bit generator seed")
    result.add_argument("-S", "--step", type=float, default=DEFAULT_STEP, help="simulated frame step in seconds")
    result.add_argument("-P", "--persistence", type=float, default=DEFAULT_PERSISTENCE, help="persistence in seconds")
    return result


def main(arguments: list[str] | None = None) -> int:
    """Generate only explicitly requested files, rejecting existing destinations and invalid parameters."""
    command = parser()
    options = command.parse_args(arguments)
    try:
        require(not options.output.exists(), "output already exists")
        require(not options.output.with_suffix(options.output.suffix + ".json").exists(), "manifest already exists")
        header = WorkloadHeader(options.frames, options.particles, options.step, options.persistence)
        validate_parameters(header)
        write_workload(options.output, header, options.seed)
    except (ValueError, OSError) as error:
        command.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
