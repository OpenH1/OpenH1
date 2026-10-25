"""network.port (HALO_NET_PORT, source/networking/network_game_protocol.h): the game's system link port, the Xbox's
5150 by default (its clients' the next), anywhere; and, with the retail maps in assets/maps and a build of this
computer (build/macos/halo, build/linux_x86_64/halo or build/linux_arm64/halo), two games side by side on the
loopback, each on a port of its own, each host joined by a client of its own. The copies draw nothing and have no
audio, internet play, clipboard or update check; each has 127.0.0.1 alone, as a Mac's loopback has."""

import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
# (StageKit's runner: place(), an executable in a folder of its own, a clone or a link)
sys.path.insert(0, str(ROOT / "tools/stagekit/tests"))
import runner  # noqa: E402

# (assets/maps, or HALO_RETAIL_MAPS: a worktree has none of its own)
RETAIL_MAPS = Path(os.environ.get("HALO_RETAIL_MAPS") or ROOT / "assets/maps")
GAME = next((path for path in (ROOT / "build/macos/halo", ROOT / "build/linux_x86_64/halo",
                               ROOT / "build/linux_arm64/halo") if path.is_file()), None)
SECONDS = 40


def test_default_port_is_the_xboxs():
    """the default is 5150 (0x141E) and the clients' the next, and no part of the game names a port of its own"""
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    assert re.search(r'\{ "network\.port", _config_integer, "5150", "HALO_NET_PORT"', config)
    protocol = (ROOT / "source/networking/network_game_protocol.h").read_text()
    assert "NETWORK_GAME_DEFAULT_SERVER_PORT = 0x141E," in protocol
    globals_text = (ROOT / "source/networking/network_game_globals.c").read_text()
    assert "return (unsigned short)(network_game_server_port() + 1);" in globals_text
    for path in (ROOT / "source").rglob("*.c"):
        text = path.read_text(errors="replace")
        assert not re.search(r"\b0x141[EF]\b", text, re.I), f"{path.relative_to(ROOT)} names the port itself"


def _free_ports(count):
    """count ports p (from 25150, ten apart) whose p and p + 1 no program has, UDP or TCP"""
    found = []
    for port in range(25150, 40000, 10):
        try:
            for number in (port, port + 1):
                for kind in (socket.SOCK_DGRAM, socket.SOCK_STREAM):
                    with socket.socket(socket.AF_INET, kind) as probe:
                        probe.bind(("127.0.0.1", number))
        except OSError:
            continue
        found.append(port)
        if len(found) == count:
            return found
    pytest.skip("no free ports")


def _start(tmp_path, name, test, port, seconds):
    folder = tmp_path / name
    for sub in ("data", "save", "home"):
        (folder / sub).mkdir(parents=True)
    (folder / "data" / "maps").symlink_to(RETAIL_MAPS.resolve())
    # (a copy of its own, beside which it writes its config.toml; its libraries linked)
    game = folder / "halo"
    runner.place(GAME, game)
    for entry in GAME.parent.glob("*.dylib"):
        (folder / entry.name).symlink_to(entry.resolve())
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(folder / "home"),
           "SDL_VIDEO_DRIVER": "dummy", "SDL_AUDIO_DRIVER": "dummy", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
           "HALO_DATA_ROOT": str(folder / "data"), "HALO_SAVE_ROOT": str(folder / "save"),
           "HALO_UPDATE_AUTO": "false", "HALO_NET_ONLINE": "false", "HALO_NET_PUBLIC_LOBBY": "false",
           "HALO_NET_MAP_SHARE": "false", "HALO_NET_MAP_SEED": "false", "HALO_NET_MAP_DOWNLOAD": "false",
           "HALO_NET_JOIN_FROM_CLIPBOARD": "false", "HALO_NET_ALLOW_UPNP": "false", "HALO_NO_CLIPBOARD": "1",
           "HALO_DISCORD_APPLICATION": "", "HALO_NET_ADDRESS": "127.0.0.1", "HALO_NET_BROADCAST": "127.0.0.1",
           "HALO_NET_PORT": str(port), "HALO_NETWORK_TEST": test, "HALO_NETWORK_TEST_START": "8",
           "HALO_TEST_INPUT": "bot:1", "HALO_EXIT_AFTER": str(seconds)}
    log = open(folder / "output.log", "w")
    process = subprocess.Popen([str(game)], cwd=folder, env=env, stdout=log, stderr=subprocess.STDOUT)
    return {"process": process, "log": log, "folder": folder}


def test_two_games_side_by_side(tmp_path):
    if not GAME or not all((RETAIL_MAPS / f"{name}.map").is_file() for name in ("bloodgulch", "beavercreek")):
        pytest.skip("needs bloodgulch.map and beavercreek.map in assets/maps and a build of this computer")
    first, second = _free_ports(2)
    copies = {}
    try:
        copies["host 1"] = _start(tmp_path, "host1", "host:bloodgulch", first, SECONDS + 10)
        copies["host 2"] = _start(tmp_path, "host2", "host:beavercreek", second, SECONDS + 10)
        time.sleep(3)
        copies["client 1"] = _start(tmp_path, "client1", "join", first, SECONDS)
        copies["client 2"] = _start(tmp_path, "client2", "join", second, SECONDS)
        for copy in copies.values():
            copy["process"].wait(timeout=SECONDS + 120)
    finally:
        for copy in copies.values():
            if copy["process"].poll() is None:
                copy["process"].kill()
                copy["process"].wait()
            copy["log"].close()
    text = {name: (copy["folder"] / "output.log").read_text(errors="replace") for name, copy in copies.items()}
    debug = {name: (copy["folder"] / "data/debug.txt").read_text(errors="replace")
             if (copy["folder"] / "data/debug.txt").is_file() else "" for name, copy in copies.items()}
    for name, copy in copies.items():
        assert copy["process"].returncode == 0, f"{name} ended with {copy['process'].returncode}: {text[name][-1500:]}"
    for number, port in ((1, first), (2, second)):
        host, client = f"host {number}", f"client {number}"
        assert f"system link on port {port} (network.port), its clients' {port + 1}" in debug[host]
        assert "failed to create the server connection" not in debug[host], f"{host} could not host on {port}"
        assert re.search(r"bandwidth to host 0 to clients \d+ \(1 clients", text[host]), f"{host} had no client"
        assert "successfully joined a net game; our machine is #1" in debug[client], f"{client} did not join"
    # (each client in its own host's game: its map)
    assert "beavercreek" not in debug["client 1"] and "beavercreek" in debug["client 2"]
