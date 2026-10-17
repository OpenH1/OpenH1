"""The load runner's evidence, isolation and failures, without game assets.

The subprocess is faked; no game, display, or network connection is started.
"""
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

import load_benchmark as benchmark


def trace(phase, wall, detail="a10"):
    return f"LOAD phase={phase} wall={wall:.6f} thread={wall / 2:.6f} process={wall * 2:.6f} owner=guest:7 detail={detail}\n"


def complete_trace(scenario=r"levels\a10\a10"):
    return "".join(trace(phase, index + 1, scenario) for index, phase in enumerate((
        "main_new_map.begin", "initialize.end", "main_new_map.initial_pulse.end",
        "first_game_frame.begin", "first_game_frame.submitted")))


def test_nested_and_repeated_phases_preserve_each_span():
    text = (trace("map.begin", 1) + trace("tags.begin", 2) + trace("tags.end", 3)
            + trace("map.end", 4) + trace("map.begin", 5, "b30") + trace("map.end", 7, "b30"))
    events, io = benchmark.parse_trace(text)
    result = benchmark.summarize(events, io, "a10")
    assert [(s["phase"], s["detail"], s["wall_ms"]) for s in result["spans"]] == [
        ("tags", "a10", 1000), ("map", "a10", 3000), ("map", "b30", 2000)]
    assert result["events"] == events


def test_recursive_same_phase_uses_stack():
    events, io = benchmark.parse_trace(trace("map.begin", 1, "outer") + trace("map.begin", 2, "inner")
                                       + trace("map.end", 3, "inner") + trace("map.end", 5, "outer"))
    spans = benchmark.summarize(events, io, "a10")["spans"]
    assert [(s["detail"], s["wall_ms"]) for s in spans] == [("inner", 1000), ("outer", 4000)]


def test_enumeration_includes_both_draw_generation_and_preparation_wait():
    events, io = benchmark.parse_trace(trace("enumerate.begin", 1)
        + trace("enumerate.draws.end", 3) + trace("enumerate.prepared.end", 4))
    spans = benchmark.summarize(events, io, "a10")["spans"]
    durations = {s["phase"]: s["wall_ms"] for s in spans}
    assert durations == {"enumerate.draws": 2000, "enumerate.total": 3000, "enumerate.wait": 1000}


def test_io_paths_with_spaces_and_failed_short_reads():
    events, io = benchmark.parse_trace(
        "ordinary log\nLOAD_IO op=read wall=2.0 elapsed=0.004 syscall=0.003 requested=4096 actual=512 "
        "offset=2048 bounce=1 path=/private/game data/a10.map\n"
        "LOAD_IO op=write wall=3.0 elapsed=0.002 syscall=0.001 requested=512 actual=0 offset=0 bounce=0 path=x\n")
    assert io[0]["path"] == "/private/game data/a10.map"
    totals = benchmark.summarize(events, io, "a10")["io"]
    assert totals["read"] == dict(calls=1, actual_bytes=512, elapsed_ms=4, syscall_ms=3)
    assert totals["write"]["actual_bytes"] == 0


def test_present_gaps_are_separate_per_backend():
    events, io = benchmark.parse_trace(trace("gl.swap.return", 1) + trace("metal.presented", 1.005)
                                       + trace("gl.swap.return", 1.025))
    presentations = benchmark.summarize(events, io, "a10")["presentations"]
    assert presentations["gl.swap.return"]["maximum_gap_ms"] == pytest.approx(25)
    assert presentations["metal.presented"]["maximum_gap_ms"] is None


@pytest.mark.parametrize("line", ["LOAD_IO op=read actual=bad", "LOAD_IO op=read elapsed=bad"])
def test_malformed_io_numbers_fail(line):
    with pytest.raises(ValueError):
        benchmark.parse_trace(line)


@pytest.mark.parametrize("line", ["LOAD phase=map.begin wall=bad thread=1 process=1 detail=a10",
                                  "LOAD phase=map.begin wall=1 thread=1"])
