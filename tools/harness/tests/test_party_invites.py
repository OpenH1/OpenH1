"""Parties made in a network game (port/linux/game/network_party.c): what the scoreboard's player menu offers whom
(an OpenH1 player who can party; never an OpenCE machine's player, a bot, or anyone in a Legacy game), the messages'
format and guards (the host takes a client's only from its stream and its own players, a few at a time, and sends an
invite on only to the OpenH1 machine it is for, never an OpenCE one), and an invite accepted, which joins the party
by its link as opening the link does."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, mutated, read, run  # noqa: E402

CASES = ["actions", "route", "list", "status", "accept", "invite", "holds", "friends"]

# a fault in network_party.c, and the case that must catch it
NEGATIVE_CONTROLS = {
    "offered-to-opence": (("!player || player->local_player_index != NONE || opence ||",
                           "!player || player->local_player_index != NONE ||"), "actions"),
    "offered-to-bots": (("\t\tbots_machine_is_bot(player->network_player_data.machine_index) ||\n\t\t!TEST_FLAG(known",
                         "\t\t!TEST_FLAG(known"), "actions"),
    "offered-in-legacy-games": (("network_standard_agreed_rate() != 0;", "1;"), "actions"),
    "offered-to-the-unheard": (("\t\t!TEST_FLAG(known->flags, _party_status_online_bit))\n\t{\n\t\treturn 0;",
                                "\t\t0)\n\t{\n\t\treturn 0;"), "actions"),
    "routed-to-opence": (("party_globals.machines[machine_index].heard && "
                          "network_standard_machine_is_openh1(machine_index);",
                          "party_globals.machines[machine_index].heard;"), "list"),
    "opence-word-taken": (("if (machine_index < 0 || machine_index >= MAXIMUM_PARTY_MACHINES ||\n"
                           "\t\t!network_standard_machine_is_openh1(machine_index))",
                           "if (machine_index < 0 || machine_index >= MAXIMUM_PARTY_MACHINES)"), "route"),
    "routed-to-the-unheard": (("party_globals.machines[machine_index].heard && ", ""), "route"),
    "datagrams-taken": (("if (!from_stream || !party_globals.active", "if (!party_globals.active"), "route"),
    "any-sender-taken": (("\t\t!distributed_machine_has_player(machine_index, request->player_index))", "\t\t0)"),
                         "route"),
    "no-rate": (("\t\tif (*burst <= 0)\n\t\t\treturn;", ""), "route"),
    "other-format-taken": (("if (request.format != PARTY_FORMAT || request.kind >= NUMBER_OF_PARTY_REQUESTS)",
                            "if (request.kind >= NUMBER_OF_PARTY_REQUESTS)"), "route"),
    "machine-gone-kept": (("\t\tif (present[index])\n\t\t\tcontinue;", "\t\tif (1)\n\t\t\tcontinue;"), "route"),
    "list-every-time": (("(!changed && party_globals.machines[machine_index].listed)", "(changed && 0)"), "list"),
    "status-legacy": (("\tif (!party_game())\n\t{\n\t\tif (party_globals.active)\n\t\t\tparty_reset();\n\t\treturn;",
                       "\tif (0)\n\t{\n\t\treturn;"), "status"),
    "status-carries-nothing": (("SET_FLAG(party_globals.own_flags, _party_status_in_party_bit, "
                                "party_globals.own_tag != 0);", ";"), "status"),
    "muted-heard": (("\t\tnetwork_voice_machine_muted(sender->network_player_data.machine_index))", "\t\t0)"),
                    "accept"),
    "invite-never-fades": (("now - party_globals.prompt.time >= PARTY_PROMPT_MILLISECONDS ||", ""), "accept"),
    "any-answer-joins": (("party_globals.asked_player_index == from && ", ""), "accept"),
    "accept-joins-nothing": (("\t\tparty_join_token(party_globals.prompt.token, name);\n\t\treturn TRUE;",
                              "\t\treturn TRUE;"), "accept"),
    "button-ignored": (("|| (button && !party_globals.button_held)", ""), "accept"),
    "let-in-not-answer": (("if (party_invite(player_index, TRUE))", "if (party_invite(player_index, FALSE))"),
                          "invite"),
    "hold-never-asked": (("\tparty_hold_update(now);\n", ""), "holds"),
    "hold-asked-each-time": (("\tif (*count == party_globals.hold_asked_count &&",
                              "\tif (0 && *count == party_globals.hold_asked_count &&"), "holds"),
    "hold-for-opence": (("\tif (!network_standard_machine_is_openh1(machine_index) || machine_index < 0 ||",
                         "\tif (machine_index < 0 ||"), "holds"),
    "hold-unanswered": (("\tdistributed_send_to_machine_reliably(machine_index, &message, _distributed_message_party_held, 1,",
                         "\tif (0) distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_party_held, 1,"),
                        "holds"),
    "holds-not-advertised": (("return party_globals.active && party_host() ? party_holds_count(party_globals.holds) : 0;",
                              "return 0;"), "holds"),
    "held-members-in-game-again": (("\t\tif (!party_key_hash_present(key_hashes + index * PARTY_KEY_HASH_SIZE))",
                                    "\t\tif (1)"), "holds"),
    "grants-unlimited": (("\tif (party_globals.machines[group].hold_grants >= PARTY_HOLD_GRANTS)", "\tif (0)"),
                         "holds"),
    "holds-never-lapse": (("\t\tshort released = party_holds_expire(party_globals.holds, now);",
                           "\t\tshort released = 0;"), "holds"),
    "leader-gone-holds-kept": (("\t\tif (party_holds_release(party_globals.holds, index))",
                                "\t\tif (0)"), "holds"),
    "own-machine-forgotten": (("\t\tif (own >= 0 && own < MAXIMUM_PARTY_MACHINES)\n\t\t\tpresent[own] = TRUE;",
                               "\t\t(void)own;"), "holds"),
    "taken-not-let-go": (("\tparty_globals.holds[index].used = FALSE;\n\terror(_error_log, \"party: machine #%ld took",
                          "\terror(_error_log, \"party: machine #%ld took"), "holds"),
    "joiner-of-no-party": (("\tgroup = party_machine_group(machine_index, address);",
                            "\tgroup = party_machine_group(machine_index, 0);"), "holds"),
    "asked-before-taken-in": ((" || (!party_host() && !party_client_accepted()) ||", " ||"), "holds"),
    "unanswered-never-asked-again": (("party_globals.hold_answered || party_globals.hold_asks >= PARTY_HOLD_ASKS ||",
                                      "1 ||"), "holds"),
    "asked-again-for-ever": (("party_globals.hold_answered || party_globals.hold_asks >= PARTY_HOLD_ASKS ||",
                              "party_globals.hold_answered ||"), "holds"),
    "answer-not-heard": (("\t\tparty_globals.hold_answered = TRUE;\n", ""), "holds"),
    # friends: the codes and the recent players
    "codes-to-anyone": (("\t\tif (!party_machine_told(machine_index) || (!changed && "
                         "party_globals.machines[machine_index].friends_listed))",
                         "\t\tif (!changed && party_globals.machines[machine_index].friends_listed)"), "friends"),
    "code-without-its-bit": (("party_globals.machines[machine_index].has_code = TEST_FLAG(request->flags, "
                              "_party_status_friend_code_bit);", "party_globals.machines[machine_index].has_code = TRUE;"),
                             "friends"),
    "codes-every-time": (("(!changed && party_globals.machines[machine_index].friends_listed)", "(changed && 0)"),
                         "friends"),
    "codes-of-any-size": (("\t\t\tsize != sizeof(header) + header.count * sizeof(struct distributed_friend_player))",
                           "\t\t\t0)"), "friends"),
    "codes-of-any-format": (("if (players[index].format != FRIEND_FORMAT || players[index].player_index >= ",
                             "if (players[index].player_index >= "), "friends"),
    "status-without-code": (("\t\tif (party_globals.has_own_code)\n\t\t\tcsmemcpy(request.token, "
                             "party_globals.own_code, FRIEND_CODE_SIZE);\n", ""), "friends"),
    "add-a-friend-again": (("\t\tparty_friend_addable(p2p_friends_relation(party_globals.codes[absolute_index].code)))",
                            "\t\t1)"), "friends"),
    "add-without-profile": (("if (party_game() && party_globals.has_own_code && player->local_player_index == NONE &&",
                             "if (party_game() && player->local_player_index == NONE &&"), "friends"),
    "own-player-recent": (("\t\tif (!player || player->local_player_index != NONE ||\n"
                           "\t\t\tbots_machine_is_bot(player->network_player_data.machine_index))\n\t\t{\n\t\t\tcontinue;",
                           "\t\tif (!player ||\n\t\t\tbots_machine_is_bot(player->network_player_data.machine_index))"
                           "\n\t\t{\n\t\t\tcontinue;"), "friends"),
    "legacy-codes-noted": (("coded = party_game() && party_globals.codes[player_index].has;",
                            "coded = party_globals.codes[player_index].has;"), "friends"),
    "recent-every-frame": (("\tif (party_globals.recent_time && now - party_globals.recent_time < "
                            "FRIEND_RECENT_MILLISECONDS)\n\t\treturn;\n", ""), "friends"),
}


def generated(fault=None):
    limits = read("port/linux/include/halo_port_limits.h")
    distributed = read("port/linux/game/network_distributed.h")
    keyboard = read("port/linux/include/halo_keyboard.h")
    config = "".join(f"#define {name} {constant(limits, name)}\n" for name in (
        "HALO_PORT_MAXIMUM_NETWORK_MACHINES", "HALO_PORT_MAXIMUM_NETWORK_PLAYERS"))
    for name in ("_distributed_message_party_up", "_distributed_message_party_down",
                 "_distributed_message_party_players", "_distributed_message_player_substeps",
                 "_distributed_message_party_hold", "_distributed_message_party_held",
                 "_distributed_message_friend_players"):
        value = re.search(rf"\b{name} = (\d+)", distributed).group(1)
        config += f"#define {name} {value}\n"
    first = re.search(r"#define FIRST_OPENH1_DISTRIBUTED_MESSAGE (\w+)", distributed).group(1)
    config += f"#define FIRST_OPENH1_DISTRIBUTED_MESSAGE {first}\n"
    config += enum_with(distributed, "_distributed_to_host_reliably") + "\n"
    config += enum_with(keyboard, "HALO_KEYBOARD_PARTY_ACCEPT") + "\n"
    config += enum_with(keyboard, "HALO_CHAT_PAD_DOWN") + "\n"
    p2p = read("port/linux/src/p2p.h")
    config += "".join(f"#define {name} {constant(p2p, name)}\n"
                      for name in ("P2P_PLAYER_KEY_HASH_SIZE", "P2P_PARTY_MAXIMUM_MEMBERS", "P2P_FRIEND_CODE_BYTES"))
    # (friends' relations and results: p2p_friends.c's)
    config += enum_with(p2p, "\t_p2p_friend_request,") + "\n" + enum_with(p2p, "_p2p_friends_done") + "\n"
    header = read("port/linux/game/network_party.h")
    # (the actions, and the prototypes: the code's functions call each other in any order)
    config += header[header.index("/* ---------- constants */"):header.rindex("#endif")]
    source = read("port/linux/game/network_party.c")
    # (the whole file from its constants to its drawing: its globals and functions)
    text = source[source.index("/* ---------- constants */"):source.index("/* ---------- drawing */")]
    if fault:
        text = mutated(text, *fault)
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("party_invites", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("party_invites", generated(fault)), case)
    assert status == CHECK_FAILED, f"network_party.c with a fault ({control}) passed '{case}': the test cannot see it"
