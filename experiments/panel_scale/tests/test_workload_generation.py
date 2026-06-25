from experiments.panel_scale.workload.generate_collectives import generate_collective


def test_tiny_workload_generation_uses_byte_directory(tmp_path):
    workload_dir = generate_collective(
        collective="all_gather",
        message_bytes=1024,
        npus=4,
        output_root=tmp_path,
    )

    assert workload_dir == tmp_path / "all_gather" / "4npus_1024B"
    files = sorted(workload_dir.glob("all_gather.*.et"))
    assert [file.name for file in files] == [
        "all_gather.0.et",
        "all_gather.1.et",
        "all_gather.2.et",
        "all_gather.3.et",
    ]
    assert all(file.stat().st_size > 0 for file in files)