def test_malformed_trace_does_not_silently_disappear(line):
    with pytest.raises(ValueError):
        benchmark.parse_trace(line)


def test_interleaved_trace_does_not_hide_another_record_in_detail():
    text = trace("tags.begin", 1).rstrip() + " " + trace("tags.end", 2)
    with pytest.raises(ValueError, match="interleaved"):
        benchmark.parse_trace(text)


@pytest.fixture
def runner(tmp_path, monkeypatch):
    work, saves, shaders, data = [tmp_path / name for name in ("work", "saves", "shaders", "data")]
    for path in (work, saves, shaders, data / "maps"):
        path.mkdir(parents=True)
    args = SimpleNamespace(data=data, halo=tmp_path / "fake-halo", seconds=1, env=[])
    args.halo.write_bytes(b"fake executable")
    (args.halo.parent / "libSDL3.so.0").write_bytes(b"fake SDL")
    calls = []

    def launch(command, **kwargs):
        calls.append(dict(kwargs, command=command))
        assert Path(command[0]).parent == Path(kwargs["cwd"])
        assert not Path(command[0]).is_symlink()
        assert Path(command[0]).read_bytes() == b"fake executable"
        assert (Path(kwargs["cwd"]) / "libSDL3.so.0").read_bytes() == b"fake SDL"
        (Path(kwargs["cwd"]) / "config.toml").write_text("private config")
        kwargs["stdout"].write(complete_trace())
        (Path(kwargs["cwd"]) / "debug.txt").write_text("diagnostic\n")
        (Path(kwargs["cwd"]) / "temporary.bmp").write_bytes(b"private image")
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(benchmark.subprocess, "run", launch)
    return args, work, saves, shaders, tmp_path / "output", calls


def execute(runner, **kwargs):
    args, work, saves, shaders, output, _ = runner
    return benchmark.run_game(args, r"levels\a10\a10", work, saves, shaders, output, **kwargs)


def test_offline_settings_override_inherited_and_requested_online(runner, monkeypatch):
    monkeypatch.setenv("HALO_NET_ONLINE", "true")
    monkeypatch.setenv("HALO_NETWORK_TEST", "join")
    monkeypatch.setenv("HALO_SCREENSHOT_EVERY", "1")
    runner[0].env = [("HALO_RANDOM_SEED", "42")]
    execute(runner)
    env = runner[-1][0]["env"]
    assert env["HALO_NET_ONLINE"] == "false"
    assert env["HALO_SCREENSHOT_EVERY"] == "0"
    assert "HALO_NETWORK_TEST" not in env
    assert env["HALO_SAVE_ROOT"] == str(runner[2])
    assert env["MESA_SHADER_CACHE_DIR"] == str(runner[3] / "mesa")
    assert not list(runner[1].iterdir())
    assert not list(runner[4].rglob("*.bmp"))
    assert (runner[4] / "debug.txt").read_text() == "diagnostic\n"


def test_inherited_desktop_driver_cannot_open_user_display(runner, monkeypatch):
    monkeypatch.setenv("SDL_VIDEO_DRIVER", "x11")
    monkeypatch.setenv("DISPLAY", ":0")
    execute(runner)
    assert runner[-1][0]["env"]["SDL_VIDEO_DRIVER"] == "offscreen"


def test_explicit_network_join_is_rejected(runner):
    runner[0].env = [("HALO_NETWORK_TEST", "join")]
    with pytest.raises((ValueError, RuntimeError)):
        execute(runner)


def test_clean_exit_with_only_load_begin_is_not_success(runner, monkeypatch):
    def launch(command, **kwargs):
        kwargs["stdout"].write(trace("main_new_map.begin", 1, r"levels\a10\a10"))
        return SimpleNamespace(returncode=0)
    monkeypatch.setattr(benchmark.subprocess, "run", launch)
    with pytest.raises(RuntimeError):
        execute(runner)
    assert not json.loads((runner[4] / "result.json").read_text())["passed"]


