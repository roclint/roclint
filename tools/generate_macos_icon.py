#!/usr/bin/env python3
"""Generate a multi-resolution macOS .icns file from a PNG source."""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def generate_icns(source_png: Path, output_icns: Path) -> None:
    if not source_png.is_file():
        raise FileNotFoundError(f"Source PNG not found: {source_png}")

    output_icns.parent.mkdir(parents=True, exist_ok=True)
    iconset_dir = output_icns.parent / f"{output_icns.stem}.iconset"
    if iconset_dir.exists():
        shutil.rmtree(iconset_dir)
    iconset_dir.mkdir(parents=True, exist_ok=True)

    sizes = [
        (16, "icon_16x16.png"),
        (32, "icon_16x16@2x.png"),
        (32, "icon_32x32.png"),
        (64, "icon_32x32@2x.png"),
        (128, "icon_128x128.png"),
        (256, "icon_128x128@2x.png"),
        (256, "icon_256x256.png"),
        (512, "icon_256x256@2x.png"),
        (512, "icon_512x512.png"),
        (1024, "icon_512x512@2x.png"),
    ]

    try:
        for px, filename in sizes:
            out_file = iconset_dir / filename
            subprocess.check_call(
                ["sips", "-z", str(px), str(px), str(source_png), "--out", str(out_file)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )

        subprocess.check_call(
            ["iconutil", "-c", "icns", str(iconset_dir), "-o", str(output_icns)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        print(f"Generated {output_icns} ({output_icns.stat().st_size} bytes)")
    finally:
        if iconset_dir.exists():
            shutil.rmtree(iconset_dir)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate macOS .icns from PNG")
    parser.add_argument(
        "--source",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "app/qml/shell/assets/roc-mark.png",
        help="Path to source PNG image",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "app/host/Roclint.icns",
        help="Path to destination .icns file",
    )
    args = parser.parse_args()
    generate_icns(args.source, args.output)


if __name__ == "__main__":
    main()
