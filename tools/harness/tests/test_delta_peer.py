"""Delta Peer's handshake (port/linux/src/delta_wire.c, delta_peer.c, ChupathingyCE's cut to it; NETCODE.md, "Delta
Peer"): the wire format (the header, HELLO, WELCOME, LEGACY, the roster, the advertisement's tag) at the spec's offsets;
the capabilities two machines agree to (never the reserved server_badge or a retired bit); a host and a client in one
process: the handshake, the roster, BYE, "takes no tables"; the fallback when a host does not speak Delta or does not
answer (HELLO every 500 ms, legacy at 4 s, then silence); who a host and a client hear; the scoreboard's badges; and
the host's advertisement (delta_peer_game.c), its tag only in a game OpenCE joins, clear of OpenCE's and OpenH1's
bytes."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run  # noqa: E402

CASES = [
    "header", "hello", "welcome", "roster", "tag", "agree", "handshake", "takes-no-tables", "legacy", "fallback",
    "who-is-heard", "badge", "advertise",
]

# a fault in the files, and the case that must catch it: (file, (before, after), case)
NEGATIVE_CONTROLS = {
    # a header's length not checked against the datagram's
    "header-length-unchecked": ("delta_wire.c", ("\treturn header->length == size - DELTA_WIRE_HEADER_SIZE;\n",
                                                 "\treturn 1;\n"), "header"),
    # HELLO's build length taken past its field
    "hello-build-unbounded": ("delta_wire.c", ("\tif (length > DELTA_WIRE_BUILD_SIZE || size < DELTA_WIRE_HELLO_SIZE + "
                                               "length)\n\t\treturn 0;\n\thello->capabilities",
                                               "\tif (size < DELTA_WIRE_HELLO_SIZE)\n\t\treturn 0;\n"
                                               "\thello->capabilities"), "hello"),
    # a roster's entries read past its bytes
    "roster-past-its-bytes": ("delta_wire.c", ("\t\tsize < DELTA_WIRE_ROSTER_SIZE + count * DELTA_WIRE_ROSTER_ENTRY_SIZE)\n"
                                               "\t{\n\t\treturn 0;\n\t}\n\troster->room_capabilities",
                                               "\t\t0)\n\t{\n\t\treturn 0;\n\t}\n\troster->room_capabilities"),
                              "roster"),
    # the tag's check byte not checked
    "tag-check-unchecked": ("delta_wire.c", ("tag[3] != DELTA_ADVERTISED_TAG_VERSION || tag[8] != tag_check(tag))",
                                             "tag[3] != DELTA_ADVERTISED_TAG_VERSION)"), "tag"),
    # the reserved and retired bits agreed to
    "agree-unknown-bits": ("delta_peer.c", ("\treturn theirs & ours & KNOWN_CAPABILITIES;\n",
                                            "\treturn theirs & ours;\n"), "agree"),
    # a client that waits for its host for ever
    "no-fallback": ("delta_peer.c", ("\t\tif (elapsed(now, peer->handshake_start, DELTA_PEER_HANDSHAKE_TIME))\n",
                                     "\t\tif (0)\n"), "fallback"),
    # HELLO said to a host whose advertisement has no tag
    "hello-to-opence": ("delta_peer.c", ("\t\tif (!host_speaks_delta)\n\t\t{\n",
                                         "\t\tif (0)\n\t\t{\n"), "fallback"),
    # a HELLO taken from any address the game has, whatever its index
    "hello-any-machine": ("delta_peer.c", ("\t\t\tif (machine->machine_index == machine_index)\n"
                                           "\t\t\t\treturn machine_index;\n",
                                           "\t\t\treturn machine_index;\n"), "who-is-heard"),
    # a client hearing any port of its host's address
    "client-any-port": ("delta_peer.c", ("ipv4 != peer->host_ipv4 || port != peer->host_port ||",
                                         "ipv4 != peer->host_ipv4 ||"), "who-is-heard"),
    # no rate on a machine's messages
    "host-unlimited": ("delta_peer.c", ("\tif (!valid_machine(first) || !delta_rate_take(&peer->rates[first], now, "
                                        "DELTA_PEER_HOST_RATE,\n\t\tDELTA_PEER_HOST_BURST))",
                                        "\tif (!valid_machine(first))"), "who-is-heard"),
    # another major's HELLO left unanswered
    "no-legacy-answer": ("delta_peer.c", ("\t\t\tpeer_send(peer, ipv4, port, reply, delta_wire_write_empty(reply, "
                                          "DELTA_MAJOR, _delta_message_legacy,\n\t\t\t\theader.session));\n", ""),
                         "legacy"),
    # TABLE_HAVE never said
    "tables-taken": ("delta_peer.c", ("\tpeer_send(peer, ipv4, port, data, delta_wire_write_table_have(data, session, "
                                      "DELTA_WIRE_TABLE_NONE));\n", ""), "takes-no-tables"),
    # the viewer's own badge shown on everyone
    "badge-of-own-kind": ("delta_peer.c", ("\tif (delta_peer_badge(&own, own_badge, (int)sizeof(own_badge)) && "
                                           "!strcmp(own_badge, text))\n",
                                           "\tif (0)\n"), "badge"),
    # the host's build kept from the roster for every machine
    "roster-drops-host-build": ("delta_peer.c", ("\t\t\tif (entry->machine_index == peer->welcome.host_machine_index)"
                                                 "\n\t\t\t\tmemcpy(machine->build, build, sizeof(machine->build));\n",
                                                 ""), "badge"),
    # the tag in a Standard game too
    "tag-in-standard-games": ("delta_peer_game.c", ("\tif (version != HALO_PORT_NETWORK_VERSION)\n",
                                                    "\tif (!version)\n"),
                              "advertise"),
    # the tag without a socket to answer it
    "tag-without-socket": ("delta_peer_game.c", ("delta_game.role != _role_host || !delta_game.port)",
                                                 "delta_game.role != _role_host)"), "advertise"),
}

LIMITS = ("HALO_PORT_NETWORK_VERSION", "HALO_PORT_STANDARD_NETWORK_VERSION", "HALO_PORT_ADVERTISED_VERSION_OFFSET",
          "HALO_PORT_ADVERTISED_BOTS_OFFSET", "HALO_PORT_STANDARD_VERSION_BIT")


def limits():
    """halo_port_limits.h's numbers the advertisement reads, as they are"""
    source = read("port/linux/include/halo_port_limits.h")
    lines = [f"#define {name} {constant(source, name)}" for name in LIMITS]
    lines.append("#define HALO_PORT_STANDARD_ADVERTISED_VERSION "
                 "(HALO_PORT_STANDARD_VERSION_BIT | HALO_PORT_STANDARD_NETWORK_VERSION)")
    return "\n".join(lines) + "\n"


