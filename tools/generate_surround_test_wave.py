"""Generate a deterministic six-channel WAVEFORMATEXTENSIBLE test signal.

The file uses the Windows 5.1-surround channel mask and activates one channel
at a time in this order: FL, FR, FC, LFE, SL, SR. It is intended for manual
end-to-end routing checks through a player that preserves multichannel PCM.
"""

from __future__ import annotations

import argparse
import math
import struct
from pathlib import Path


SAMPLE_RATE = 48_000
CHANNEL_COUNT = 6
BITS_PER_SAMPLE = 16
BLOCK_ALIGN = CHANNEL_COUNT * BITS_PER_SAMPLE // 8
CHANNEL_MASK_5_1_SURROUND = 0x60F
CHANNELS = (
    ("FL", 440.00),
    ("FR", 554.37),
    ("FC", 659.25),
    ("LFE", 60.00),
    ("SL", 783.99),
    ("SR", 987.77),
)


def _pcm_subformat_guid() -> bytes:
    # KSDATAFORMAT_SUBTYPE_PCM: 00000001-0000-0010-8000-00AA00389B71
    return struct.pack("<IHH8s", 1, 0, 0x0010, bytes.fromhex("800000aa00389b71"))


def build_wave_bytes(tone_seconds: float = 0.8, silence_seconds: float = 0.2) -> bytes:
    if tone_seconds <= 0 or silence_seconds < 0:
        raise ValueError("tone_seconds must be positive and silence_seconds non-negative")

    tone_frames = round(tone_seconds * SAMPLE_RATE)
    silence_frames = round(silence_seconds * SAMPLE_RATE)
    segment_frames = tone_frames + silence_frames
    total_frames = segment_frames * len(CHANNELS)
    pcm = bytearray(total_frames * BLOCK_ALIGN)
    fade_frames = max(1, min(round(0.01 * SAMPLE_RATE), tone_frames // 2))

    for channel_index, (_, frequency_hz) in enumerate(CHANNELS):
        segment_start = channel_index * segment_frames
        amplitude = 0.32 if channel_index == 3 else 0.22
        for local_frame in range(tone_frames):
            envelope = min(
                1.0,
                local_frame / fade_frames,
                (tone_frames - 1 - local_frame) / fade_frames,
            )
            sample = amplitude * envelope * math.sin(
                2.0 * math.pi * frequency_hz * local_frame / SAMPLE_RATE
            )
            pcm_value = max(-32_768, min(32_767, round(sample * 32_767.0)))
            byte_offset = ((segment_start + local_frame) * CHANNEL_COUNT + channel_index) * 2
            struct.pack_into("<h", pcm, byte_offset, pcm_value)

    average_bytes_per_second = SAMPLE_RATE * BLOCK_ALIGN
    fmt = struct.pack(
        "<HHIIHHH",
        0xFFFE,  # WAVE_FORMAT_EXTENSIBLE
        CHANNEL_COUNT,
        SAMPLE_RATE,
        average_bytes_per_second,
        BLOCK_ALIGN,
        BITS_PER_SAMPLE,
        22,  # cbSize
    )
    fmt += struct.pack("<HI", BITS_PER_SAMPLE, CHANNEL_MASK_5_1_SURROUND)
    fmt += _pcm_subformat_guid()
    fmt_chunk = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    data_chunk = b"data" + struct.pack("<I", len(pcm)) + pcm
    riff_payload = b"WAVE" + fmt_chunk + data_chunk
    return b"RIFF" + struct.pack("<I", len(riff_payload)) + riff_payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "output",
        nargs="?",
        type=Path,
        default=Path("build/surround-5.1-channel-check.wav"),
        help="Output WAV path (default: build/surround-5.1-channel-check.wav)",
    )
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_wave_bytes())
    print(f"Wrote {args.output.resolve()}")
    print("Channel sequence: FL, FR, FC, LFE, SL, SR")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
