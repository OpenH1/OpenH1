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


def test_loading_epoch_includes_entry_and_exit_present_gaps():
    text = (trace("gl.swap.return", 0.9) + trace("loading.begin", 1, "epoch=1 serial=0")
            + trace("loading.swap.return", 1.1, "epoch=1 serial=1 elapsed_ns=100000000")
            + trace("loading.swap.return", 1.2, "epoch=1 serial=2 elapsed_ns=200000000")
            + trace("loading.end", 1.4, "epoch=1 serial=2") + trace("gl.swap.return", 1.5))
    events, operations = benchmark.parse_trace(text)
    epoch = benchmark.summarize(events, operations, "a10")["loading_epochs"][0]
    assert epoch["loading_frame_count"] == 2
    assert epoch["serials_contiguous"] and epoch["animation_clock_increasing"]
    assert not epoch["pose_each_frame"]
    stream = epoch["presentations"]["gl"]
    assert stream["boundary_samples_complete"]
    assert stream["maximum_clipped_gap_ms"] == pytest.approx(200)
    assert stream["maximum_bracketing_gap_ms"] == pytest.approx(300)


def test_loading_pose_and_missing_boundaries_are_not_assumed():
    text = (trace("loading.begin", 1, "epoch=7 serial=0")
            + trace("loading.pose", 1.01, "epoch=7 serial=1 predicted=100 valid=1")
            + trace("xr.submit.end", 1.02)
            + trace("loading.submitted", 1.021, "epoch=7 serial=1 elapsed_ns=100")
            + trace("loading.pose", 1.03, "epoch=7 serial=3 predicted=100 valid=0")
            + trace("xr.submit.end", 1.04)
            + trace("loading.submitted", 1.041, "epoch=7 serial=3 elapsed_ns=100")
            + trace("loading.end", 1.05, "epoch=7 serial=3")
            + trace("loading.begin", 2, "epoch=8 serial=0"))
    events, _ = benchmark.parse_trace(text)
    epoch, unfinished = benchmark.loading_epochs(events)
    assert epoch["pose_each_frame"]
    assert not epoch["pose_valid"]
    assert not epoch["predicted_time_increasing"]
    assert not epoch["serials_contiguous"]
    assert epoch["serials_increasing"]
    assert epoch["loading_frame_count"] == 2
    assert not epoch["animation_clock_increasing"]
    assert epoch["presentations"]["xr"]["maximum_bracketing_gap_ms"] is None
    assert not unfinished["completed"]
    assert epoch["world_anchor_stable"] is None
    assert epoch["world_space_each_frame"] is None
    assert epoch["head_orientation_max_delta_degrees"] is None


@pytest.mark.parametrize("second_anchor,stable", [("0,1,-2", True), ("0,1,-3", False)])
def test_loading_head_motion_is_independent_of_world_anchor(second_anchor, stable):
    text = trace("loading.begin", 1, "epoch=1 serial=0")
    for serial, orientation, position in ((1, "0,0,0,1", "0,1,-2"),
                                          (2, "0,0.70710678,0,0.70710678", second_anchor)):
        text += trace("loading.pose", 1 + serial * .01,
                      f"epoch=1 serial={serial} predicted={serial} valid=1 orientation={orientation}")
        text += trace("loading.submitted", 1 + serial * .01 + .001,
                      f"epoch=1 serial={serial} elapsed_ns={serial} world_space=1 "
                      f"anchor_position={position} anchor_orientation=0,0,0,1")
    text += trace("loading.end", 1.1, "epoch=1 serial=2")
    events, _ = benchmark.parse_trace(text)
    epoch = benchmark.loading_epochs(events)[0]
    assert epoch["head_orientation_samples"] == 2
    assert epoch["head_orientation_max_delta_degrees"] == pytest.approx(90)
    assert epoch["world_space_each_frame"]
    assert epoch["world_anchor_stable"] is stable
    assert epoch["world_anchor_unique"] == (1 if stable else 2)


