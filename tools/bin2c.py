#!/usr/bin/env python3
"""Turn a binary file into a C array.

The device build embeds blobs with ESP-IDF's EMBED_FILES. The simulator has no
equivalent, so its CMake runs this at build time and compiles the result out of
the build directory. Nothing this emits is committed.
"""

import argparse
import pathlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--name", required=True)
    args = parser.parse_args()

    data = args.source.read_bytes()
    rows = [
        "    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ","
        for i in range(0, len(data), 16)
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        f"/* Generated from {args.source.name} by tools/bin2c.py. */\n"
        f"const unsigned char {args.name}[] = {{\n"
        + "\n".join(rows)
        + f"\n}};\nconst unsigned int {args.name}_len = {len(data)};\n")


if __name__ == "__main__":
    main()