def test_exception_in_debug_fails_and_cleans_private_run(runner, monkeypatch):
    def launch(command, **kwargs):
        kwargs["stdout"].write(trace("map.begin", 1))
        (Path(kwargs["cwd"]) / "debug.txt").write_text("EXCEPTION access violation\n")
        return SimpleNamespace(returncode=0)
    monkeypatch.setattr(benchmark.subprocess, "run", launch)
    with pytest.raises(RuntimeError):
        execute(runner)
    assert not list(runner[1].iterdir())
    result = json.loads((runner[4] / "result.json").read_text())
    assert not result["passed"] and result["errors"] == ["EXCEPTION access violation"]


def test_timeout_fails_and_cleans_private_run(runner, monkeypatch):
    def launch(command, **kwargs):
        raise benchmark.subprocess.TimeoutExpired(command, kwargs["timeout"])
    monkeypatch.setattr(benchmark.subprocess, "run", launch)
    with pytest.raises(RuntimeError):
        execute(runner)
    assert not list(runner[1].iterdir())
    assert json.loads((runner[4] / "result.json").read_text())["exit_status"] == 124


@pytest.mark.skipif(sys.platform != "linux", reason="Linux mincore/fadvise proof")
def test_real_file_warm_and_cold_residency(tmp_path):
    path = tmp_path / "owned.map"
    path.write_bytes(os.urandom(8 * 1024 * 1024))
    warm = benchmark.prepare_files([path], "warm")
    assert warm[0]["after"]["resident_pages"] == warm[0]["after"]["pages"] > 0
    # If this filesystem refuses eviction, production must reject the run.
    try:
        cold = benchmark.prepare_files([path], "cold")
    except RuntimeError as error:
        assert "eviction not verified" in str(error)
    else:
        assert cold[0]["after"]["resident_pages"] == 0


def test_missing_source_cannot_prove_cold_cache(tmp_path):
    with pytest.raises((FileNotFoundError, ValueError, RuntimeError)):
        benchmark.prepare_files([tmp_path / "missing.map"], "cold")


def test_nonresident_after_warm_prefetch_rejected(tmp_path, monkeypatch):
    path = tmp_path / "owned.map"
    path.write_bytes(b"data")
    monkeypatch.setattr(benchmark, "residency", lambda p: dict(bytes=4, pages=1, resident_pages=0))
    with pytest.raises(RuntimeError):
        benchmark.prepare_files([path], "warm")


def test_owner_fields_can_precede_clocks_and_detail_is_not_fields():
    events, _ = benchmark.parse_trace(
        "prefix LOAD owner=host:abcd process=-1 phase=prepare.begin thread=-1 wall=1 detail=count=2 map=a10\n"
        "prefix LOAD owner=host:abcd process=-1 phase=prepare.end thread=-1 wall=2 detail=wait_ms=4\n")
    span = benchmark.summarize(events, [], "a10")["spans"][0]
    assert events[0]["detail"] == "count=2 map=a10"
    assert span["wall_ms"] == 1000
    assert span["thread_ms"] is None and span["process_ms"] is None


def test_interleaved_owners_do_not_pair_each_other():
    text = (trace("compile.begin", 1).replace("guest:7", "host:a")
            + trace("compile.begin", 2).replace("guest:7", "host:b")
            + trace("compile.end", 4).replace("guest:7", "host:a")
            + trace("compile.end", 8).replace("guest:7", "host:b"))
    events, _ = benchmark.parse_trace(text)
    spans = benchmark.summarize(events, [], "a10")["spans"]
    assert [(s["owner"], s["wall_ms"]) for s in spans] == [("host:a", 3000), ("host:b", 6000)]


def test_legacy_ownerless_trace_does_not_claim_thread_cpu():
    events, _ = benchmark.parse_trace((trace("map.begin", 1) + trace("map.end", 2)).replace(" owner=guest:7", ""))
    assert benchmark.summarize(events, [], "a10")["spans"][0]["thread_ms"] is None


