"""The VR runner must report game failures even when it successfully saves logs.
Run its real shell script with temporary mount paths and a fake game, no GPU."""
import os
import re
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.skipif(os.name != "posix", reason="the VR runner uses a POSIX shell")
@pytest.mark.parametrize("game_status,exception,expected", [(0, False, 0), (7, False, 7),
                                                          (0, True, 1), (7, True, 7)])
def test_runner_reports_game_failures(tmp_path, game_status, exception, expected):
    paths = {name: tmp_path / name for name in ("game", "out", "maps", "runtime", "bin")}
    for path in paths.values():
        path.mkdir()
    game = paths["game"] / "halo"
    game.write_text("#!/bin/sh\n[ \"$HALO_NET_ONLINE\" = false ] || exit 99\n"
                    "printf '%s\\n' \"$FAKE_DEBUG\" > debug.txt\n"
                    "touch config.toml\nexit \"$FAKE_STATUS\"\n")
    game.chmod(0o755)
    (paths["game"] / "libSDL3.so.0").touch()
    for name in ("sleep", "monado-service"):
        stub = paths["bin"] / name
        stub.write_text("#!/bin/sh\nexit 0\n")
        stub.chmod(0o755)
    script = (ROOT / "tools/vr_test/container_run.sh").read_text()
    mounts = {"/usr/local/bin/monado-service": paths["bin"] / "monado-service",
              "/tmp": paths["runtime"], "/game": paths["game"],
              "/maps": paths["maps"], "/out": paths["out"]}
    script = re.sub(r"/usr/local/bin/monado-service|/(?:tmp|game|maps|out)(?=[/\s$;])",
                    lambda match: str(mounts[match[0]]), script)
    runner = tmp_path / "run.sh"
    runner.write_text(script)
    environment = dict(os.environ, PATH=f"{paths['bin']}:{os.environ['PATH']}",
                       FAKE_DEBUG="EXCEPTION: test failure" if exception else "game stopped normally",
                       FAKE_STATUS=str(game_status))
    result = subprocess.run(["bash", str(runner), "1", "b30", "1", "HALO_NET_ONLINE=true"], env=environment,
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == expected, result.stdout + result.stderr
    assert ("VR run failed: EXCEPTION" in result.stderr) == exception
    assert (paths["out"] / "debug.txt").read_text().strip() == environment["FAKE_DEBUG"]


@pytest.mark.parametrize("failure", [None, "missing_event", "exception", "validation", "pipeline", "exit", "timeout", "missing_eye"])
def test_scenario_checks_events_and_preserves_evidence(tmp_path, monkeypatch, failure):
    """A normal process exit alone must not certify a VR transition."""
    import importlib.util
    import json
    from types import SimpleNamespace
    Image = pytest.importorskip("PIL.Image")
    spec = importlib.util.spec_from_file_location("vr_scenarios", ROOT / "tools/vr_test/scenarios.py")
    scenarios = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(scenarios)
    game = tmp_path / "game"
    game.mkdir()
    (game / "halo").write_bytes(b"fake game")
    out = tmp_path / "evidence"
    out.mkdir()
    arguments = SimpleNamespace(game=game, maps=tmp_path / "maps", out=out,
                                env=["HALO_NET_ONLINE=true"])
    directories = []

    def run(command, **kwargs):
        assert "HALO_NET_ONLINE=false" in command
        assert "HALO_NET_ONLINE=true" not in command
        directory = Path(command[1])
        directories.append(directory)
        (directory / "shots").mkdir()
        image_name = "hud144.bmp" if failure == "missing_eye" else "eyes144.bmp"
        Image.new("RGB", (2, 2), "red").save(directory / "shots" / image_name)
        messages = [] if failure == "missing_event" else ["the transition happened"]
        messages += {"exception": ["EXCEPTION: a crash"],
                     "validation": ["vulkan validation: VUID-failure"],
                     "pipeline": ["pipeline missed map preparation" ]}.get(failure, [])
        (directory / "game.log").write_text("\n".join(messages))
        (directory / "debug.txt").write_text("debug evidence")
        if failure == "timeout":
            raise subprocess.TimeoutExpired(command, 1)
        return subprocess.CompletedProcess(command, 7 if failure == "exit" else 0)

    monkeypatch.setattr(scenarios.subprocess, "run", run)
    result = scenarios.run_case("transition", ("a10", 1, {}, ["the transition happened"]), arguments)
    assert result["passed"] == (failure is None)
    assert (out / "transition/game.log").exists()
    assert list((out / "transition/images").glob("*.png"))
    assert json.loads((out / "transition/result.json").read_text()) == result
    assert not directories[0].exists()
    assert not list(out.rglob("*.bmp"))
