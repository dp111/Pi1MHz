#!/usr/bin/env python3
"""
make_pvfv2.py - build a .pvf (Pi Video File) for the Pi1MHz hardware player

Supersedes make_pvf.py. Same container, same firmware; what changes is the
picture geometry and that the file now records its own pixel aspect ratio.

WHY 832x576 AND NOT 768x576
---------------------------
The Pi1MHz composites two rasters that have to line up: the LaserDisc
picture and the BBC's own display, exactly as the VP415 and the Beeb did
through a genlock. So the sensible sampling grid for the video is the
Beeb's, and the Beeb's is set by its 16 MHz pixel clock:

    PAL active line                     52 us
    BBC MODE 0, 640 pixels at 16 MHz    40 us
    52 us at 16 MHz                     832 pixels     <- this file's width
    PAL frame, active                   576 lines

At 832 wide, one video pixel IS one MODE 0 pixel and one Beeb display line
is two video frame lines, so the two planes register at integer ratios with
no resampling of the Beeb's raster. At 768 the video is 832/768 = 8.3% short
of the Beeb's grid, which is what made the picture look horizontally
squashed against the computer's display.

832 also throws away less of the source. An ld-decode/ld-chroma-decoder PAL
frame is 4fsc: 4 x 4.43361875 MHz = 17.734475 MHz, so its 52 us active line
is about 922 samples. 922 -> 832 keeps 90% of that; 922 -> 768 keeps 83%.

Both constraints of the hardware decoder still hold: width a multiple of 32
(832 = 26 x 32) and height a multiple of 16 (576 = 36 x 16).

PIXEL ASPECT RATIO
------------------
832x576 is not square-pixel. A 4:3 picture over 832x576 has

    PAR = (4/3) / (832/576) = 12/13 = 0.923

which this tool computes and stores in the header, and also declares in the
H264 VUI (via setsar) so host players show it correctly. 768x576 files come
out with PAR 1/1, so nothing changes for them.

The header stays PVF version 1 on purpose: the firmware rejects any other
version, and it reads a fixed 64-byte header whose last three words were
always zero. The PAR goes in two of those, so new files still play on old
firmware (as square pixels) and old files still read as PAR unspecified.

    word 13  par_num       0 = unspecified, treat as square
    word 14  par_den

FILE LAYOUT (unchanged)
-----------------------
  - video: H264 Annex-B, ALL-INTRA (every frame an IDR with its own SPS/PPS)
    so any frame is randomly accessible with one decode, which is what gives
    the player LaserDisc goto/still/step/reverse for free.
  - audio: s16le stereo at --audio-rate, sliced per video frame.
  - a u32 file offset per frame, loaded into RAM by the player.

USAGE
-----
  ./make_pvfv2.py south.mkv video.pvf --crf 17
  ./make_pvfv2.py south.mkv video.pvf --deinterlace     # interlaced source
  ./make_pvfv2.py south.mkv video.pvf --width 768       # old geometry

Then copy video.pvf into the /BeebVFSn directory of the SD card, install the
full start.elf/fixup.dat and set gpu_mem=64 (see firmware/config.txt).
Check the result with verify_pvf.py before copying gigabytes.

Requires: python3, ffmpeg (with libx264) on PATH.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
from fractions import Fraction

PVF_MAGIC = 0x31465650          # 'PVF1'
PVF_VERSION = 1                 # deliberately still 1 - see the notes above
AUDIO_RATE = 48000              # default; see --audio-rate
AUDIO_CHANNELS = 2
AUDIO_BYTES_PER_SAMPLE = 2

BEEB_PIXEL_CLOCK_HZ = 16_000_000
PAL_ACTIVE_LINE_US = 52
BEEB_GRID_WIDTH = 832           # 52 us at 16 MHz
DEFAULT_HEIGHT = 576


def run(cmd):
    print(">>", " ".join(cmd))
    subprocess.run(cmd, check=True)


def pixel_aspect(width, height, display_aspect):
    """PAR that makes width x height display at display_aspect.

    PAR = display aspect / storage aspect. Returned in lowest terms, which
    is what the header and setsar both want."""
    return Fraction(display_aspect.numerator * height,
                    display_aspect.denominator * width)


def build_filter(args, par):
    """The ffmpeg filter chain: optional deinterlace, scale, declared SAR.

    --vf replaces only the scaling part, so the deinterlacer and the SAR
    still apply. (In make_pvf.py --vf replaced the whole chain, which made
    it easy to drop the scale by accident.)"""
    chain = []
    if args.deinterlace:
        chain.append("yadif=1")
    if args.vf:
        chain.append(args.vf)
    else:
        chain.append(f"scale={args.width}:{args.height}:flags={args.scaler}")
    # Declare the pixel aspect on the encoded stream. The Pi player takes
    # its geometry from the header, but this keeps host playback honest.
    chain.append(f"setsar={par.numerator}/{par.denominator}")
    return ",".join(chain)


def encode_video(args, par, h264_path):
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
    cmd += [
        "-an", "-sn",
        "-vf", build_filter(args, par),
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


def parse_aspect(text):
    """'4:3' or '4/3' or '1.3333' -> Fraction."""
    t = text.replace(":", "/")
    try:
        return Fraction(t).limit_denominator(10000)
    except (ValueError, ZeroDivisionError):
        sys.exit(f"error: cannot read display aspect '{text}' (want e.g. 4:3)")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--width", type=int, default=BEEB_GRID_WIDTH,
                   help="encoded width (default 832 = 52 us at the Beeb's "
                        "16 MHz pixel clock)")
    p.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    p.add_argument("--display-aspect", default="4:3",
                   help="how the picture should finally be shown "
                        "(default 4:3); sets the stored pixel aspect ratio")
    p.add_argument("--fps-num", type=int, default=25)
    p.add_argument("--fps-den", type=int, default=1)
    p.add_argument("--crf", type=int, default=18,
                   help="x264 quality, lower = better/bigger (default 18)")
    p.add_argument("--preset", default="slow")
    p.add_argument("--scaler", default="lanczos",
                   help="ffmpeg swscale flags for the resize (default "
                        "lanczos, a better fit for 922 -> 832 than bicubic)")
    p.add_argument("--deinterlace", action="store_true",
                   help="prepend yadif=1 for an interlaced source")
    p.add_argument("--vf", default=None,
                   help="replace the scale filter (deinterlace and the "
                        "declared aspect are still applied around it)")
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

    par = pixel_aspect(args.width, args.height, parse_aspect(args.display_aspect))
    if par.numerator > 0xFFFFFFFF or par.denominator > 0xFFFFFFFF:
        sys.exit("error: pixel aspect ratio does not fit the header")

    print(f"geometry: {args.width}x{args.height}, "
          f"pixel aspect {par.numerator}/{par.denominator} "
          f"({float(par):.4f}), display {args.display_aspect}")
    if args.width != BEEB_GRID_WIDTH:
        print(f"note: {args.width} is off the Beeb's 16 MHz pixel grid "
              f"({BEEB_GRID_WIDTH} = {PAL_ACTIVE_LINE_US} us at "
              f"{BEEB_PIXEL_CLOCK_HZ // 1_000_000} MHz); the overlay will not "
              "register pixel-for-pixel with the computer's display")

    with tempfile.TemporaryDirectory() as tmp:
        h264_path = os.path.join(tmp, "video.h264")
        pcm_path = os.path.join(tmp, "audio.pcm")

        encode_video(args, par, h264_path)
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
            par.numerator, par.denominator,     # words 13, 14 - new in v2
            0,
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
              f"avg {total // max(frame_count, 1)} bytes/frame"
              f"{f', audio {args.audio_rate} Hz' if has_audio else ''}")
        if max_video > 512 * 1024:
            print("WARNING: an access unit exceeds the player's 512 KB "
                  "staging buffer (H264DEC_INPUT_BUF_SIZE) - raise --crf")


if __name__ == "__main__":
    main()
