"""The simulated VR timing runner preserves failures and rejects stale logs."""
import importlib.util
from pathlib import Path
import subprocess
import json
import sys
from types import SimpleNamespace

import pytest
from test_load_benchmark import complete_trace, trace

SPEC = importlib.util.spec_from_file_location("vr_load_sweep", Path(__file__).parent / "vr_test/load_sweep.py")
sweep = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sweep)


@pytest.mark.parametrize("failure", [None, "exception", "pipeline", "exit", "no_log", "incomplete", "flat"])
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
            if failure == "flat":
                text += "halo-linux: vr: OpenXR is not available; playing flat\n"
            else:
                text += trace("xr.wait.end", 4.1, "predicted=42 render=1")
                text += trace("xr.submit.end", 4.2, "predicted=42 layers=2")
            (work / "game.log").write_text(text)
        return subprocess.CompletedProcess(command, 139 if failure == "exit" else 0)
    monkeypatch.setenv("HALO_NET_ONLINE", "true")
    monkeypatch.setattr(sweep.subprocess, "run", execute)
    result = sweep.run(args, r"levels\a10\a10", work, tmp_path / "evidence")
    assert result["passed"] == (failure is None)
    assert (tmp_path / "evidence/result.json").exists()


@pytest.mark.parametrize("render,layers,wait,when,valid", [
    ("1", "2", True, 4.2, True), ("1", "5", True, 4.2, True),
    ("0", "2", True, 4.2, False), ("1", "1", True, 4.2, False),
    ("1", "2", False, 4.2, False), ("1", "2", True, 3.2, False),
    ("1", "nan", True, 4.2, False)])
def test_runtime_requires_rendered_game_picture(render, layers, wait, when, valid):
    text = complete_trace()
    if wait:
        text += trace("xr.wait.end", when-.1, f"predicted=42 render={render}")
    text += trace("xr.submit.end", when, f"predicted=42 layers={layers}")
    events, _ = sweep.parse_trace(text)
    assert sweep.runtime_evidence(events, r"levels\a10\a10", text)["valid"] == valid


def test_runtime_errors_remain_failures_even_with_a_later_frame():
    text = (complete_trace() + trace("xr.wait.end", 4.1, "predicted=42 render=1")
            + trace("xr.submit.end", 4.2, "predicted=42 layers=2")
            + "XR_ERROR_RUNTIME_FAILURE in xrCreateInstance\n")
    events, _ = sweep.parse_trace(text)
    assert not sweep.runtime_evidence(events, r"levels\a10\a10", text)["valid"]


def test_later_map_cannot_supply_requested_map_runtime_evidence():
    text = (complete_trace() + trace("main_new_map.begin", 6, r"levels\b30\b30")
            + trace("xr.wait.end", 6.1, "predicted=42 render=1")
            + trace("xr.submit.end", 6.2, "predicted=42 layers=2"))
    events, _ = sweep.parse_trace(text)
    assert not sweep.runtime_evidence(events, r"levels\a10\a10", text)["valid"]


def test_failed_prime_keeps_followup_runs_but_fails_their_protocol(tmp_path, monkeypatch):
    game = tmp_path / "game"
    game.mkdir()
    (game / "halo").write_bytes(b"fake executable")
    out = tmp_path / "evidence"
    calls = []
    def run(args, scenario, work, destination):
        calls.append(destination.name)
        destination.mkdir(parents=True)
        (work / "save-live").mkdir(exist_ok=True)
        (work / "save-live/fake.map").write_bytes(b"fake cache")
        (work / "shaders/fake.bin").write_bytes(b"fake shader")
        result = dict(passed=destination.name != "prime", errors=[])
        (destination / "result.json").write_text(json.dumps(result))
        return result
    monkeypatch.setattr(sweep, "run", run)
    monkeypatch.setattr(sweep, "prepare_files", lambda paths, state: [])
    monkeypatch.setattr(sys, "argv", ["load_sweep.py", "--game", str(game), "--maps", str(tmp_path / "maps"),
                                    "--out", str(out), "--map", "a10"])
    assert sweep.main() == 1
    assert calls == ["prime", "cold", "warm"]
    manifest = json.loads((out / "manifest.json").read_text())
    assert not any(record["passed"] for record in manifest["runs"])
    for state in ("cold", "warm"):
        result = json.loads((out / "a10" / state / "result.json").read_text())
        assert result["run_passed"] and not result["passed"] and not result["priming_valid"]
        assert "initial VR cache priming failed" in result["errors"]


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
