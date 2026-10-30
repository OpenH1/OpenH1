#!/usr/bin/env python3
"""Internet play's relays end to end (port/linux/src/p2p_relay.c), on one Linux computer with no root: network
namespaces in a user namespace make a small internet (a bridge, 203.0.113.0/24, with a delay on each of its ports),
an MQTT broker and two STUN servers on it (this file's own), and machines behind symmetric NATs (iptables MASQUERADE
--random: a port of its own for each destination, and only answers let in), whose direct connection fails.

    python3 tools/relay_netns.py --game build/linux_x86_64/halo --maps ~/halo-x86-maps/maps [scenario ...]

runs itself again inside `unshare -Urnm` and plays each scenario's debug.network_test games (no window, no sound),
then prints each one's result: whether the joiner reached the host, how (directly, or through which relay), how long
the join took, and the joiner's ping to the host (the latency meter's, debug.network_test's "ping own" lines).

Scenarios: baseline (an open joiner, directly: the ping without a relay), direct-fails (two symmetric NATs, relays
off), peer (two symmetric NATs and an open player in the host's game: the host offers it), server (two symmetric
NATs and a dedicated server with server.relay, named in network.relay_servers), server-corrupt (the same, the relay
receiving a third of its packets damaged: debug.network_corrupt). With --joiner-game an older build joins (baseline:
it joins directly as before; direct-fails, the host's relays on: it fails as before, and the host stops offering it
relays after a few).
"""

import argparse
import asyncio
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BRIDGE_NET = "203.0.113"
arguments_verbose = bool(os.environ.get("RELAY_NETNS_VERBOSE"))
BROKER = f"{BRIDGE_NET}.10"
STUN_SECOND = f"{BRIDGE_NET}.11"
DELAY_MS = 10

# ---------- an MQTT 3.1.1 broker: what the game's signalling uses (no will, QoS 0 deliveries)


def _length(value):
    out = bytearray()
    while True:
        byte = value % 128
        value //= 128
        out.append(byte | (128 if value else 0))
        if not value:
            return bytes(out)


def _topic_matches(pattern, topic):
    parts, names = pattern.split("/"), topic.split("/")
    for index, part in enumerate(parts):
        if part == "#":
            return True
        if index >= len(names) or (part != "+" and part != names[index]):
            return False
    return len(parts) == len(names)


class Broker:
    def __init__(self):
        self.clients = {}
        self.retained = {}

    async def client(self, reader, writer):
        subscriptions = set()
        try:
            while True:
                header = await reader.readexactly(1)
                multiplier, remaining = 1, 0
                while True:
                    byte = (await reader.readexactly(1))[0]
                    remaining += (byte & 127) * multiplier
                    multiplier *= 128
                    if not byte & 128:
                        break
                body = await reader.readexactly(remaining) if remaining else b""
                kind = header[0] >> 4
                if kind == 1:
                    name_length = struct.unpack(">H", body[:2])[0]
                    level = body[2 + name_length]
                    if level != 4:
                        writer.write(bytes([0x20, 2, 0, 1]))
                        await writer.drain()
                        return
                    writer.write(bytes([0x20, 2, 0, 0]))
                    self.clients[writer] = subscriptions
                elif kind == 8:
                    identifier = body[:2]
                    offset, granted = 2, bytearray()
                    while offset < len(body):
                        length = struct.unpack(">H", body[offset:offset + 2])[0]
                        topic = body[offset + 2:offset + 2 + length].decode("utf-8", "replace")
                        offset += 2 + length + 1
                        subscriptions.add(topic)
                        granted.append(0)
                    writer.write(bytes([0x90]) + _length(2 + len(granted)) + identifier + bytes(granted))
                    for topic, payload in list(self.retained.items()):
                        if any(_topic_matches(pattern, topic) for pattern in subscriptions):
                            writer.write(self._publish(topic, payload, True))
                elif kind == 10:
                    offset = 2
                    while offset < len(body):
                        length = struct.unpack(">H", body[offset:offset + 2])[0]
                        subscriptions.discard(body[offset + 2:offset + 2 + length].decode("utf-8", "replace"))
                        offset += 2 + length
                    writer.write(bytes([0xB0, 2]) + body[:2])
                elif kind == 3:
                    qos, retain = (header[0] >> 1) & 3, header[0] & 1
                    length = struct.unpack(">H", body[:2])[0]
                    topic = body[2:2 + length].decode("utf-8", "replace")
                    offset = 2 + length
                    if qos:
                        writer.write(bytes([0x40, 2]) + body[offset:offset + 2])
                        offset += 2
                    payload = body[offset:]
                    if retain:
                        if payload:
                            self.retained[topic] = payload
                        else:
                            self.retained.pop(topic, None)
                    message = self._publish(topic, payload, False)
                    for other, patterns in list(self.clients.items()):
                        if any(_topic_matches(pattern, topic) for pattern in patterns):
                            other.write(message)
                elif kind == 12:
                    writer.write(bytes([0xD0, 0]))
                elif kind == 14:
                    return
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError, IndexError, struct.error):
            pass
        finally:
            self.clients.pop(writer, None)
            writer.close()

    @staticmethod
    def _publish(topic, payload, retain):
        name = topic.encode()
        body = struct.pack(">H", len(name)) + name + payload
        return bytes([0x30 | (1 if retain else 0)]) + _length(len(body)) + body


