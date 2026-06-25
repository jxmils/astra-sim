#!/usr/bin/env python3
"""Prepare and run one ASTRA-sim + ns-3 panel-scale torus experiment."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


PANEL_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from experiments.panel_scale.analysis.parse_ns3_results import parse_result_dir
from experiments.panel_scale.topology.generate_torus import write_torus_topology
from experiments.panel_scale.topology.validate_topology import assert_valid_torus
from experiments.panel_scale.workload.generate_collectives import (
    COLLECTIVE_TYPES,
    DEFAULT_NPUS,
    collective_workload_prefix,
    default_output_root,
    generate_collective,
)


TOPOLOGIES = {"torus_16x16": (16, 16)}
SYSTEMS = {"ring_2d_4chunks": PANEL_DIR / "configs" / "system" / "ring_2d_4chunks.json"}
NS3_BINARY = REPO_ROOT / "extern" / "network_backend" / "ns-3" / "build" / "scratch" / "ns3.42-AstraSimNetwork-default"
REMOTE_MEMORY_CONFIG = REPO_ROOT / "examples" / "remote_memory" / "analytical" / "no_memory_expansion.json"
NS3_TEMPLATE = PANEL_DIR / "configs" / "ns3" / "torus_16x16_4x800g.conf.template"


def render_template(template: Path, output: Path, replacements: dict[str, Path]) -> Path:
    text = template.read_text()
    for key, value in replacements.items():
        text = text.replace("{" + key + "}", str(value.resolve()))
    output.write_text(text)
    return output


def result_dir(topology: str, collective: str, message_bytes: int, system: str) -> Path:
    return PANEL_DIR / "results" / topology / collective / f"{message_bytes}B" / system


def prepare_run(args: argparse.Namespace) -> dict[str, Path]:
    rows, cols = TOPOLOGIES[args.topology]
    out_dir = result_dir(args.topology, args.collective, args.message_bytes, args.system)
    out_dir.mkdir(parents=True, exist_ok=True)

    topology_path = out_dir / "topology.txt"
    write_torus_topology(topology_path, rows=rows, cols=cols)
    assert_valid_torus(topology_path, rows, cols)

    workload_root = default_output_root()
    generate_collective(args.collective, args.message_bytes, DEFAULT_NPUS, workload_root)
    workload_prefix = collective_workload_prefix(
        workload_root, args.collective, DEFAULT_NPUS, args.message_bytes
    )

    flow_path = out_dir / "flow.txt"
    trace_path = out_dir / "trace.txt"
    flow_path.write_text("0\n")
    trace_path.write_text("0\n")

    network_path = out_dir / "network.conf"
    render_template(
        NS3_TEMPLATE,
        network_path,
        {
            "TOPOLOGY_FILE": topology_path,
            "FLOW_FILE": flow_path,
            "TRACE_FILE": trace_path,
            "TRACE_OUTPUT_FILE": out_dir / "trace_output.tr",
            "FCT_OUTPUT_FILE": out_dir / "fct.txt",
            "PFC_OUTPUT_FILE": out_dir / "pfc.txt",
            "QLEN_MON_FILE": out_dir / "qlen.txt",
        },
    )

    logical_topology = PANEL_DIR / "configs" / "logical" / f"{args.topology}.json"
    return {
        "result_dir": out_dir,
        "topology": topology_path,
        "workload_prefix": workload_prefix,
        "system": SYSTEMS[args.system],
        "network": network_path,
        "remote_memory": REMOTE_MEMORY_CONFIG,
        "logical_topology": logical_topology,
    }


def build_command(paths: dict[str, Path]) -> list[str]:
    return [
        str(NS3_BINARY),
        f"--workload-configuration={paths['workload_prefix'].resolve()}",
        f"--system-configuration={paths['system'].resolve()}",
        f"--network-configuration={paths['network'].resolve()}",
        f"--remote-memory-configuration={paths['remote_memory'].resolve()}",
        f"--logical-topology-configuration={paths['logical_topology'].resolve()}",
        "--comm-group-configuration=empty",
    ]


def write_summary(result_path: Path, summary: dict[str, object]) -> None:
    result_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")


def run_experiment(args: argparse.Namespace) -> dict[str, object]:
    paths = prepare_run(args)
    out_dir = paths["result_dir"]
    command = build_command(paths)
    stdout_path = out_dir / "stdout.log"

    base_summary: dict[str, object] = {
        "collective": args.collective,
        "bytes": args.message_bytes,
        "topology": args.topology,
        "system": args.system,
        "command": command,
        "paths": {key: str(value.resolve()) for key, value in paths.items()},
    }

    if args.prepare_only:
        summary = {
            **base_summary,
            "returncode": None,
            "status": "prepared",
            "simulator_wall_clock_seconds": None,
        }
        write_summary(out_dir / "summary.json", summary)
        return summary

    if not NS3_BINARY.exists():
        summary = {
            **base_summary,
            "returncode": 127,
            "status": "missing_ns3_binary",
            "simulator_wall_clock_seconds": None,
            "pass": False,
            "failures": [f"missing ns-3 binary: {NS3_BINARY}"],
        }
        stdout_path.write_text(f"missing ns-3 binary: {NS3_BINARY}\n")
        write_summary(out_dir / "summary.json", summary)
        raise FileNotFoundError(NS3_BINARY)

    start = time.monotonic()
    with stdout_path.open("w") as stdout:
        completed = subprocess.run(
            command,
            cwd=NS3_BINARY.parent,
            stdout=stdout,
            stderr=subprocess.STDOUT,
            check=False,
        )
    elapsed = time.monotonic() - start

    parsed = parse_result_dir(out_dir, expected_ranks=DEFAULT_NPUS, simulator_wall_clock_seconds=elapsed)
    summary = {
        **base_summary,
        **parsed,
        "returncode": completed.returncode,
        "status": "completed" if completed.returncode == 0 else "failed",
    }
    if completed.returncode != 0:
        summary["pass"] = False
        summary["failures"] = list(summary.get("failures", [])) + [
            f"ns-3 exited with return code {completed.returncode}"
        ]
    write_summary(out_dir / "summary.json", summary)
    return summary


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--collective", choices=sorted(COLLECTIVE_TYPES), required=True)
    parser.add_argument("--bytes", type=int, required=True, dest="message_bytes")
    parser.add_argument("--topology", choices=sorted(TOPOLOGIES), required=True)
    parser.add_argument("--system", choices=sorted(SYSTEMS), required=True)
    parser.add_argument(
        "--prepare-only",
        action="store_true",
        help="write topology, workload, ns-3 config, flow.txt, trace.txt, and summary.json without running ns-3",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    summary = run_experiment(args)
    result_path = Path(summary["paths"]["result_dir"])
    print(result_path)
    if summary.get("pass") is False:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
