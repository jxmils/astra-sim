#!/usr/bin/env python3
"""Validate the panel-scale ns-3 torus topology."""

from __future__ import annotations

import argparse
import json
from collections import deque
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ParsedLink:
    src: int
    dst: int
    rate: str
    delay: str
    error_rate: str

    @property
    def pair(self) -> tuple[int, int]:
        return tuple(sorted((self.src, self.dst)))


@dataclass(frozen=True)
class ParsedTopology:
    node_count: int
    switch_ids: frozenset[int]
    links: tuple[ParsedLink, ...]

    @property
    def endpoint_ids(self) -> frozenset[int]:
        return frozenset(set(range(self.node_count)) - set(self.switch_ids))


def parse_topology(path: Path) -> ParsedTopology:
    tokens = path.read_text().split()
    if len(tokens) < 3:
        raise ValueError(f"{path} does not contain a topology header")

    node_count = int(tokens[0])
    switch_count = int(tokens[1])
    link_count = int(tokens[2])
    cursor = 3

    switch_tokens = tokens[cursor : cursor + switch_count]
    if len(switch_tokens) != switch_count:
        raise ValueError(f"{path} ended while reading switch IDs")
    switch_ids = frozenset(int(token) for token in switch_tokens)
    cursor += switch_count

    links: list[ParsedLink] = []
    for _ in range(link_count):
        if cursor + 5 > len(tokens):
            raise ValueError(f"{path} ended while reading links")
        src = int(tokens[cursor])
        dst = int(tokens[cursor + 1])
        rate = tokens[cursor + 2]
        delay = tokens[cursor + 3]
        error_rate = tokens[cursor + 4]
        links.append(ParsedLink(src, dst, rate, delay, error_rate))
        cursor += 5

    if cursor != len(tokens):
        raise ValueError(f"{path} contains extra tokens after {link_count} links")

    return ParsedTopology(node_count, switch_ids, tuple(links))


def _adjacency(topology: ParsedTopology) -> dict[int, set[int]]:
    graph = {node_id: set() for node_id in range(topology.node_count)}
    for link in topology.links:
        if link.src in graph and link.dst in graph:
            graph[link.src].add(link.dst)
            graph[link.dst].add(link.src)
    return graph


def _bfs_reachable(graph: dict[int, set[int]], start: int, allowed: set[int] | None = None) -> set[int]:
    if allowed is not None and start not in allowed:
        return set()
    seen = {start}
    queue = deque([start])
    while queue:
        node = queue.popleft()
        for neighbor in graph[node]:
            if allowed is not None and neighbor not in allowed:
                continue
            if neighbor not in seen:
                seen.add(neighbor)
                queue.append(neighbor)
    return seen


def _router_diameter(graph: dict[int, set[int]], router_ids: set[int]) -> int:
    diameter = 0
    for router in router_ids:
        distances = {router: 0}
        queue = deque([router])
        while queue:
            node = queue.popleft()
            for neighbor in graph[node]:
                if neighbor not in router_ids or neighbor in distances:
                    continue
                distances[neighbor] = distances[node] + 1
                queue.append(neighbor)
        if set(distances) != router_ids:
            return -1
        diameter = max(diameter, max(distances.values()))
    return diameter


def validate_topology(
    topology: ParsedTopology,
    expected_rows: int = 16,
    expected_cols: int = 16,
) -> dict[str, object]:
    expected_endpoints = expected_rows * expected_cols
    expected_switches = expected_endpoints
    expected_nodes = expected_endpoints + expected_switches
    expected_links = expected_endpoints + expected_rows * expected_cols * 2
    expected_diameter = expected_rows // 2 + expected_cols // 2

    errors: list[str] = []
    endpoint_ids = set(topology.endpoint_ids)
    switch_ids = set(topology.switch_ids)
    graph = _adjacency(topology)

    if topology.node_count != expected_nodes:
        errors.append(f"expected {expected_nodes} total nodes, found {topology.node_count}")
    if len(endpoint_ids) != expected_endpoints:
        errors.append(f"expected {expected_endpoints} endpoints, found {len(endpoint_ids)}")
    if len(switch_ids) != expected_switches:
        errors.append(f"expected {expected_switches} switch/router nodes, found {len(switch_ids)}")
    if len(topology.links) != expected_links:
        errors.append(f"expected {expected_links} links, found {len(topology.links)}")

    seen_pairs: set[tuple[int, int]] = set()
    for link in topology.links:
        if link.src == link.dst:
            errors.append(f"self-link found on node {link.src}")
        if link.src < 0 or link.src >= topology.node_count:
            errors.append(f"link source {link.src} is outside node range")
        if link.dst < 0 or link.dst >= topology.node_count:
            errors.append(f"link destination {link.dst} is outside node range")
        if link.pair in seen_pairs:
            errors.append(f"duplicate undirected link found between {link.pair[0]} and {link.pair[1]}")
        seen_pairs.add(link.pair)
        if link.src in endpoint_ids and link.dst in endpoint_ids:
            errors.append(f"endpoint-to-endpoint link found between {link.src} and {link.dst}")

    endpoint_degree_errors = [
        endpoint for endpoint in sorted(endpoint_ids) if len(graph[endpoint]) != 1
    ]
    router_degree_errors = [
        router for router in sorted(switch_ids) if len(graph[router]) != 5
    ]
    router_external_errors = [
        router
        for router in sorted(switch_ids)
        if sum(1 for neighbor in graph[router] if neighbor in switch_ids) != 4
    ]

    if endpoint_degree_errors:
        errors.append(f"endpoints with degree != 1: {endpoint_degree_errors[:8]}")
    if router_degree_errors:
        errors.append(f"routers with degree != 5: {router_degree_errors[:8]}")
    if router_external_errors:
        errors.append(f"routers with external torus degree != 4: {router_external_errors[:8]}")

    connected = False
    endpoint_paths_exist = False
    if topology.node_count > 0:
        connected = len(_bfs_reachable(graph, 0)) == topology.node_count
        if not connected:
            errors.append("graph is not connected")

        endpoint_paths_exist = True
        for endpoint in endpoint_ids:
            reachable = _bfs_reachable(graph, endpoint)
            if not endpoint_ids.issubset(reachable):
                endpoint_paths_exist = False
                errors.append(f"not all endpoints are reachable from endpoint {endpoint}")
                break

    diameter = _router_diameter(graph, switch_ids) if switch_ids else -1
    if diameter != expected_diameter:
        errors.append(f"expected router diameter {expected_diameter}, found {diameter}")

    return {
        "valid": not errors,
        "errors": errors,
        "endpoint_count": len(endpoint_ids),
        "switch_count": len(switch_ids),
        "node_count": topology.node_count,
        "link_count": len(topology.links),
        "connected": connected,
        "endpoint_paths_exist": endpoint_paths_exist,
        "router_diameter": diameter,
    }


def validate_topology_file(
    path: Path,
    expected_rows: int = 16,
    expected_cols: int = 16,
) -> dict[str, object]:
    return validate_topology(parse_topology(path), expected_rows, expected_cols)


def assert_valid_torus(path: Path, expected_rows: int = 16, expected_cols: int = 16) -> dict[str, object]:
    result = validate_topology_file(path, expected_rows, expected_cols)
    if not result["valid"]:
        raise ValueError("; ".join(result["errors"]))
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("topology", type=Path)
    parser.add_argument("--rows", type=int, default=16)
    parser.add_argument("--cols", type=int, default=16)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    result = validate_topology_file(args.topology, args.rows, args.cols)
    print(json.dumps(result, indent=2, sort_keys=True))
    if not result["valid"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
