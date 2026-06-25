#!/usr/bin/env python3
"""Run the 16x16 torus baseline sweep."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


RUN_ONE = Path(__file__).resolve().with_name("run_one_ns3.py")
DEFAULT_COLLECTIVES = ("all_reduce", "all_gather", "reduce_scatter", "all_to_all")
DEFAULT_SIZES = (4 * 1024, 64 * 1024, 1024 * 1024, 16 * 1024 * 1024)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--collective", action="append", choices=DEFAULT_COLLECTIVES)
    parser.add_argument("--bytes", action="append", type=int, dest="message_bytes")
    parser.add_argument("--system", default="ring_2d_4chunks")
    parser.add_argument("--topology", default="torus_16x16")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--keep-going", action="store_true")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    collectives = args.collective or list(DEFAULT_COLLECTIVES)
    sizes = args.message_bytes or list(DEFAULT_SIZES)
    failures: list[tuple[str, int, int]] = []

    for collective in collectives:
        for message_bytes in sizes:
            command = [
                sys.executable,
                str(RUN_ONE),
                "--collective",
                collective,
                "--bytes",
                str(message_bytes),
                "--topology",
                args.topology,
                "--system",
                args.system,
            ]
            if args.prepare_only:
                command.append("--prepare-only")
            completed = subprocess.run(command, check=False)
            if completed.returncode != 0:
                failures.append((collective, message_bytes, completed.returncode))
                if not args.keep_going:
                    raise SystemExit(completed.returncode)

    if failures:
        for collective, message_bytes, returncode in failures:
            print(f"{collective} {message_bytes}B failed with return code {returncode}")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
