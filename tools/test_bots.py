"""The bots (port/linux/game/bots.c, bots.count) in the game: a local Slayer game on a stock map, played without a
person (debug.network_test "local:", the scripted controller 1 of debug.test_input) for a minute and more, in which
every bot walks the map, the bots kill and die, and the game ends cleanly; and a system link game, whose host plays
them as a local game's. Needs the retail maps in assets/maps and a Linux build (ninja linux_x86_64 or linux_arm64); the game draws
offscreen (SDL's offscreen video driver), with no audio, internet play or update check."""

import math
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

RETAIL_MAPS = Path("assets/maps")
# (the Linux build of this computer's processor)
GAME = next((path for path in (Path("build/linux_x86_64/halo"), Path("build/linux_arm64/halo")) if path.is_file()),
            Path("build/linux_x86_64/halo"))
# a player's line of the network test's each second (network_test.c): alive or dead, its kills, deaths, team and
# machine (a bot's is bots.h's BOTS_FIRST_MACHINE, 64, and up)
PLAYER = re.compile(r"player (\d+): (?:\((-?[\d.]+) (-?[\d.]+) -?[\d.]+\)|dead).*? s-?\d+ k(-?\d+) d(-?\d+) f-?\d+ "
                    r"t-?\d+ m(\d+)")
BOTS_FIRST_MACHINE = 64


def _game(map_name):
    if not GAME.is_file() or not (RETAIL_MAPS / f"{map_name}.map").is_file():
        pytest.skip(f"needs {map_name}.map in assets/maps and a Linux build ({GAME})")


def _address():
    """the loopback address of this game: pytest-xdist's workers each their own, so that their games run at once"""
    worker = os.environ.get("PYTEST_XDIST_WORKER", "")
    index = int(worker[2:]) if worker.startswith("gw") and worker[2:].isdigit() else 0
    return f"127.0.0.{1 + index}"


def _play(tmp_path, test, seconds, **settings):
    """the game played for that many seconds: (exit status, its output, debug.txt)"""
    (tmp_path / "data").mkdir()
    (tmp_path / "save").mkdir()
    (tmp_path / "data" / "maps").symlink_to(RETAIL_MAPS.resolve())
    game = tmp_path / "halo"
    shutil.copy(GAME, game)
    env = {**os.environ, "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy",
           "HALO_DATA_ROOT": str(tmp_path / "data"), "HALO_SAVE_ROOT": str(tmp_path / "save"),
           "HALO_HIDDEN_WINDOW": "1", "HALO_FULLSCREEN": "0", "HALO_NO_VSYNC": "1", "HALO_NO_AUDIO": "1",
           "HALO_UPDATE_AUTO": "false", "HALO_NET_ONLINE": "false", "HALO_NET_ADDRESS": _address(),
           "HALO_NET_BROADCAST": "127.255.255.255", "HALO_NETWORK_TEST": test, "HALO_NETWORK_TEST_START": "5",
           "HALO_NETWORK_TEST_SCORE": "500", "HALO_TEST_INPUT": "bot:1", "HALO_EXIT_AFTER": str(seconds + 20)}
    env.update({name: str(value) for name, value in settings.items()})
    result = subprocess.run([str(game)], cwd=tmp_path, env=env, capture_output=True, text=True,
                            errors="replace", timeout=seconds + 120)
    debug = tmp_path / "data" / "debug.txt"
    return result.returncode, result.stdout + result.stderr, debug.read_text(errors="replace") if debug.is_file() else ""


def _players(output):
    """each player's walk (world units), kills, deaths and machine at the end"""
    players, last = {}, {}
    for line in output.splitlines():
        if "network test: tick" not in line:
            continue
        for match in PLAYER.finditer(line):
            index = int(match.group(1))
            player = players.setdefault(index, {"walked": 0.0})
            player.update(kills=int(match.group(4)), deaths=int(match.group(5)), machine=int(match.group(6)))
            if match.group(2) is None:
                last.pop(index, None)
                continue
            point = (float(match.group(2)), float(match.group(3)))
            # (a step of more than 10 units in a second is a respawn or a teleporter)
            if index in last and math.dist(last[index], point) < 10.0:
                player["walked"] += math.dist(last[index], point)
            last[index] = point
    return players


