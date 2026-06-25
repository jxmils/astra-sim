# Panel-Scale Torus Baseline

This directory contains a self-contained 256-NPU ASTRA-sim + ns-3 baseline for a 16x16 physical torus.

## Model

- NPUs: `0..255`
- Torus routers: `256..511`
- NPU to local router: `3200Gbps`
- Router to router links: four `800Gbps` torus directions
- Logical topology: `["16", "16"]`
- System config: 2D ring collectives with four preferred dataset splits

The ns-3 backend only forwards through nodes marked as switches, so only routers `256..511` are switches. NPUs are endpoints and are not connected directly to other NPUs.

## Generate and Validate Topology

```bash
python experiments/panel_scale/topology/generate_torus.py \
  --output experiments/panel_scale/generated/topology/torus_16x16.txt

python experiments/panel_scale/topology/validate_topology.py \
  experiments/panel_scale/generated/topology/torus_16x16.txt
```

The 16x16 topology should have 512 total nodes, 256 switch/router nodes, and 768 undirected links.

## Generate Workloads

```bash
python experiments/panel_scale/workload/generate_collectives.py
```

This writes byte-sized Chakra ET files under `experiments/panel_scale/generated/workloads/` for:

- `all_reduce`
- `all_gather`
- `reduce_scatter`
- `all_to_all`

at `4096`, `65536`, `1048576`, and `16777216` bytes.

## Smoke Run

Build the ns-3 backend first, then run the initial clean baseline:

```bash
python experiments/panel_scale/run/run_one_ns3.py \
  --collective all_gather \
  --bytes 1048576 \
  --topology torus_16x16 \
  --system ring_2d_4chunks
```

The runner creates:

```text
experiments/panel_scale/results/torus_16x16/all_gather/1048576B/ring_2d_4chunks/
```

with `topology.txt`, `network.conf`, `flow.txt`, `trace.txt`, `stdout.log`, `fct.txt`, `pfc.txt`, `qlen.txt`, and `summary.json`. It always writes `flow.txt` and `trace.txt` as `0`.

To prepare artifacts without launching ns-3:

```bash
python experiments/panel_scale/run/run_one_ns3.py \
  --collective all_gather \
  --bytes 1048576 \
  --topology torus_16x16 \
  --system ring_2d_4chunks \
  --prepare-only
```

## Full Baseline Sweep

After the smoke run passes:

```bash
python experiments/panel_scale/run/run_torus_16x16_sweep.py
```

This runs all four collectives at 4 KiB, 64 KiB, 1 MiB, and 16 MiB.

## Tests

These are lightweight unit tests only. They do not run ns-3.

```bash
python -m pytest experiments/panel_scale/tests
```

Generated workloads, topology files, ns-3 configs, logs, and result files live under ignored directories.