def test_metal_loading_callbacks_count_separately_from_game_frames():
    text = (trace("metal.presented", .9, "display_time=9.9 image=1")
            + trace("loading.begin", 1, "epoch=1 serial=0")
            + trace("loading.presented", 1.1, "epoch=1 serial=1 elapsed_ns=100000000 display_time=10.1")
            + trace("loading.end", 1.2, "epoch=1 serial=1")
            + trace("metal.presented", 1.3, "display_time=10.3 image=1"))
    events, _ = benchmark.parse_trace(text)
    epoch = benchmark.loading_epochs(events)[0]
    assert epoch["loading_frame_count"] == 1
    assert epoch["serials_increasing"]
    assert epoch["presentations"]["metal"]["maximum_bracketing_gap_ms"] == pytest.approx(200)


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
    assert env["HALO_NET_ONLINE"] == "false" and env["HALO_NO_CLIPBOARD"] == "1"
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


def test_explicit_virtual_display_is_preserved(runner, monkeypatch):
    runner[0].video_driver = "x11"
    monkeypatch.setenv("DISPLAY", ":193")
    execute(runner)
    env = runner[-1][0]["env"]
    assert env["SDL_VIDEO_DRIVER"] == "x11" and env["DISPLAY"] == ":193"
    assert env["HALO_NET_ONLINE"] == "false"


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


def test_warming_later_file_cannot_hide_earlier_page_reclaim(tmp_path, monkeypatch):
    first, second = tmp_path / "first.map", tmp_path / "second.map"
    first.write_bytes(b"first")
    second.write_bytes(b"second")
    calls = []
    def resident(path):
        calls.append(path)
        count = 0 if path == first and calls.count(first) >= 3 else 1
        return dict(bytes=5, pages=1, resident_pages=count)
    monkeypatch.setattr(benchmark, "residency", resident)
    with pytest.raises(RuntimeError, match="warm file-cache residency not verified"):
        benchmark.prepare_files([first, second], "warm")
    assert calls == [first, first, second, second, first, second,
                     first, second, first, second]


def test_warm_retry_reads_only_incomplete_files_then_rechecks_every_file(tmp_path, monkeypatch):
    first, second = tmp_path / "first.map", tmp_path / "second.map"
    first.write_bytes(b"first")
    second.write_bytes(b"second")
    calls, reads = [], []
    original_open = type(first).open
    def opened(path, *args, **kwargs):
        reads.append(path)
        return original_open(path, *args, **kwargs)
    def resident(path):
        calls.append(path)
        count = 0 if path == first and calls.count(first) == 3 else 1
        return dict(bytes=5, pages=1, resident_pages=count)
    monkeypatch.setattr(type(first), "open", opened)
    monkeypatch.setattr(benchmark, "residency", resident)
    proof = benchmark.prepare_files([first, second], "warm")
    assert reads == [first, second, first]
    assert calls == [first, first, second, second, first, second, first, second]
    assert [item["warm_reads"] for item in proof] == [2, 1]
    assert [check["resident_pages"] for check in proof[0]["warm_checks"]] == [0, 1]
    assert all(item["after"]["resident_pages"] == item["after"]["pages"] for item in proof)


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


@pytest.mark.parametrize("failure", ["pipeline missed map preparation", "EXCEPTION"])
def test_keep_going_retains_failures_and_attempts_both_cache_states(tmp_path, monkeypatch, capsys, failure):
    binary, output = tmp_path / "halo", tmp_path / "out"
    binary.write_bytes(b"binary")
    calls = []
    def fail(args, scenario, work, saves, shaders, destination, **kwargs):
        calls.append(destination.name)
        destination.mkdir(parents=True)
        (saves / "cache000.map").write_bytes(b"cache")
        (shaders / "pipeline.bin").write_bytes(b"shader")
        (destination / "result.json").write_text(json.dumps(dict(passed=False, events=[], errors=[failure])))
        raise RuntimeError(failure)
    monkeypatch.setattr(benchmark, "run_game", fail)
    monkeypatch.setattr(benchmark, "prepare_files", lambda paths, state: [])
    monkeypatch.setattr(sys, "platform", "linux")
    monkeypatch.setattr(sys, "argv", ["load_benchmark.py", "--halo", str(binary), "--data", str(tmp_path),
                                     "--out", str(output), "--maps", "a10", "--keep-going"])
    assert benchmark.main() == 1
    assert calls == ["prime", "0-cold", "0-warm"]
    manifest = json.loads((output / "manifest.json").read_text())
    assert not manifest["passed"]
    assert all(not run["passed"] and failure in run["error"] for run in manifest["runs"])
    assert failure in capsys.readouterr().err
    assert not list(output.glob(".work-*"))