def serve_broker(address):
    async def main():
        broker = Broker()
        server = await asyncio.start_server(broker.client, address, 1883)
        async with server:
            await server.serve_forever()
    asyncio.run(main())


def serve_stun(address):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((address, 3478))
    while True:
        data, (host, port) = sock.recvfrom(2048)
        if len(data) < 20 or data[:2] != b"\x00\x01":
            continue
        cookie, transaction = data[4:8], data[8:20]
        xport = port ^ 0x2112
        xaddress = bytes(a ^ b for a, b in zip(socket.inet_aton(host), cookie))
        attribute = struct.pack(">HHBBH", 0x0020, 8, 0, 1, xport) + xaddress
        sock.sendto(struct.pack(">HH", 0x0101, len(attribute)) + cookie + transaction + attribute, (host, port))


# ---------- the namespaces


def sh(*command, check=True):
    result = subprocess.run(command, capture_output=True, text=True)
    if check and result.returncode:
        raise RuntimeError(f"{' '.join(command)}: {result.stderr.strip()}")
    return result


class Internet:
    """the bridge, and machines on it: open ones, and ones behind a symmetric NAT"""

    def __init__(self):
        os.makedirs("/run/netns", exist_ok=True)
        sh("ip", "netns", "add", "inet")
        sh("ip", "-n", "inet", "link", "add", "br0", "type", "bridge")
        sh("ip", "-n", "inet", "link", "set", "br0", "up")
        self.count = 0
        self.port("svc", [BROKER, STUN_SECOND])

    def port(self, namespace, addresses, delay=True):
        """namespace on the bridge, with these addresses (the first its own)"""
        self.count += 1
        sh("ip", "netns", "add", namespace)
        sh("ip", "-n", namespace, "link", "set", "lo", "up")
        outside, inside = f"p{self.count}", f"wan{self.count}"
        sh("ip", "link", "add", outside, "type", "veth", "peer", "name", inside)
        sh("ip", "link", "set", outside, "netns", "inet")
        sh("ip", "link", "set", inside, "netns", namespace)
        sh("ip", "-n", "inet", "link", "set", outside, "master", "br0", "up")
        for address in addresses:
            sh("ip", "-n", namespace, "addr", "add", f"{address}/24", "dev", inside)
        sh("ip", "-n", namespace, "link", "set", inside, "up")
        if delay:
            # (each way's delay, on the bridge's side toward the machine)
            sh("ip", "netns", "exec", "inet", "tc", "qdisc", "add", "dev", outside, "root", "netem", "delay",
               f"{DELAY_MS}ms")
        return inside

    def behind_nat(self, name, public, lan):
        """name behind a symmetric NAT router (name-nat) at public"""
        router = f"{name}-nat"
        wan = self.port(router, [public])
        sh("ip", "netns", "add", name)
        sh("ip", "-n", name, "link", "set", "lo", "up")
        sh("ip", "link", "add", f"{name}-lan", "type", "veth", "peer", "name", "eth0")
        sh("ip", "link", "set", f"{name}-lan", "netns", router)
        sh("ip", "link", "set", "eth0", "netns", name)
        sh("ip", "-n", router, "addr", "add", f"{lan}.1/24", "dev", f"{name}-lan")
        sh("ip", "-n", router, "link", "set", f"{name}-lan", "up")
        sh("ip", "-n", name, "addr", "add", f"{lan}.2/24", "dev", "eth0")
        sh("ip", "-n", name, "link", "set", "eth0", "up")
        sh("ip", "-n", name, "route", "add", "default", "via", f"{lan}.1")
        sh("ip", "netns", "exec", router, "sysctl", "-qw", "net.ipv4.ip_forward=1")
        sh("ip", "netns", "exec", router, "iptables", "-t", "nat", "-A", "POSTROUTING", "-o", wan, "-j",
           "MASQUERADE", "--random")
        # (nothing in but answers: conntrack's filter is each destination's own, a symmetric NAT's)
        sh("ip", "netns", "exec", router, "iptables", "-A", "FORWARD", "-i", wan, "-m", "conntrack", "!",
           "--ctstate", "ESTABLISHED,RELATED", "-j", "DROP")
        return f"{lan}.2"

    def close(self):
        result = sh("ip", "netns", "list", check=False)
        for line in result.stdout.splitlines():
            sh("ip", "netns", "delete", line.split()[0], check=False)


