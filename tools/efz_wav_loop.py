#!/usr/bin/env python3
"""Inspect and write EFZ-compatible WAV loop metadata.

EFZ does not use a separate loop table. It reads `cue ` and `LIST/adtl/ltxt`
metadata from near the end of the WAV and then seeks back to the loop start
after the intro finishes.

This tool supports:
  - `inspect`: show audio format, chunk layout, and the loop points EFZ will use
  - `set`: append or replace EFZ loop metadata on a PCM WAV

The implementation mirrors the decompiled EXE behavior closely:
  - EFZ only scans the last 320 bytes for `cue` and `adtl`
  - EFZ multiplies the stored sample counts by 4, so looped files should be
    16-bit stereo PCM (4 bytes per sample frame)
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
from dataclasses import dataclass
from typing import Iterable, List, Optional


@dataclass
class Chunk:
    chunk_id: bytes
    data: bytes
    data_offset: int

    @property
    def size(self) -> int:
        return len(self.data)


@dataclass
class WavInfo:
    path: pathlib.Path
    riff_size: int
    chunks: List[Chunk]
    channels: int
    sample_rate: int
    byte_rate: int
    block_align: int
    bits_per_sample: int
    audio_format: int
    data_size: int

    @property
    def duration_seconds(self) -> float:
        if not self.byte_rate:
            return 0.0
        return self.data_size / float(self.byte_rate)


@dataclass
class EfzLoop:
    cue_position_samples: int
    loop_length_samples: int

    @property
    def loop_start_seconds(self) -> float:
        raise RuntimeError("sample rate required to compute seconds")


def read_le_u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_wav(path: pathlib.Path) -> WavInfo:
    raw = path.read_bytes()
    if len(raw) < 12 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a RIFF/WAVE file")

    riff_size = read_le_u32(raw, 4)
    offset = 12
    chunks: List[Chunk] = []
    audio_format = channels = sample_rate = byte_rate = block_align = bits_per_sample = 0
    data_size = 0

    while offset + 8 <= len(raw):
        chunk_id = raw[offset : offset + 4]
        chunk_size = read_le_u32(raw, offset + 4)
        data_offset = offset + 8
        data_end = data_offset + chunk_size
        if data_end > len(raw):
            raise ValueError(f"{path} has a truncated chunk at 0x{offset:X}")

        data = raw[data_offset:data_end]
        chunks.append(Chunk(chunk_id=chunk_id, data=data, data_offset=data_offset))

        if chunk_id == b"fmt ":
            if chunk_size < 16:
                raise ValueError(f"{path} has an invalid fmt chunk")
            audio_format, channels, sample_rate, byte_rate, block_align, bits_per_sample = struct.unpack_from(
                "<HHIIHH", data, 0
            )
        elif chunk_id == b"data":
            data_size = chunk_size

        offset = data_end + (chunk_size & 1)

    return WavInfo(
        path=path,
        riff_size=riff_size,
        chunks=chunks,
        channels=channels,
        sample_rate=sample_rate,
        byte_rate=byte_rate,
        block_align=block_align,
        bits_per_sample=bits_per_sample,
        audio_format=audio_format,
        data_size=data_size,
    )


def find_engine_visible_loop(raw: bytes) -> Optional[EfzLoop]:
    tail = raw[-320:] if len(raw) > 320 else raw
    cue_position = 0
    loop_length = 0

    for i in range(len(tail)):
        if i + 20 <= len(tail) and tail[i : i + 3] == b"cue":
            cue_position = read_le_u32(tail, i + 16)
        elif i + 20 <= len(tail) and tail[i : i + 4] == b"adtl":
            loop_length = read_le_u32(tail, i + 16)

    if cue_position and loop_length:
        return EfzLoop(cue_position_samples=cue_position, loop_length_samples=loop_length)
    return None


def find_riff_loop(chunks: Iterable[Chunk]) -> Optional[EfzLoop]:
    cue_position = 0
    loop_length = 0

    for chunk in chunks:
        if chunk.chunk_id == b"cue " and len(chunk.data) >= 28:
            cue_position = read_le_u32(chunk.data, 8)
        elif chunk.chunk_id == b"LIST" and len(chunk.data) >= 24 and chunk.data[:4] == b"adtl":
            ltxt_index = chunk.data.find(b"ltxt")
            if ltxt_index != -1 and ltxt_index + 20 <= len(chunk.data):
                loop_length = read_le_u32(chunk.data, ltxt_index + 12)

    if cue_position and loop_length:
        return EfzLoop(cue_position_samples=cue_position, loop_length_samples=loop_length)
    return None


def seconds_from_samples(samples: int, sample_rate: int) -> float:
    if sample_rate <= 0:
        return 0.0
    return samples / float(sample_rate)


def build_cue_chunk(loop_start_samples: int) -> bytes:
    data = struct.pack(
        "<III4sIII",
        1,  # dwCuePoints
        1,  # cue ID
        loop_start_samples,  # dwPosition
        b"data",
        0,  # dwChunkStart
        0,  # dwBlockStart
        loop_start_samples,  # dwSampleOffset
    )
    return b"cue " + struct.pack("<I", len(data)) + data


def build_adtl_chunk(loop_length_samples: int) -> bytes:
    ltxt_data = struct.pack(
        "<II4sHHHH",
        1,  # cue ID
        loop_length_samples,  # dwSampleLength
        b"rgn ",
        0,
        0,
        0,
        0,
    )
    ltxt = b"ltxt" + struct.pack("<I", len(ltxt_data)) + ltxt_data
    list_data = b"adtl" + ltxt
    return b"LIST" + struct.pack("<I", len(list_data)) + list_data


def pad_chunk(chunk: bytes) -> bytes:
    return chunk + (b"\x00" if len(chunk) & 1 else b"")


def should_strip_chunk(chunk: Chunk) -> bool:
    if chunk.chunk_id == b"cue ":
        return True
    if chunk.chunk_id == b"LIST" and chunk.data[:4] == b"adtl":
        return True
    return False


def rewrite_with_loop(
    wav: WavInfo,
    raw: bytes,
    loop_start_samples: int,
    loop_length_samples: int,
    out_path: pathlib.Path,
) -> None:
    kept_chunks = [chunk for chunk in wav.chunks if not should_strip_chunk(chunk)]
    rebuilt = bytearray(b"RIFF\x00\x00\x00\x00WAVE")

    for chunk in kept_chunks:
        rebuilt.extend(chunk.chunk_id)
        rebuilt.extend(struct.pack("<I", len(chunk.data)))
        rebuilt.extend(chunk.data)
        if len(chunk.data) & 1:
            rebuilt.append(0)

    rebuilt.extend(pad_chunk(build_cue_chunk(loop_start_samples)))
    rebuilt.extend(pad_chunk(build_adtl_chunk(loop_length_samples)))
    struct.pack_into("<I", rebuilt, 4, len(rebuilt) - 8)
    out_path.write_bytes(rebuilt)


def format_chunks(wav: WavInfo) -> str:
    lines = []
    for chunk in wav.chunks:
        chunk_id = chunk.chunk_id.decode("ascii", errors="replace")
        lines.append(f"  {chunk_id!r} data_off=0x{chunk.data_offset:X} size={chunk.size}")
    return "\n".join(lines)


def inspect_command(args: argparse.Namespace) -> int:
    path = pathlib.Path(args.wav)
    raw = path.read_bytes()
    wav = parse_wav(path)
    engine_loop = find_engine_visible_loop(raw)
    riff_loop = find_riff_loop(wav.chunks)

    print(f"File: {path}")
    print(
        "Format: "
        f"audio_format={wav.audio_format} channels={wav.channels} "
        f"sample_rate={wav.sample_rate} byte_rate={wav.byte_rate} "
        f"block_align={wav.block_align} bits={wav.bits_per_sample}"
    )
    print(f"Duration: {wav.duration_seconds:.3f}s")
    print("Chunks:")
    print(format_chunks(wav))

    if riff_loop:
        start = seconds_from_samples(riff_loop.cue_position_samples, wav.sample_rate)
        end = seconds_from_samples(
            riff_loop.cue_position_samples + riff_loop.loop_length_samples, wav.sample_rate
        )
        length = seconds_from_samples(riff_loop.loop_length_samples, wav.sample_rate)
        print(
            "RIFF loop metadata: "
            f"start_sample={riff_loop.cue_position_samples} "
            f"length_samples={riff_loop.loop_length_samples} "
            f"start={start:.3f}s end={end:.3f}s length={length:.3f}s"
        )
    else:
        print("RIFF loop metadata: none found")

    if engine_loop:
        start = seconds_from_samples(engine_loop.cue_position_samples, wav.sample_rate)
        end = seconds_from_samples(
            engine_loop.cue_position_samples + engine_loop.loop_length_samples, wav.sample_rate
        )
        length = seconds_from_samples(engine_loop.loop_length_samples, wav.sample_rate)
        print(
            "EFZ-visible loop metadata: "
            f"start_sample={engine_loop.cue_position_samples} "
            f"length_samples={engine_loop.loop_length_samples} "
            f"start={start:.3f}s end={end:.3f}s length={length:.3f}s"
        )
    else:
        print("EFZ-visible loop metadata: none found in the last 320 bytes")

    if wav.audio_format != 1:
        print("Warning: EFZ only loads PCM WAV cleanly.", file=sys.stderr)
    if wav.channels != 2 or wav.bits_per_sample != 16 or wav.block_align != 4:
        print(
            "Warning: EFZ hardcodes loop math as samples * 4. Use 16-bit stereo PCM for correct looping.",
            file=sys.stderr,
        )
    return 0


def resolve_loop_samples(args: argparse.Namespace, wav: WavInfo) -> tuple[int, int]:
    if args.start_samples is not None:
        loop_start_samples = args.start_samples
        if args.length_samples is not None:
            loop_length_samples = args.length_samples
        else:
            loop_length_samples = args.end_samples - args.start_samples
    else:
        if wav.sample_rate <= 0:
            raise ValueError("Cannot convert seconds without a valid sample rate")
        loop_start_samples = int(round(args.start_seconds * wav.sample_rate))
        loop_end_samples = int(round(args.end_seconds * wav.sample_rate))
        loop_length_samples = loop_end_samples - loop_start_samples

    if loop_start_samples < 0 or loop_length_samples <= 0:
        raise ValueError("Loop start must be >= 0 and loop length must be > 0")
    return loop_start_samples, loop_length_samples


def set_command(args: argparse.Namespace) -> int:
    in_path = pathlib.Path(args.input_wav)
    out_path = pathlib.Path(args.output_wav)
    raw = in_path.read_bytes()
    wav = parse_wav(in_path)

    if wav.audio_format != 1:
        raise ValueError("EFZ expects PCM WAV files")
    if not args.force and (wav.channels != 2 or wav.bits_per_sample != 16 or wav.block_align != 4):
        raise ValueError(
            "EFZ loop math assumes 16-bit stereo PCM. Use --force only if you know the file is compatible."
        )

    loop_start_samples, loop_length_samples = resolve_loop_samples(args, wav)
    rewrite_with_loop(wav, raw, loop_start_samples, loop_length_samples, out_path)

    print(f"Wrote {out_path}")
    print(
        f"Loop start: {loop_start_samples} samples ({seconds_from_samples(loop_start_samples, wav.sample_rate):.3f}s)"
    )
    print(
        "Loop end: "
        f"{loop_start_samples + loop_length_samples} samples "
        f"({seconds_from_samples(loop_start_samples + loop_length_samples, wav.sample_rate):.3f}s)"
    )
    print(
        f"Loop length: {loop_length_samples} samples ({seconds_from_samples(loop_length_samples, wav.sample_rate):.3f}s)"
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser("inspect", help="Inspect WAV chunks and EFZ loop metadata")
    inspect_parser.add_argument("wav", help="Input WAV file")
    inspect_parser.set_defaults(func=inspect_command)

    set_parser = subparsers.add_parser("set", help="Write EFZ loop metadata to a WAV copy")
    set_parser.add_argument("input_wav", help="Input WAV file")
    set_parser.add_argument("output_wav", help="Output WAV file")

    sample_group = set_parser.add_argument_group("sample-based loop")
    sample_group.add_argument("--start-samples", type=int, help="Loop start in samples")
    sample_group.add_argument("--end-samples", type=int, help="Loop end in samples")
    sample_group.add_argument("--length-samples", type=int, help="Loop length in samples")

    second_group = set_parser.add_argument_group("time-based loop")
    second_group.add_argument("--start-seconds", type=float, help="Loop start in seconds")
    second_group.add_argument("--end-seconds", type=float, help="Loop end in seconds")

    set_parser.add_argument(
        "--force",
        action="store_true",
        help="Allow writing metadata even if the WAV is not 16-bit stereo PCM",
    )
    set_parser.set_defaults(func=set_command)
    return parser


def validate_args(args: argparse.Namespace) -> None:
    if args.command != "set":
        return

    sample_mode = args.start_samples is not None
    second_mode = args.start_seconds is not None

    if sample_mode == second_mode:
        raise ValueError("Use either sample-based or time-based loop arguments")

    if sample_mode:
        if args.end_samples is None and args.length_samples is None:
            raise ValueError("Sample-based mode requires --end-samples or --length-samples")
        if args.end_samples is not None and args.length_samples is not None:
            raise ValueError("Use only one of --end-samples or --length-samples")
    else:
        if args.end_seconds is None:
            raise ValueError("Time-based mode requires --end-seconds")


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    validate_args(args)
    try:
        return args.func(args)
    except ValueError as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