@pytest.mark.parametrize("map_name,variant,count,skill", [
    ("bloodgulch", "slayer", 6, "normal"),
    ("chillout", "team_slayer", 5, "heroic"),
])
def test_bots_play_a_local_slayer_game(tmp_path, map_name, variant, count, skill):
    _game(map_name)
    status, output, debug = _play(tmp_path, f"local:{map_name}:{variant}", 75, HALO_BOTS=count, HALO_BOT_SKILL=skill)
    assert status == 0, output[-2000:]
    assert not re.search(r"assert|exception", output + debug, re.I), "an assert or exception in the game's log"
    bots = {index: player for index, player in _players(output).items() if player["machine"] >= BOTS_FIRST_MACHINE}
    assert len(bots) == count, f"{len(bots)} bots in the game, not {count}"
    for index, bot in bots.items():
        assert bot["walked"] >= 30.0, f"bot {index} walked {bot['walked']:.0f} units"
    kills = sum(bot["kills"] for bot in bots.values())
    deaths = sum(bot["deaths"] for bot in bots.values())
    assert kills >= count // 2 and deaths > 0, f"the bots killed {kills} and died {deaths} times"
    assert "bots: tick" in output, "no bots line a minute into the game"


# the bots' minute line of the vehicles (bots.c's bots_report)
VEHICLES = re.compile(r"bots: tick \d+, vehicles: driven (\d+) \(Warthog (\d+), Ghost (\d+), Scorpion (\d+), "
                      r"Banshee (\d+)\), gunned (\d+), ridden (\d+), flipped (\d+); seconds driving (\d+), "
                      r"riding (\d+); kills (\d+), deaths (\d+)")
VEHICLE_FIELDS = ("driven", "warthog", "ghost", "scorpion", "banshee", "gunned", "ridden", "flipped", "driving",
                  "riding", "kills", "deaths")


def _vehicles(output):
    """the last minute line's vehicles: seats taken, seconds in them, kills and deaths there"""
    lines = VEHICLES.findall(output)
    assert lines, "no vehicles line a minute into the game"
    return dict(zip(VEHICLE_FIELDS, map(int, lines[-1])))


@pytest.mark.parametrize("variant,kind", [
    ("team_slayer", "warthog"),
    ("team_slayer+team_vehicles=pc", "ghost"),
])
def test_bots_use_the_vehicles(tmp_path, variant, kind):
    """bots.vehicles on Blood Gulch, two minutes of Team Slayer: the bots drive the Warthogs (and the Ghosts, with the
    PC's vehicles), ride a teammate's Warthog's gun, and kill from them"""
    _game("bloodgulch")
    status, output, debug = _play(tmp_path, f"local:bloodgulch:{variant}", 120, HALO_BOTS=8)
    assert status == 0, output[-2000:]
    assert not re.search(r"assert|exception", output + debug, re.I), "an assert or exception in the game's log"
    vehicles = _vehicles(output)
    assert vehicles[kind] >= 1, f"no bot drove a {kind}: {vehicles}"
    assert vehicles["driving"] >= 60, f"the bots drove {vehicles['driving']} seconds: {vehicles}"
    if kind == "warthog":
        assert vehicles["gunned"] >= 1, f"no bot rode a Warthog's gun: {vehicles}"
    assert vehicles["kills"] >= 1, f"no kill from a vehicle: {vehicles}"


def test_bots_in_a_system_link_game(tmp_path):
    """a system link game's host plays its bots as a local game's does: each a player of a machine of its own"""
    _game("bloodgulch")
    status, output, _ = _play(tmp_path, "host:bloodgulch:slayer", 75, HALO_BOTS=6)
    assert status == 0, output[-2000:]
    players = _players(output)
    assert players, "the system link game did not start"
    bots = {index: player for index, player in players.items() if player["machine"] >= BOTS_FIRST_MACHINE}
    assert len(bots) == 6, f"{len(bots)} bots in the system link game, not 6"
    assert sum(bot["walked"] for bot in bots.values()) >= 6 * 30.0, "the bots stood still"
    assert "bots: tick" in output, "no bots line a minute into the game"


def test_many_bots_in_a_system_link_game(tmp_path):
    """up to bots.h's 64 bots in one game (ADD 48 in a game of 64), each a machine of its own from
    BOTS_FIRST_MACHINE up, playing a minute"""
    _game("bloodgulch")
    status, output, debug = _play(tmp_path, "host:bloodgulch:slayer", 75, HALO_BOTS=48, HALO_BOTS_MAKE_ROOM="false",
                                  HALO_NETWORK_TEST_MOST_PLAYERS=64)
    assert status == 0, output[-2000:]
    assert not re.search(r"assert|exception", output + debug, re.I), "an assert or exception in the game's log"
    lines = re.findall(r"bots: tick \d+, (\d+) bots", output)
    assert lines and int(lines[-1]) == 48, f"bots playing a minute in: {lines}"