def generated(fault=None):
    files = {
        "delta.h": read("port/linux/include/delta.h"),
        "delta_wire.h": read("port/linux/src/delta_wire.h"),
        "delta_peer.h": read("port/linux/src/delta_peer.h"),
        "delta_wire.c": read("port/linux/src/delta_wire.c"),
        "delta_peer.c": read("port/linux/src/delta_peer.c"),
        "delta_peer_game.c": read("port/linux/game/delta_peer_game.c"),
    }
    if fault:
        name, change, _ = fault
        files[name] = mutated(files[name], *change)
    advertise = function(files.pop("delta_peer_game.c"), "delta_peer_game_advertise")
    # (the sources as .inc: the test is delta_peer.c too)
    return (("delta.h", files["delta.h"]), ("delta_wire.h", files["delta_wire.h"]),
            ("delta_peer.h", files["delta_peer.h"]), ("delta_wire.inc", files["delta_wire.c"]),
            ("delta_peer.inc", files["delta_peer.c"]), ("limits.inc", limits()), ("advertise.inc", advertise))


def test_tag_clear_of_openh1():
    """the tag's first byte past every byte OpenCE and OpenH1 write (the build checks it too)"""
    source = read("port/linux/include/halo_port_limits.h")
    delta = read("port/linux/include/delta.h")
    assert constant(delta, "DELTA_ADVERTISED_RESERVED_SIZE") == 0x1A
    assert constant(source, "HALO_PORT_ADVERTISED_BOTS_OFFSET") < 0x1A - 9
    assert "delta_peer_tag_clear_of_openh1" in read("port/linux/game/delta_peer_game.c")


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("delta_peer", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault = NEGATIVE_CONTROLS[control]
    status, output = run(build("delta_peer", generated(fault)), fault[2])
    assert status == CHECK_FAILED, f"a fault ({control}) passed '{fault[2]}': the test cannot see it"
