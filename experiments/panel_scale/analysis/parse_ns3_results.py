#!/usr/bin/env python3
"""Parse ASTRA-sim + ns-3 panel-scale result directories."""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path
from typing import Iterable


FINISHED_RE = re.compile(
    r"sys\[(?P<rank>\d+)\]\s+finished,\s+"
    r"(?P<cycles>[0-9.eE+-]+)\s+cycles,\s+"
    r"exposed communication\s+(?P<comm>[0-9.eE+-]+)\s+cycles"
)
WALL_RE = re.compile(r"sys\[(?P<rank>\d+)\],\s+Wall time:\s+(?P<value>[0-9.eE+-]+)")
COMM_RE = re.compile(r"sys\[(?P<rank>\d+)\],\s+Comm time:\s+(?P<value>[0-9.eE+-]+)")
FATAL_RE = re.compile(
    r"(NS_ASSERT|NS_FATAL|assert failed|segmentation fault|terminate called|"
    r"Fail to setup ns3 simulation|Error: cannot|fatal)",
    re.IGNORECASE,
)


def _to_number(value: str) -> int | float:
    parsed = float(value)
    if parsed.is_integer():
        return int(parsed)
    return parsed


def parse_stdout_text(text: str) -> dict[str, object]:
    rank_completion_times: dict[int, int | float] = {}
    exposed_comm_times: dict[int, int | float] = {}
    wall_times: dict[int, int | float] = {}
    comm_times: dict[int, int | float] = {}

    for match in FINISHED_RE.finditer(text):
        rank = int(match.group("rank"))
        rank_completion_times[rank] = _to_number(match.group("cycles"))
        exposed_comm_times[rank] = _to_number(match.group("comm"))

    for match in WALL_RE.finditer(text):
        wall_times[int(match.group("rank"))] = _to_number(match.group("value"))

    for match in COMM_RE.finditer(text):
        comm_times[int(match.group("rank"))] = _to_number(match.group("value"))

    fatal_errors = [line.strip() for line in text.splitlines() if FATAL_RE.search(line)]
    return {
        "rank_completion_times": rank_completion_times,
        "exposed_comm_times": exposed_comm_times,
        "wall_times": wall_times,
        "comm_times": comm_times,
        "fatal_errors": fatal_errors,
    }


def _percentile(values: list[int], percentile: float) -> int | None:
    if not values:
        return None
    sorted_values = sorted(values)
    index = math.ceil((percentile / 100.0) * len(sorted_values)) - 1
    index = max(0, min(index, len(sorted_values) - 1))
    return sorted_values[index]


def parse_fct_file(path: Path) -> dict[str, int | None]:
    fcts: list[int] = []
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            parts = line.split()
            if len(parts) < 7:
                continue
            try:
                fcts.append(int(parts[6]))
            except ValueError:
                continue
    return {
        "fct_count": len(fcts),
        "fct_p50": _percentile(fcts, 50),
        "fct_p95": _percentile(fcts, 95),
        "fct_p99": _percentile(fcts, 99),
    }


def count_pfc_events(path: Path) -> int:
    if not path.exists():
        return 0
    return sum(1 for line in path.read_text(errors="replace").splitlines() if line.strip())


def max_queue_occupancy(path: Path) -> int | None:
    if not path.exists():
        return None
    max_occupancy: int | None = None
    for line in path.read_text(errors="replace").splitlines():
        values = [int(match.group(1)) for match in re.finditer(r"\bj\s+\d+\s+(\d+)\b", line)]
        if values:
            line_max = max(values)
            max_occupancy = line_max if max_occupancy is None else max(max_occupancy, line_max)
    return max_occupancy


def _stringify_keys(values: dict[int, int | float]) -> dict[str, int | float]:
    return {str(key): values[key] for key in sorted(values)}


def _finite_values(values: Iterable[int | float]) -> bool:
    return all(math.isfinite(float(value)) for value in values)


def parse_result_dir(
    result_dir: Path,
    expected_ranks: int = 256,
    simulator_wall_clock_seconds: float | None = None,
) -> dict[str, object]:
    stdout_path = result_dir / "stdout.log"
    stdout_text = stdout_path.read_text(errors="replace") if stdout_path.exists() else ""
    stdout = parse_stdout_text(stdout_text)
    fct = parse_fct_file(result_dir / "fct.txt")
    pfc_events = count_pfc_events(result_dir / "pfc.txt")
    queue_occupancy = max_queue_occupancy(result_dir / "qlen.txt")

    summary_path = result_dir / "summary.json"
    if simulator_wall_clock_seconds is None and summary_path.exists():
        try:
            existing = json.loads(summary_path.read_text())
            value = existing.get("simulator_wall_clock_seconds")
            if value is not None:
                simulator_wall_clock_seconds = float(value)
        except (json.JSONDecodeError, TypeError, ValueError):
            pass

    rank_completion_times = stdout["rank_completion_times"]
    wall_times = stdout["wall_times"]
    comm_times = stdout["comm_times"]
    failures: list[str] = []

    if len(rank_completion_times) != expected_ranks:
        failures.append(f"expected {expected_ranks} ranks finished, found {len(rank_completion_times)}")
    if len(wall_times) != expected_ranks:
        failures.append(f"expected wall time for {expected_ranks} ranks, found {len(wall_times)}")
    if not wall_times:
        failures.append("no rank wall times found")
    elif not _finite_values(wall_times.values()):
        failures.append("at least one rank wall time is not finite")
    if stdout["fatal_errors"]:
        failures.append("fatal ns-3 errors found")

    max_wall_time = max(wall_times.values()) if wall_times else None
    max_comm_time = max(comm_times.values()) if comm_times else None

    return {
        "pass": not failures,
        "failures": failures,
        "expected_ranks": expected_ranks,
        "ranks_completed": len(rank_completion_times),
        "rank_completion_times": _stringify_keys(rank_completion_times),
        "wall_times": _stringify_keys(wall_times),
        "comm_times": _stringify_keys(comm_times),
        "max_wall_time": max_wall_time,
        "max_comm_time": max_comm_time,
        "fct_p50": fct["fct_p50"],
        "fct_p95": fct["fct_p95"],
        "fct_p99": fct["fct_p99"],
        "fct_count": fct["fct_count"],
        "total_pfc_events": pfc_events,
        "max_queue_occupancy": queue_occupancy,
        "simulator_wall_clock_seconds": simulator_wall_clock_seconds,
        "fatal_errors": stdout["fatal_errors"],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result_dir", type=Path)
    parser.add_argument("--expected-ranks", type=int, default=256)
    parser.add_argument("--runtime-seconds", type=float, default=None)
    parser.add_argument("--write-summary", action="store_true")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    summary = parse_result_dir(args.result_dir, args.expected_ranks, args.runtime_seconds)
    text = json.dumps(summary, indent=2, sort_keys=True)
    print(text)
    if args.write_summary:
        (args.result_dir / "summary.json").write_text(text + "\n")
    if not summary["pass"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
