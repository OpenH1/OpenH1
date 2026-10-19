"""tools/gpu_busy.py: the kernel's fdinfo counts of a process's GPU time."""
from pathlib import Path

import gpu_busy

MSM = """pos:\t0
flags:\t02100002
mnt_id:\t26
ino:\t1234
drm-driver:\tmsm
drm-client-id:\t{client}
drm-engine-gpu:\t{busy} ns
drm-cycles-gpu:\t99999
drm-maxfreq-gpu:\t903000000 Hz
drm-total-memory:\t1024 KiB
"""


def test_a_client_is_counted_once_however_many_descriptors_it_has(tmp_path):
    fdinfo = tmp_path / "77" / "fdinfo"
    fdinfo.mkdir(parents=True)
    (fdinfo / "5").write_text(MSM.format(client=3, busy=2_000_000))
    (fdinfo / "6").write_text(MSM.format(client=3, busy=2_000_000))
    (fdinfo / "9").write_text(MSM.format(client=4, busy=500))
    (fdinfo / "0").write_text("pos:\t0\nflags:\t02\n")
    assert gpu_busy.process_busy(77, tmp_path) == 2_000_500


def test_engines_are_summed_and_capacities_are_not(tmp_path):
    text = ("drm-client-id: 1\ndrm-engine-render: 300 ns\ndrm-engine-copy: 20 ns\n"
            "drm-engine-capacity-render: 2\n")
    assert gpu_busy.client_times(text) == ("1", 320)
    assert gpu_busy.client_times("drm-client-id: 1\n") is None
    assert gpu_busy.client_times("drm-engine-gpu: 5 ns\n") is None


def test_report_gives_the_share_and_the_frames_milliseconds():
    # 0.5 s of a second's work at 120 Hz: 50%, 4.17 ms of each 8.33 ms frame
    line = gpu_busy.report(500_000_000, 1_000_000_000, 120.0)
    assert "50.0%" in line and "4.17 ms a frame" in line and "8.33 ms at 120 Hz" in line
    assert gpu_busy.report(250, 1000, 0.0).strip() == "GPU busy  25.0%"


def test_the_game_is_found_by_its_name(tmp_path):
    (tmp_path / "12").mkdir()
    (tmp_path / "12" / "comm").write_text("bash\n")
    (tmp_path / "34").mkdir()
    (tmp_path / "34" / "comm").write_text("halo\n")
    (tmp_path / "self").mkdir()
    assert gpu_busy.find_process("halo", tmp_path) == 34
    assert gpu_busy.find_process("steam", tmp_path) is None
