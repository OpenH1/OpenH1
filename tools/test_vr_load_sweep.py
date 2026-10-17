"""The simulated VR timing runner preserves failures and rejects stale logs."""
import importlib.util
from pathlib import Path
import subprocess
import json
import sys
from types import SimpleNamespace

import pytest
from test_load_benchmark import complete_trace

SPEC = importlib.util.spec_from_file_location("vr_load_sweep", Path(__file__).parent / "vr_test/load_sweep.py")
sweep = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sweep)


@pytest.mark.parametrize("failure", [None, "exception", "pipeline", "exit", "no_log", "incomplete"])
def test_run_evidence(tmp_path, monkeypatch, failure):
    work = tmp_path / "work"
    work.mkdir()
    (work / "game.log").write_text(complete_trace())
    args = SimpleNamespace(game=tmp_path / "game", maps=tmp_path / "maps", seconds=1)
    def execute(command, **kwargs):
        assert not (work / "game.log").exists()
        assert "HALO_NET_ONLINE=false" in command
        assert "HALO_SCREENSHOT_EVERY=0" in command
        assert "HALO_NET_ONLINE" not in kwargs["env"]
        if failure != "no_log":
            text = "" if failure == "incomplete" else complete_trace()
            text += {"exception": "EXCEPTION: bad\n", "pipeline": "pipeline missed map preparation\n"}.get(failure, "")
            (work / "game.log").write_text(text)
        return subprocess.CompletedProcess(command, 139 if failure == "exit" else 0)
    monkeypatch.setenv("HALO_NET_ONLINE", "true")
    monkeypatch.setattr(sweep.subprocess, "run", execute)
    result = sweep.run(args, r"levels\a10\a10", work, tmp_path / "evidence")
    assert result["passed"] == (failure is None)
    assert (tmp_path / "evidence/result.json").exists()


def test_stop_finishes_current_map_and_preserves_pending_maps(tmp_path, monkeypatch):
    game = tmp_path / "game"
    game.mkdir()
    (game / "halo").write_bytes(b"fake executable")
    stop = tmp_path / "stop"
    out = tmp_path / "evidence"
    calls = []
    def run(args, scenario, work, destination):
        calls.append((scenario, destination.name))
        (work / "save-live").mkdir(exist_ok=True)
        (work / "save-live/fake.map").write_bytes(b"fake cache")
        (work / "shaders/fake.bin").write_bytes(b"fake shader")
        stop.touch()
        return dict(passed=True)
    monkeypatch.setattr(sweep, "run", run)
    monkeypatch.setattr(sweep, "prepare_files", lambda paths, state: [])
    monkeypatch.setattr(sys, "argv", ["load_sweep.py", "--game", str(game), "--maps", str(tmp_path / "maps"),
                                    "--out", str(out), "--map", "a10", "--map", "a30", "--stop-file", str(stop)])
    assert sweep.main() == 2
    assert calls == [(r"levels\a10\a10", state) for state in ("prime", "cold", "warm")]
    manifest = json.loads((out / "manifest.json").read_text())
    assert not manifest["passed"]
    assert manifest["pending_maps"] == [r"levels\a30\a30"]
    assert all(record["passed"] for record in manifest["runs"])
    assert not list(out.glob(".work-*"))