def test_actual_milestone_names_are_summarized():
    names = ["tags.read.end", "tags.validate.end", "bsp.read.end", "bsp.validate.end", "bsp.end",
             "objects.begin", "initialize.end", "prepare.wait.begin", "prepare.end"]
    events, _ = benchmark.parse_trace("".join(trace(name, index) for index, name in enumerate(names)))
    spans = {s["phase"]: s for s in benchmark.summarize(events, [], "a10")["spans"]}
    assert set(spans) == {"tags.validate", "bsp.validate", "bsp.register", "objects_and_remaining_initialize", "prepare.wait"}
    assert all(span["wall_ms"] == 1000 for span in spans.values())


@pytest.mark.parametrize("mutation", [
    lambda text: text.replace("first_game_frame.submitted", "unfinished"),
    lambda text: text.replace("initialize.end", "unfinished"),
    lambda text: text.replace("levels\\a10\\a10", "levels\\b30\\b30"),
    lambda text: text.replace("phase=first_game_frame.submitted", "phase=main_new_map.begin"),
    lambda text: text.replace("wall=5.000000", "wall=0.000000"),
    lambda text: text.replace("phase=first_game_frame.submitted wall=5.000000 thread=2.500000 process=10.000000 owner=guest:7",
                              "phase=first_game_frame.submitted wall=5.000000 thread=2.500000 process=10.000000 owner=host:a"),
])
def test_completed_load_requires_requested_ordered_map_and_owner(mutation):
    events, _ = benchmark.parse_trace(mutation(complete_trace()))
    assert not benchmark.completed_load(events, r"levels\a10\a10")


def test_ui_uses_full_scenario_path_for_completion():
    events, _ = benchmark.parse_trace(complete_trace(r"levels\ui\ui"))
    assert benchmark.completed_load(events, "ui")


@pytest.mark.parametrize("text", [
    "LOAD phase=map.begin wall=nan thread=0 process=0 detail=a10",
    "LOAD phase=map.begin wall=-1 thread=0 process=0 detail=a10",
    "LOAD phase=map.begin wall=1 thread=-2 process=0 detail=a10",
    "LOAD phase=map.begin wall=1 wall=2 thread=0 process=0 detail=a10",
    "LOAD_IO op=read wall=1 elapsed=inf requested=1 actual=1 offset=0 path=x",
    "LOAD_IO op=read wall=1 elapsed=0 requested=1 actual=2 offset=0 path=x",
    "LOAD_IO op=read wall=1 elapsed=0 requested=1 actual=1 offset=-1 path=x",
])
def test_invalid_measurement_values_fail(text):
    with pytest.raises(ValueError):
        benchmark.parse_trace(text)


def test_pipeline_miss_retains_timings_but_fails(runner, monkeypatch):
    def launch(command, **kwargs):
        kwargs["stdout"].write(complete_trace() + "vulkan: pipeline missed map preparation: vertex 1\n")
        return SimpleNamespace(returncode=0)
    monkeypatch.setattr(benchmark.subprocess, "run", launch)
    with pytest.raises(RuntimeError):
        execute(runner)
    result = json.loads((runner[4] / "result.json").read_text())
    assert result["load_completed"] and result["pipeline_misses"]
    assert result["spans"] and not result["passed"]


def test_malformed_run_retains_error_and_cache_proof(runner, monkeypatch):
    def launch(command, **kwargs):
        kwargs["stdout"].write("LOAD phase=map.begin wall=bad\n")
        return SimpleNamespace(returncode=0)
    monkeypatch.setattr(benchmark.subprocess, "run", launch)
    proof = [{"path": "owned.map", "after": {"resident_pages": 0}}]
    with pytest.raises(RuntimeError):
        execute(runner, cache_proof=proof)
    assert json.loads((runner[4] / "cache-proof.json").read_text()) == proof
    assert "malformed LOAD" in json.loads((runner[4] / "result.json").read_text())["errors"][0]


