from experiments.panel_scale.analysis.parse_ns3_results import parse_stdout_text


def test_parse_stdout_extracts_rank_completion_times():
    stdout = """
[2026-06-25 12:00:00] [workload] [info] sys[0] finished, 100 cycles, exposed communication 75 cycles.
[2026-06-25 12:00:00] [workload] [info] sys[0], Wall time: 100
[2026-06-25 12:00:00] [workload] [info] sys[0], Comm time: 75
[2026-06-25 12:00:00] [workload] [info] sys[1] finished, 125 cycles, exposed communication 80 cycles.
[2026-06-25 12:00:00] [workload] [info] sys[1], Wall time: 125
[2026-06-25 12:00:00] [workload] [info] sys[1], Comm time: 80
"""

    parsed = parse_stdout_text(stdout)

    assert parsed["rank_completion_times"] == {0: 100, 1: 125}
    assert parsed["exposed_comm_times"] == {0: 75, 1: 80}
    assert parsed["wall_times"] == {0: 100, 1: 125}
    assert parsed["comm_times"] == {0: 75, 1: 80}
    assert parsed["fatal_errors"] == []