def boundary_trace(stream):
    text = trace("loading.request", .995, "begin") + trace("loading.begin", 1, "epoch=1")
    if stream == "xr":
        for when, predicted, layers in ((.99, 1, 5), (1.01, 2, 1), (1.03, 3, 1), (1.05, 4, 5), (1.10, 5, 5)):
            text += trace("xr.wait.end", when - .002, f"predicted={predicted} render=1")
            if layers == 1:
                text += trace("loading.pose", when - .001, f"epoch=1 serial={predicted - 1} predicted={predicted} valid=1")
            text += trace("xr.submit.end", when, f"predicted={predicted} layers={layers}")
            if layers == 1:
                text += trace("loading.submitted", when + .0001, f"epoch=1 serial={predicted - 1} elapsed_ns={predicted}")
    elif stream == "vulkan":
        for when in (.99, 1.01, 1.03, 1.05, 1.10):
            text += trace("vulkan.present.return", when, "result=0 image=1")
        text += trace("loading.submitted", 1.0101, "epoch=1 serial=1 elapsed_ns=1")
        text += trace("loading.submitted", 1.0301, "epoch=1 serial=2 elapsed_ns=2")
    else:
        for when in (.99, 1.05, 1.10):
            text += trace("gl.swap.return", when, "paced=1")
        text += trace("loading.swap.return", 1.01, "epoch=1 serial=1 elapsed_ns=1")
        text += trace("loading.swap.return", 1.03, "epoch=1 serial=2 elapsed_ns=2")
    text += trace("loading.ready", 1.04, "epoch=1 serial=2")
    text += trace("loading.end", 1.051, "epoch=1 serial=2")
    return text


@pytest.mark.parametrize("stream", ["xr", "vulkan", "gl"])
def test_actual_handoff_excludes_second_game_frame(stream):
    events, _ = benchmark.parse_trace(boundary_trace(stream))
    epoch = benchmark.loading_epochs(events)[0]
    presentation = epoch["presentations"][stream]
    actual = presentation["actual_boundaries"]
    assert presentation["maximum_bracketing_gap_ms"] == pytest.approx(50)
    assert actual["entry_gap_ms"] == pytest.approx(20)
    assert actual["internal_max_gap_ms"] == pytest.approx(20)
    assert actual["first_game_handoff_ms"] == pytest.approx(20)
    assert actual["maximum_transition_gap_ms"] == pytest.approx(20)
    assert actual["request_to_first_loading_ms"] == pytest.approx(15)


def test_xr_nonrenderable_previous_frame_is_not_entry_proof():
    text = boundary_trace("xr").replace("predicted=1 render=1", "predicted=1 render=0")
    events, _ = benchmark.parse_trace(text)
    actual = benchmark.loading_epochs(events)[0]["presentations"]["xr"]["actual_boundaries"]
    assert actual["entry_gap_ms"] is None
    assert actual["maximum_transition_gap_ms"] is None
    assert actual["first_game_handoff_ms"] == pytest.approx(20)


