from experiments.panel_scale.topology.generate_torus import write_torus_topology
from experiments.panel_scale.topology.validate_topology import (
    assert_valid_torus,
    parse_topology,
)


def test_generate_16x16_torus_counts_and_degrees(tmp_path):
    topology_path = tmp_path / "torus_16x16.txt"
    write_torus_topology(topology_path)

    parsed = parse_topology(topology_path)
    result = assert_valid_torus(topology_path)

    assert result["node_count"] == 512
    assert result["endpoint_count"] == 256
    assert result["switch_count"] == 256
    assert result["link_count"] == 768
    assert result["router_diameter"] == 16
    assert result["connected"] is True

    graph = {node_id: set() for node_id in range(parsed.node_count)}
    for link in parsed.links:
        graph[link.src].add(link.dst)
        graph[link.dst].add(link.src)

    for endpoint in range(256):
        assert len(graph[endpoint]) == 1
    for router in range(256, 512):
        assert len(graph[router]) == 5
        assert sum(1 for neighbor in graph[router] if neighbor >= 256) == 4
