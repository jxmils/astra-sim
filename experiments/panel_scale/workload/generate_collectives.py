#!/usr/bin/env python3
"""Generate byte-sized Chakra collective ET files for panel-scale runs."""

from __future__ import annotations

import argparse
from pathlib import Path


COMM_COLL_NODE = 7
COLLECTIVE_TYPES = {
    "all_reduce": 0,
    "all_gather": 2,
    "all_to_all": 6,
    "reduce_scatter": 7,
}
DEFAULT_MESSAGE_BYTES = (4 * 1024, 64 * 1024, 1024 * 1024, 16 * 1024 * 1024)
DEFAULT_NPUS = 256


def default_output_root() -> Path:
    return Path(__file__).resolve().parents[1] / "generated" / "workloads"


def _encode_varint(value: int) -> bytes:
    if value < 0:
        raise ValueError("varint value must be non-negative")
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def _field_key(field_number: int, wire_type: int) -> bytes:
    return _encode_varint((field_number << 3) | wire_type)


def _varint_field(field_number: int, value: int) -> bytes:
    return _field_key(field_number, 0) + _encode_varint(value)


def _length_field(field_number: int, value: bytes) -> bytes:
    return _field_key(field_number, 2) + _encode_varint(len(value)) + value


def _string_field(field_number: int, value: str) -> bytes:
    return _length_field(field_number, value.encode("utf-8"))


def _attribute_bool(name: str, value: bool) -> bytes:
    return _string_field(1, name) + _varint_field(27, int(value))


def _attribute_int64(name: str, value: int) -> bytes:
    return _string_field(1, name) + _varint_field(9, value)


def _metadata_record() -> bytes:
    return _string_field(1, "0.0.4")


def _node_record(node_id: int, name: str, collective_type: int, message_bytes: int) -> bytes:
    parts: list[bytes] = []
    if node_id != 0:
        parts.append(_varint_field(1, node_id))
    parts.append(_string_field(2, name))
    parts.append(_varint_field(3, COMM_COLL_NODE))
    parts.append(_length_field(10, _attribute_bool("is_cpu_op", False)))
    parts.append(_length_field(10, _attribute_int64("comm_type", collective_type)))
    parts.append(_length_field(10, _attribute_int64("comm_size", message_bytes)))
    return b"".join(parts)


def _write_record(handle, payload: bytes) -> None:
    handle.write(_encode_varint(len(payload)))
    handle.write(payload)


def write_collective_et(path: Path, node_id: int, collective: str, npus: int, message_bytes: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    collective_type = COLLECTIVE_TYPES[collective]
    node_name = f"{collective}_{npus}npus_{message_bytes}B"
    with path.open("wb") as handle:
        _write_record(handle, _metadata_record())
        _write_record(handle, _node_record(node_id, node_name, collective_type, message_bytes))


def collective_workload_dir(
    output_root: Path,
    collective: str,
    npus: int,
    message_bytes: int,
) -> Path:
    return output_root / collective / f"{npus}npus_{message_bytes}B"


def collective_workload_prefix(
    output_root: Path,
    collective: str,
    npus: int,
    message_bytes: int,
) -> Path:
    return collective_workload_dir(output_root, collective, npus, message_bytes) / collective


def generate_collective(
    collective: str,
    message_bytes: int,
    npus: int = DEFAULT_NPUS,
    output_root: Path | None = None,
) -> Path:
    if collective not in COLLECTIVE_TYPES:
        raise ValueError(f"unsupported collective: {collective}")
    if message_bytes <= 0:
        raise ValueError("message_bytes must be positive")
    if npus <= 0:
        raise ValueError("npus must be positive")

    root = output_root if output_root is not None else default_output_root()
    workload_dir = collective_workload_dir(root, collective, npus, message_bytes)
    for npu in range(npus):
        write_collective_et(
            workload_dir / f"{collective}.{npu}.et",
            node_id=npu,
            collective=collective,
            npus=npus,
            message_bytes=message_bytes,
        )
    return workload_dir


def generate_matrix(
    collectives: list[str],
    message_sizes: list[int],
    npus: int = DEFAULT_NPUS,
    output_root: Path | None = None,
) -> list[Path]:
    generated: list[Path] = []
    for collective in collectives:
        for message_bytes in message_sizes:
            generated.append(generate_collective(collective, message_bytes, npus, output_root))
    return generated


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--collective",
        choices=sorted(COLLECTIVE_TYPES) + ["all"],
        default="all",
        help="collective to generate; default generates every supported collective",
    )
    parser.add_argument(
        "--bytes",
        type=int,
        action="append",
        dest="message_bytes",
        help="message size in bytes; repeat to generate multiple sizes",
    )
    parser.add_argument("--npus", type=int, default=DEFAULT_NPUS)
    parser.add_argument("--output-root", type=Path, default=default_output_root())
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    collectives = sorted(COLLECTIVE_TYPES) if args.collective == "all" else [args.collective]
    message_sizes = args.message_bytes or list(DEFAULT_MESSAGE_BYTES)
    generated = generate_matrix(collectives, message_sizes, args.npus, args.output_root)
    for path in generated:
        print(path)


if __name__ == "__main__":
    main()
