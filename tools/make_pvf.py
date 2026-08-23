#!/usr/bin/env python3
"""
make_pvf.py - build a .pvf (Pi Video File) for the Pi1MHz hardware video player

Takes any video ffmpeg can read and produces a single seekable file:

  - video: H264, Annex-B, ALL-INTRA (every frame an IDR with its own
    SPS/PPS in front) so any frame is randomly accessible with a single
    decode - which is what gives the player LaserDisc-style goto/still/
    step/reverse for free. High profile, CABAC, decoded in hardware by
    the Pi Zero's VideoCore (fine up to 1080p; the target is 768x576).
  - audio: s16le stereo resampled to --audio-rate (default 48000 Hz, a
    standard HDMI rate; the PWM path runs at any rate), sliced per video
    frame (25 fps -> exactly 1920 samples per frame, no resampling on
    the Pi).
  - a frame index (u32 file offset per frame) the player loads into RAM.

Typical use for a Domesday/LaserDisc capture (see Doomsday.md for the
ld-chroma-decoder step that produces the source video):

  ./make_pvf.py south.mkv video.pvf --crf 17
  ./make_pvf.py south.mkv video.pvf --vf "yadif=1,scale=768:576" # deinterlace

Then copy video.pvf into the /Pi1MHz directory of the SD card, install
the full start.elf/fixup.dat and set gpu_mem=64 (see firmware/config.txt).

Requires: python3, ffmpeg (with libx264) on PATH.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

PVF_MAGIC = 0x31465650          # 'PVF1'
PVF_VERSION = 1
AUDIO_RATE = 48000              # default; see --audio-rate
AUDIO_CHANNELS = 2
AUDIO_BYTES_PER_SAMPLE = 2


def run(cmd):
    print(">>", " ".join(cmd))
    subprocess.run(cmd, check=True)


def encode_video(args, h264_path):
    x264 = (
        "keyint=1:min-keyint=1:scenecut=0:repeat-headers=1:"
        "bframes=0:ref=1:rc-lookahead=0:threads=auto"
    )
    # -ss BEFORE -i: input seeking, so ffmpeg jumps rather than decoding
    # (and, over a network drive, reading) everything before the start
    cmd = ["ffmpeg", "-y"]
    if args.start:
        cmd += ["-ss", str(args.start)]
    cmd += ["-i", args.input]
    if args.frames:
        cmd += ["-frames:v", str(args.frames)]
    vf = args.vf if args.vf else f"scale={args.width}:{args.height}"
    cmd += [
        "-an", "-sn",
        "-vf", vf,
        "-pix_fmt", "yuv420p",
        "-r", f"{args.fps_num}/{args.fps_den}",
        "-c:v", "libx264",
        "-preset", args.preset,
        "-crf", str(args.crf),
        "-profile:v", "high",
        "-x264-params", x264,
        "-f", "h264", h264_path,
    ]
    run(cmd)


def encode_audio(args, pcm_path):
    cmd = ["ffmpeg", "-y"]
    if args.start:
        cmd += ["-ss", str(args.start)]
    if args.audio_input:
        # A separate audio file, e.g. the raw .pcm that ld-decode writes
        # alongside the .tbc: far cheaper than re-reading a huge FFV1
        # capture just for its sound track. Raw PCM needs its format
        # given up front.
        if args.audio_input.lower().endswith(".pcm"):
            cmd += ["-f", "s16le", "-ar", str(args.audio_input_rate), "-ac", "2"]
        cmd += ["-i", args.audio_input]
    else:
        cmd += ["-i", args.input]
    if args.frames:
        # match the video's duration exactly
        cmd += ["-t", f"{args.frames * args.fps_den / args.fps_num:.6f}"]
    cmd += [
        "-vn", "-sn",
        "-ar", str(args.audio_rate),
        "-ac", str(AUDIO_CHANNELS),
        "-f", "s16le", pcm_path,
    ]
    try:
        run(cmd)
        return True
    except subprocess.CalledProcessError:
        print("note: no audio track found, continuing without audio")
        return False


def split_nal_types(au):
    """NAL types present in one access unit."""
    out, i = [], 0
    while i + 3 < len(au):
        if au[i:i + 3] == b"\0\0\1":
            out.append(au[i + 3] & 0x1F)
            i += 3
        elif i + 4 < len(au) and au[i:i + 4] == b"\0\0\0\1":
            out.append(au[i + 4] & 0x1F)
            i += 4
        else:
            i += 1
    return out


def split_access_units(data):
    """Split an Annex-B byte stream into access units.

    With repeat-headers + keyint=1 every frame is SPS PPS [SEI] IDR, so a
    new AU starts at each SPS (NAL type 7). Also honour access-unit
    delimiters (type 9) if present."""
    # Find all start codes (both 3- and 4-byte forms)
    positions = []          # (offset_of_start_code, nal_type)
    i = 0
    n = len(data)
    while i < n - 3:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                positions.append((i, data[i + 3] & 0x1F))
                i += 3
                continue
            if data[i + 2] == 0 and i < n - 4 and data[i + 3] == 1:
                positions.append((i, data[i + 4] & 0x1F))
                i += 4
                continue
        i += 1

    if not positions:
        sys.exit("error: no NAL units found - is this an Annex-B H264 stream?")

    au_starts = [off for off, nal in positions if nal in (7, 9)]
    # De-duplicate AUD-followed-by-SPS
    starts = []
    last = -1
    for off in au_starts:
        # Two boundary markers with no VCL NAL between them = same AU
        if starts and not any(
            nal in (1, 5) for o, nal in positions if starts[-1] <= o < off
        ):
            continue
        if off != last:
            starts.append(off)
            last = off
    if starts[0] != 0:
        starts.insert(0, 0)

    aus = []
    for idx, off in enumerate(starts):
        end = starts[idx + 1] if idx + 1 < len(starts) else n
        aus.append(data[off:end])
    return aus


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--width", type=int, default=768)
    p.add_argument("--height", type=int, default=576)
    p.add_argument("--fps-num", type=int, default=25)
    p.add_argument("--fps-den", type=int, default=1)
    p.add_argument("--crf", type=int, default=18,
                   help="x264 quality, lower = better/bigger (default 18)")
    p.add_argument("--preset", default="slow")
    p.add_argument("--vf", default=None,
                   help="override the ffmpeg video filter chain "
                        "(default scale=WxH); e.g. 'yadif=1,scale=768:576'")
    p.add_argument("--start", default=None, help="ffmpeg -ss start point")
    p.add_argument("--frames", type=int, default=None, help="limit frame count")
    p.add_argument("--no-audio", action="store_true")
    p.add_argument("--audio-input", default=None,
                   help="take the sound track from this file instead of the "
                        "video input (a .pcm is read as raw s16le stereo)")
    p.add_argument("--audio-input-rate", type=int, default=44100,
                   help="sample rate of a raw .pcm --audio-input")
    p.add_argument("--audio-rate", type=int, default=AUDIO_RATE,
                   help="PCM sample rate in Hz (48000 for HDMI; 46875 was the "
                        "original PWM-only rate)")
    args = p.parse_args()

    if args.width % 32 or args.height % 16:
        sys.exit("error: width must be a multiple of 32 and height of 16 "
                 "(I420 hardware constraint)")

    with tempfile.TemporaryDirectory() as tmp:
        h264_path = os.path.join(tmp, "video.h264")
        pcm_path = os.path.join(tmp, "audio.pcm")

        encode_video(args, h264_path)
        has_audio = (not args.no_audio) and encode_audio(args, pcm_path)

        with open(h264_path, "rb") as f:
            aus = split_access_units(f.read())
        pcm = b""
        if has_audio:
            with open(pcm_path, "rb") as f:
                pcm = f.read()

        frame_count = len(aus)
        print(f"{frame_count} access units")

        # Every AU must be standalone-decodable: at least one IDR slice
        # (type 5) and no non-IDR slices (types 1-4). A leading SEI-only
        # "AU" (possible if the stream did not start with an SPS) or any
        # inter-coded slice would stall the player's frame accounting.
        for i, au in enumerate(aus):
            types = split_nal_types(au)
            if 5 not in types or any(t in (1, 2, 3, 4) for t in types):
                sys.exit(f"error: access unit {i} is not a standalone IDR "
                         f"picture (NAL types {types}) - encoder settings "
                         "changed?")

        # Per-frame audio slice sizes (fractional accumulation keeps sync
        # for frame rates that do not divide the sample rate)
        bytes_per_sample_pair = AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE
        def audio_slice(i):
            s0 = i * args.audio_rate * args.fps_den // args.fps_num
            s1 = (i + 1) * args.audio_rate * args.fps_den // args.fps_num
            return s0 * bytes_per_sample_pair, s1 * bytes_per_sample_pair

        nominal = (args.audio_rate * args.fps_den // args.fps_num) * bytes_per_sample_pair

        header_size = 64
        index_offset = header_size
        data_offset = index_offset + 4 * frame_count

        index = []
        records = []
        offset = data_offset
        max_video = 0
        for i, au in enumerate(aus):
            vlen = len(au)
            max_video = max(max_video, vlen)
            vpad = (-vlen) % 4
            if has_audio:
                a0, a1 = audio_slice(i)
                chunk = pcm[a0:a1]
                if len(chunk) < a1 - a0:      # pad tail with silence
                    chunk += b"\0" * (a1 - a0 - len(chunk))
            else:
                chunk = b""
            rec = struct.pack("<II", vlen, len(chunk)) + au + b"\0" * vpad + chunk
            index.append(offset)
            records.append(rec)
            offset += len(rec)
            if offset >= 0xFFFFFFFF:
                sys.exit("error: output exceeds 4GB (FAT32/PVF limit) - "
                         "raise --crf or split the disc")

        header = struct.pack(
            "<16I",
            PVF_MAGIC, PVF_VERSION,
            args.width, args.height,
            args.fps_num, args.fps_den,
            frame_count,
            args.audio_rate if has_audio else 0,
            AUDIO_CHANNELS if has_audio else 0,
            nominal if has_audio else 0,
            index_offset, data_offset,
            max_video,
            0, 0, 0,
        )

        with open(args.output, "wb") as out:
            out.write(header)
            out.write(struct.pack(f"<{frame_count}I", *index))
            for rec in records:
                out.write(rec)

        total = offset
        print(f"wrote {args.output}: {frame_count} frames, "
              f"{total / 1e6:.1f} MB total, "
              f"largest AU {max_video} bytes, "
              f"avg {total // max(frame_count,1)} bytes/frame"
              f"{f', audio {args.audio_rate} Hz' if has_audio else ''}")
        if max_video > 512 * 1024:
            print("WARNING: an access unit exceeds the player's 512 KB "
                  "staging buffer (H264DEC_INPUT_BUF_SIZE) - raise --crf")


if __name__ == "__main__":
    main()
