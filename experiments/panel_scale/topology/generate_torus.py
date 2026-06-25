#!/usr/bin/env python3
"""Generate ns-3 topology files for a 2D NPU-router torus."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


DEFAULT_ROWS = 16
DEFAULT_COLS = 16
DEFAULT_ENDPOINT_RATE = "3200Gbps"
DEFAULT_TORUS_RATE = "800Gbps"
DEFAULT_DELAY = "0.005ms"
DEFAULT_ERROR_RATE = "0"


@dataclass(frozen=True)
class Link:
    src: int
    dst: int
    rate: str
    delay: str = DEFAULT_DELAY
    error_rate: str = DEFAULT_ERROR_RATE

    @property
    def pair(self) -> tuple[int, int]:
        return tuple(sorted((self.src, self.dst)))

    def line(self) -> str:
        return f"{self.src} {self.dst} {self.rate} {self.delay} {self.error_rate}"


@dataclass(frozen=True)
class Topology:
    node_count: int
    switch_ids: tuple[int, ...]
    links: tuple[Link, ...]

    @property
    def switch_count(self) -> int:
        return len(self.switch_ids)

    @property
    def link_count(self) -> int:
        return len(self.links)


def rank(row: int, col: int, cols: int) -> int:
    return row * cols + col


def router_id(npu_rank: int, npu_count: int) -> int:
    return npu_count + npu_rank


def _add_unique_link(links: list[Link], seen: set[tuple[int, int]], link: Link) -> None:
    if link.src == link.dst:
        raise ValueError(f"self-link requested for node {link.src}")
    if link.pair in seen:
        return
    seen.add(link.pair)
    links.append(link)


def generate_torus(
    rows: int = DEFAULT_ROWS,
    cols: int = DEFAULT_COLS,
    endpoint_rate: str = DEFAULT_ENDPOINT_RATE,
    torus_rate: str = DEFAULT_TORUS_RATE,
    delay: str = DEFAULT_DELAY,
    error_rate: str = DEFAULT_ERROR_RATE,
) -> Topology:
    """Return an NPU-router 2D torus topology.

    NPUs are numbered 0..N-1. Routers are numbered N..2N-1 and are the
    only nodes marked as switches in the ns-3 topology file.
    """

    if rows <= 0 or cols <= 0:
        raise ValueError("rows and cols must both be positive")

    npu_count = rows * cols
    node_count = npu_count * 2
    switch_ids = tuple(range(npu_count, node_count))
    links: list[Link] = []
    seen: set[tuple[int, int]] = set()

    for npu in range(npu_count):
        _add_unique_link(
            links,
            seen,
            Link(npu, router_id(npu, npu_count), endpoint_rate, delay, error_rate),
        )

    for row in range(rows):
        for col in range(cols):
            this_rank = rank(row, col, cols)
            east_rank = rank(row, (col + 1) % cols, cols)
            _add_unique_link(
                links,
                seen,
                Link(
                    router_id(this_rank, npu_count),
                    router_id(east_rank, npu_count),
                    torus_rate,
                    delay,
                    error_rate,
                ),
            )

    for row in range(rows):
        for col in range(cols):
            this_rank = rank(row, col, cols)
            south_rank = rank((row + 1) % rows, col, cols)
            _add_unique_link(
                links,
                seen,
                Link(
                    router_id(this_rank, npu_count),
                    router_id(south_rank, npu_count),
                    torus_rate,
                    delay,
                    error_rate,
                ),
            )

    return Topology(node_count=node_count, switch_ids=switch_ids, links=tuple(links))


def topology_lines(topology: Topology) -> Iterable[str]:
    yield f"{topology.node_count} {topology.switch_count} {topology.link_count}"
    yield " ".join(str(switch_id) for switch_id in topology.switch_ids)
    for link in topology.links:
        yield link.line()


def write_topology(topology: Topology, output_path: Path) -> Path:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(topology_lines(topology)) + "\n")
    return output_path


def write_torus_topology(
    output_path: Path,
    rows: int = DEFAULT_ROWS,
    cols: int = DEFAULT_COLS,
    endpoint_rate: str = DEFAULT_ENDPOINT_RATE,
    torus_rate: str = DEFAULT_TORUS_RATE,
    delay: str = DEFAULT_DELAY,
    error_rate: str = DEFAULT_ERROR_RATE,
) -> Path:
    topology = generate_torus(
        rows=rows,
        cols=cols,
        endpoint_rate=endpoint_rate,
        torus_rate=torus_rate,
        delay=delay,
        error_rate=error_rate,
    )
    return write_topology(topology, output_path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", type=int, default=DEFAULT_ROWS)
    parser.add_argument("--cols", type=int, default=DEFAULT_COLS)
    parser.add_argument("--endpoint-rate", default=DEFAULT_ENDPOINT_RATE)
    parser.add_argument("--torus-rate", default=DEFAULT_TORUS_RATE)
    parser.add_argument("--delay", default=DEFAULT_DELAY)
    parser.add_argument("--error-rate", default=DEFAULT_ERROR_RATE)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    write_torus_topology(
        args.output,
        rows=args.rows,
        cols=args.cols,
        endpoint_rate=args.endpoint_rate,
        torus_rate=args.torus_rate,
        delay=args.delay,
        error_rate=args.error_rate,
    )
    print(args.output)


if __name__ == "__main__":
    main()
