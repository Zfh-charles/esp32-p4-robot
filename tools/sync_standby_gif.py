#!/usr/bin/env python3
"""Build-time: convert board standby.mjpeg -> standby.gif for Flash assets partition."""

import argparse
import os
import shutil
import subprocess
import sys


def run_ffmpeg(mjpeg: str, gif: str) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found in PATH; install ffmpeg to build ep-chat assets")

    vf = (
        "fps=15,scale='min(480,iw)':'min(480,ih)':flags=lanczos,"
        "split[s0][s1];[s0]palettegen=max_colors=128[p];[s1][p]paletteuse=dither=bayer:bayer_scale=3"
    )
    cmd = [
        ffmpeg, "-y", "-i", mjpeg,
        "-vf", vf,
        "-loop", "0",
        gif,
    ]
    print("Running:", " ".join(cmd))
    subprocess.check_call(cmd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mjpeg", required=True)
    parser.add_argument("--gif", required=True)
    args = parser.parse_args()

    if not os.path.isfile(args.mjpeg):
        print(f"ERROR: missing {args.mjpeg}", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(args.gif)), exist_ok=True)

    if os.path.isfile(args.gif) and os.path.getmtime(args.gif) >= os.path.getmtime(args.mjpeg):
        size_kb = os.path.getsize(args.gif) // 1024
        print(f"standby.gif up to date ({size_kb} KiB)")
        return 0

    run_ffmpeg(args.mjpeg, args.gif)
    size_kb = os.path.getsize(args.gif) // 1024
    print(f"Generated standby.gif ({size_kb} KiB)")
    if size_kb > 2048:
        print(f"WARNING: standby.gif is {size_kb} KiB (> 2 MiB target)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
