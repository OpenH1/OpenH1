"""Internet play's relays (port/linux/src/p2p_relay.c): the relay pairs two machines only by the token of a session
both hold (an HMAC of its secret, which gives neither the secret nor the tunnel's keys away), forwards only tunnel
packets and only between the pair's two addresses, within its limits of pairs, binds and bytes a second, ends idle
pairs, survives anything sent to it; the machines' attempts bind, take the pairing and pass packets both ways; and the
fallback order (p2p_relay_stage): directly, then the game's machines, then the dedicated servers'."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = [
    "pairing", "secret", "same-side", "limits-pairs", "rate-bytes", "rate-binds", "idle", "only-tunnel",
    "peers-only", "attempt", "attempt-expiry", "through", "malformed", "fallback-order",
]

# a fault in p2p_relay.c and the case that must catch it
NEGATIVE_CONTROLS = {
    "any-token-pairs": (("pair = pair_of_token(token);", "pair = pairs_used() ? &relay.pairs[0] : NULL;"),
                        "secret"),
    "one-side-twice": (("side = &pair->sides[side_index];\n\t\tif (side->bound && (side->address != address",
                        "side = &pair->sides[side_index];\n\t\tif (0 && side->bound && (side->address != address"),
                       "same-side"),
    "from-anyone": (("\telse\n\t\treturn;\n\tto = &pair->sides[from == &pair->sides[0]];",
                     "\telse\n\t\tfrom = &pair->sides[0];\n\tto = &pair->sides[from == &pair->sides[0]];"),
                    "secret"),
    "no-pair-limit": (("if (pairs_used() >= relay.limits.maximum_pairs ||", "if (0 ||"), "limits-pairs"),
    "no-address-limit": (("pairs_of_address(address) >= relay.limits.pairs_per_address)\n\t{",
                          "0)\n\t{"), "limits-pairs"),
    "no-burst": (("long burst = (long)((long long)rate * BUCKET_BURST_TIME / 1000);", "long burst = 0;"),
                 "rate-bytes"),
    "no-pair-bucket": (("!bucket_spend(&pair->bytes, relay.limits.pair_upload, size, now) ||", ""), "rate-bytes"),
    "no-bind-bucket": (("if (!bucket_spend(&heard->binds, relay.limits.binds_per_second, 1, now))",
                        "if (0)"), "rate-binds"),
    "never-idle": (("else if (relay_elapsed(now, pair->active_time, IDLE_TIME))", "else if (0)"), "idle"),
    "any-payload": ((" || packet[DATA_HEADER_SIZE] != P2P_TUNNEL_MAGIC)\n\t\treturn;\n\tpair = pair_of_channel",
                     ")\n\t\treturn;\n\tpair = pair_of_channel"), "only-tunnel"),
    "strangers-relayed": (("if (relay.limits.peers_only && !p2p_relay_peer_address(address, port))\n\t\treturn;", ""),
                          "peers-only"),
    "delivered-from-anyone": (("attempt->relay.address == address &&\n\t\t\tattempt->relay.port == port && "
                               "!memcmp(attempt->channel",
                               "1 &&\n\t\t\tattempt->relay.port == port && !memcmp(attempt->channel"), "attempt"),
    "forwarded-again": (("\tcase _type_delivered:\n\t\tdelivered_received(",
                         "\tcase _type_delivered:\n\t\tif (relay.limits.enabled)\n\t\t\tdata_received(packet, size, "
                         "address, port, now);\n\t\tdelivered_received("), "pairing"),
    "token-is-secret": (("p2p_hmac_sha256(secret, P2P_SHA256_SIZE, label, (int)sizeof(label) - 1, token);",
                         "memcpy(token, secret, P2P_RELAY_TOKEN_SIZE);"), "secret"),
    "servers-first": (("\tif (age >= RELAY_SERVER_DELAY ||", "\tif (1 ||"), "fallback-order"),
    "relay-before-direct": (("\tif (age < (unsigned long)(both_strict ? RELAY_STRICT_DELAY : RELAY_DELAY))",
                             "\tif (age < (unsigned long)(both_strict ? RELAY_STRICT_DELAY : 0))"), "fallback-order"),
    "waiting-side-untold": (("send_bound(other->address, other->port, _bound_paired, other->nonce, pair->channel);",
                             ""), "pairing"),
}

STUB_HEADERS = (
    ("platform.h", "/* (the test's) */\n"),
    ("posix.h", "void posix_random_bytes(void *buffer, unsigned long size);\n"),
)


def crypto():
    """p2p_crypto.c's SHA-256, HMAC and constant-time comparison, as they are"""
    source = read("port/linux/src/p2p_crypto.c")
    start = source.index("struct sha256\n{")
    hmac = function(source, "p2p_hmac_sha256")
    end = source.index(hmac) + len(hmac)
    return source[start:end] + "\n" + function(source, "p2p_equal") + "\n"


def generated(fault=None):
    relay = read("port/linux/src/p2p_relay.c")
    if fault:
        relay = mutated(relay, *fault)
    files = tuple((name, read(f"port/linux/src/{name}")) for name in ("p2p.h", "p2p_internal.h"))
    return files + STUB_HEADERS + (("under_test.inc", crypto() + relay),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("p2p_relay", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("p2p_relay", generated(fault)), case)
    assert status == CHECK_FAILED, f"p2p_relay.c with a fault ({control}) passed '{case}': the test cannot see it"