@pytest.mark.parametrize("value", ["HALO_NET_ONLINE=true", "HALO_NETWORK_TEST=join", "SDL_VIDEO_DRIVER=x11"])
def test_cli_rejects_unsafe_settings_before_launch(tmp_path, monkeypatch, value):
    monkeypatch.setattr(sys, "argv", ["load_benchmark.py", "--halo", "unused", "--data", "unused",
                                     "--out", str(tmp_path / "output"), "--env", value])
    with pytest.raises(SystemExit) as error:
        benchmark.main()
    assert error.value.code == 2
    assert not (tmp_path / "output").exists()


def test_missing_syscall_timing_is_unknown_not_zero():
    _, io = benchmark.parse_trace("LOAD_IO op=write wall=1 elapsed=0.1 requested=1 actual=1 offset=0 path=x")
    assert benchmark.summarize([], io, "a10")["io"]["write"]["syscall_ms"] is None


@pytest.mark.parametrize("game_cache,shader_cache", [("fresh", "reused"), ("reused", "fresh"), ("fresh", "fresh"), ("reused", "reused")])
def test_main_keeps_game_and_shader_states_independent(tmp_path, monkeypatch, game_cache, shader_cache):
    binary, data, output = tmp_path / "halo", tmp_path / "data", tmp_path / "out"
    binary.write_bytes(b"binary")
    (data / "maps").mkdir(parents=True)
    for name in ("a10", "ui"):
        (data / "maps" / f"{name}.map").write_bytes(b"source map")
    calls, proofs = [], []

    def fake_run(args, scenario, work, saves, shaders, destination, cache_proof=None):
        calls.append((destination.name, list(saves.iterdir()), list(shaders.iterdir()), work))
        destination.mkdir(parents=True)
        (saves / "cache000.map").write_bytes(b"decompressed")
        (shaders / "pipeline.bin").write_bytes(b"pipelines")
        return dict(passed=True, events=[1])

    def fake_prepare(paths, state):
        proofs.append((list(paths), state))
        return [dict(path=str(path), after=dict(pages=1, resident_pages=int(state == "warm"))) for path in paths]

    monkeypatch.setattr(benchmark, "run_game", fake_run)
    monkeypatch.setattr(benchmark, "prepare_files", fake_prepare)
    monkeypatch.setattr(sys, "platform", "linux")
    monkeypatch.setattr(sys, "argv", ["load_benchmark.py", "--halo", str(binary), "--data", str(data),
        "--out", str(output), "--maps", "a10", "--game-cache", game_cache, "--shader-cache", shader_cache])
    assert benchmark.main() == 0
    measured = [call for call in calls if call[0] != "prime"]
    assert len(measured) == 2
    for _, saves, shaders, work in measured:
        assert bool(saves) == (game_cache == "reused")
        assert bool(shaders) == (shader_cache == "reused")
        assert not work.exists()
    assert [state for _, state in proofs] == ["cold", "warm"]
    for paths, _ in proofs:
        assert (data / "maps" / "a10.map") in paths
        assert any(p.name == "cache000.map" for p in paths) == (game_cache == "reused")
    manifest = json.loads((output / "manifest.json").read_text())
    assert manifest["passed"] and all(run["passed"] for run in manifest["runs"])
    assert binary.read_bytes() == b"binary"


def test_main_retains_failed_prime_manifest(tmp_path, monkeypatch):
    binary, output = tmp_path / "halo", tmp_path / "out"
    binary.write_bytes(b"binary")
    work_folders = []
    def fail(args, scenario, work, *unused, **kwargs):
        work_folders.append(work)
        raise RuntimeError("prime never reached first frame")
    monkeypatch.setattr(benchmark, "run_game", fail)
    monkeypatch.setattr(sys, "platform", "linux")
    monkeypatch.setattr(sys, "argv", ["load_benchmark.py", "--halo", str(binary), "--data", str(tmp_path),
                                     "--out", str(output), "--maps", "a10"])
    with pytest.raises(RuntimeError, match="prime never"):
        benchmark.main()
    manifest = json.loads((output / "manifest.json").read_text())
    assert not manifest["passed"] and not manifest["runs"][0]["passed"]
    assert manifest["runs"][0]["kind"] == "prime"
    assert not work_folders[0].exists()