# ---------- the games


class Machine:
    def __init__(self, name, namespace, folder, game, maps):
        self.name, self.namespace, self.game = name, namespace, game
        self.folder = folder / name
        (self.folder / "data").mkdir(parents=True)
        (self.folder / "data" / "maps").symlink_to(maps)
        (self.folder / "brokers.txt").write_text(f"{BROKER}:1883\n")
        self.log = self.folder / "out.txt"
        self.process = None

    def start(self, arguments=(), **settings):
        environment = {key: value for key, value in os.environ.items() if not key.startswith("HALO_")}
        environment.update({
            "HALO_DATA_ROOT": str(self.folder / "data"), "HALO_SAVE_ROOT": str(self.folder / "saves"),
            "HALO_CONFIG_FILE": str(self.folder / "config.toml"), "HALO_LOG_ROOT": str(self.folder),
            "SDL_VIDEO_DRIVER": "dummy", "SDL_AUDIO_DRIVER": "dummy", "HALO_NO_AUDIO": "1",
            "HALO_NULL_RENDERER": "1", "HALO_HIDDEN_WINDOW": "1", "HALO_NO_CLIPBOARD": "1",
            "HALO_UPDATE_AUTO": "false", "HALO_DISCORD_APPLICATION": "", "HALO_NET_JOIN_FROM_CLIPBOARD": "0",
            "HALO_NET_ONLINE": "true", "HALO_NET_ALLOW_UPNP": "false", "HALO_NET_PUBLIC_LOBBY": "false",
            "HALO_NET_STUN": f"{BROKER}:3478,{STUN_SECOND}:3478",
            "HALO_NET_BROKERS_FILE": str(self.folder / "brokers.txt"),
            "HALO_NET_LISTINGS": "brokers", "HALO_NETWORK_TEST_NAME": self.name, "HALO_LATENCY_METER": "true",
        })
        environment.update({key: str(value) for key, value in settings.items()})
        command = ["ip", "netns", "exec", self.namespace, self.game, *arguments]
        self.process = subprocess.Popen(command, cwd=self.folder, env=environment, stdin=subprocess.DEVNULL,
                                        stdout=open(self.log, "w"), stderr=subprocess.STDOUT)
        return self

    def text(self):
        return self.log.read_text(errors="replace") if self.log.exists() else ""

    def wait_for(self, pattern, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            match = re.search(pattern, self.text())
            if match:
                return match
            if self.process.poll() is not None:
                return None
            time.sleep(0.25)
        return None

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()


def joined(joiner, seconds=40):
    """how the joiner reached its host: (reached, via, join ms, pings)"""
    match = joiner.wait_for(r"connected to host \w+ at \S+( through a relay \(([^)]*)\))? \((\d+) ms after the join",
                            seconds)
    if not match:
        return False, None, None, []
    # (the game under way: its pings for ten seconds)
    if joiner.wait_for(r"network test: ping own \d+", 60):
        time.sleep(10)
    pings = [int(value) for value in re.findall(r"network test: ping own (-?\d+)", joiner.text())]
    return True, match.group(2) or "direct", int(match.group(3)), [ping for ping in pings if ping >= 0][-8:]


def scenario(name, folder, game, maps, joiner_game=None):
    internet = Internet()
    services = [subprocess.Popen(["ip", "netns", "exec", "svc", sys.executable, __file__, flag, address])
                for flag, address in (("--stun", BROKER), ("--stun", STUN_SECOND), ("--broker", BROKER))]
    time.sleep(1)
    machines = []
    relays_off = {"HALO_NET_USE_RELAYS": "false"}
    try:
        internet.behind_nat("host", f"{BRIDGE_NET}.101", "10.1.0")
        if name in ("baseline", "server-open"):
            internet.port("joiner", [f"{BRIDGE_NET}.40"])
        else:
            internet.behind_nat("joiner", f"{BRIDGE_NET}.102", "10.2.0")
        paths = {"peer": "peer", "server": "server", "server-corrupt": "server"}.get(name, "all")
        common = {"HALO_RELAY_PATHS": paths}
        if name.startswith("server"):
            internet.port("relay", [f"{BRIDGE_NET}.20"])
            server = Machine("relay", "relay", folder, game, maps)
            machines.append(server)
            server.start(["--server"], HALO_SERVER_WEB_PORT=0, HALO_SERVER_PUBLIC="false", HALO_SERVER_RELAY="true",
                         HALO_SERVER_ROTATION="bloodgulch:slayer", HALO_SERVER_CONSOLE="false",
                         **({"HALO_NETWORK_CORRUPT": 33} if name == "server-corrupt" else {}))
            common["HALO_NET_RELAY_SERVERS"] = f"{BRIDGE_NET}.20:2302"
        host = Machine("host", "host", folder, game, maps)
        machines.append(host)
        host.start(HALO_NETWORK_TEST="host:bloodgulch", HALO_NETWORK_TEST_START=50,
                   **(relays_off if name == "direct-fails" and not joiner_game else common))
        invite = host.wait_for(r"network test: invite (\S+)", 60)
        if not invite:
            return {"scenario": name, "error": "the host made no invite"}
        if name == "peer":
            internet.port("player", [f"{BRIDGE_NET}.30"])
            player = Machine("player", "player", folder, game, maps)
            machines.append(player)
            player.start([invite.group(1)], HALO_NETWORK_TEST="join", **common)
            if not joined(player, 40)[0]:
                return {"scenario": name, "error": "the open player did not reach the host"}
        joiner = Machine("joiner", "joiner", folder, joiner_game or game, maps)
        machines.append(joiner)
        joiner.start([invite.group(1)], HALO_NETWORK_TEST="join",
                     **(relays_off if name == "direct-fails" else common))
        reached, via, join_ms, pings = joined(joiner, 45)
        result = {"scenario": name, "reached": reached, "via": via, "join_ms": join_ms, "pings_ms": pings}
        result["crashed"] = [m.name for m in machines if m.process.poll() not in (None, 0)]
        relay_lines = [f"{m.name}: {line.split('Internet play: ', 1)[1]}" for m in machines
                       for line in m.text().splitlines() if "Internet play: " in line and "relay" in line]
        result["relay_log"] = relay_lines[:12]
        if arguments_verbose:
            result["joiner_tail"] = [line for line in joiner.text().splitlines()][-40:]
            result["host_tail"] = [line for line in host.text().splitlines() if re.search("join|reject|machine|connect|Internet", line)][-40:]
        return result
    finally:
        for machine in machines:
            machine.stop()
        for service in services:
            service.kill()
            service.wait()
        internet.close()


def inside(arguments):
    results = []
    for name in arguments.scenarios:
        folder = Path(tempfile.mkdtemp(prefix=f"relay-{name}-", dir=arguments.work))
        result = scenario(name, folder, arguments.game, arguments.maps, arguments.joiner_game)
        results.append(result)
        print(result, flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="build/linux_x86_64/halo")
    parser.add_argument("--maps", default=str(Path.home() / "halo-x86-maps/maps"))
    parser.add_argument("--work", default=None)
    parser.add_argument("--joiner-game", default=None,
                        help="another build for the joiner (an older one, or OpenCE's: it joins as before)")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--broker", help=argparse.SUPPRESS)
    parser.add_argument("--stun", help=argparse.SUPPRESS)
    parser.add_argument("scenarios", nargs="*", default=["baseline", "direct-fails", "peer", "server", "server-corrupt"])
    arguments = parser.parse_args()
    if arguments.broker:
        serve_broker(arguments.broker)
        return 0
    if arguments.stun:
        serve_stun(arguments.stun)
        return 0
    arguments.game = str(Path(arguments.game).resolve())
    if arguments.joiner_game:
        arguments.joiner_game = str(Path(arguments.joiner_game).expanduser().resolve())
    arguments.maps = str(Path(arguments.maps).expanduser().resolve())
    if not arguments.inside:
        work = tempfile.mkdtemp(prefix="relay-netns-")
        try:
            return subprocess.call(["unshare", "-Urnm", "--propagation", "private", "sh", "-c",
                                    "mount -t tmpfs none /run && exec \"$@\"", "-", sys.executable, __file__,
                                    "--inside", "--game", arguments.game, "--maps", arguments.maps, "--work", work,
                                    *(["--joiner-game", arguments.joiner_game] if arguments.joiner_game else []),
                                    *arguments.scenarios])
        finally:
            if os.environ.get("RELAY_NETNS_KEEP"):
                print(f"kept {work}")
            else:
                shutil.rmtree(work, ignore_errors=True)
    return inside(arguments)


if __name__ == "__main__":
    sys.exit(main())