@pytest.mark.parametrize("before,after", [
    ("predicted=2 render=1", "predicted=2 render=0"),
    ("predicted=2 layers=1", "predicted=2 layers=0"),
    ("predicted=2 valid=1", "predicted=2 valid=0"),
])
def test_xr_loading_marker_without_rendered_layers_is_not_presentation_proof(before, after):
    events, _ = benchmark.parse_trace(boundary_trace("xr").replace(before, after))
    actual = benchmark.loading_epochs(events)[0]["presentations"]["xr"]["actual_boundaries"]
    assert not actual["loading_samples_complete"]
    assert actual["maximum_transition_gap_ms"] is None


def test_missing_readiness_cannot_prove_first_game_handoff():
    events, _ = benchmark.parse_trace(boundary_trace("vulkan").replace("phase=loading.ready", "phase=unknown"))
    actual = benchmark.loading_epochs(events)[0]["presentations"]["vulkan"]["actual_boundaries"]
    assert actual["first_game_handoff_ms"] is None


@pytest.mark.parametrize("stream", ["xr", "vulkan"])
def test_ambiguous_or_failed_loading_present_does_not_prove_cadence(stream):
    text = boundary_trace(stream)
    if stream == "xr":
        text += trace("xr.submit.end", 1.01005, "predicted=2 layers=1")
    else:
        text = text.replace(trace("vulkan.present.return", 1.01, "result=0 image=1"),
                            trace("vulkan.present.return", 1.01, "result=-1 image=1"))
    events, _ = benchmark.parse_trace(text)
    actual = benchmark.loading_epochs(events)[0]["presentations"][stream]["actual_boundaries"]
    assert not actual["loading_samples_complete"]
    assert actual["internal_max_gap_ms"] is None
    assert actual["first_game_handoff_ms"] is None


def test_vulkan_legacy_unmarked_presents_do_not_prove_game_picture():
    events, _ = benchmark.parse_trace(boundary_trace("vulkan").replace(" image=1", ""))
    actual = benchmark.loading_epochs(events)[0]["presentations"]["vulkan"]["actual_boundaries"]
    assert actual["entry_gap_ms"] is None
    assert actual["first_game_handoff_ms"] is None
    assert actual["internal_max_gap_ms"] == pytest.approx(20)


def test_vulkan_black_present_is_not_first_game_picture():
    text = boundary_trace("vulkan") + trace("vulkan.present.return", 1.045, "result=0 image=0")
    events, _ = benchmark.parse_trace(text)
    actual = benchmark.loading_epochs(events)[0]["presentations"]["vulkan"]["actual_boundaries"]
    assert actual["first_game_time"] == pytest.approx(1.05)
    assert actual["first_game_handoff_ms"] == pytest.approx(20)


def test_metal_late_out_of_order_callbacks_use_drawable_time():
    text = (trace("metal.presented", .9, "display_time=9.98 image=1 input=1")
            + trace("loading.begin", 1, "epoch=1")
            + trace("loading.ready", 1.4, "epoch=1 serial=2")
            + trace("loading.end", 1.5, "epoch=1 serial=2")
            + trace("metal.presented", 1.6, "display_time=10.04 image=1 input=2")
            + trace("metal.presented", 2, "display_time=10.02 image=0 input=0")
            + trace("loading.presented", 2.001, "epoch=1 serial=2 elapsed_ns=2 display_time=10.02")
            + trace("metal.presented", 3, "display_time=10 image=0 input=0")
            + trace("loading.presented", 3.001, "epoch=1 serial=1 elapsed_ns=1 display_time=10"))
    events, _ = benchmark.parse_trace(text)
    epoch = benchmark.loading_epochs(events)[0]
    assert epoch["loading_frame_count"] == 2
    assert epoch["serials_increasing"] and epoch["animation_clock_increasing"]
    actual = epoch["presentations"]["metal"]["actual_boundaries"]
    assert actual["clock"] == "drawable_presented_time"
    assert actual["entry_gap_ms"] == pytest.approx(20)
    assert actual["internal_max_gap_ms"] == pytest.approx(20)
    assert actual["first_game_handoff_ms"] == pytest.approx(20)
    assert actual["request_to_first_loading_ms"] is None
