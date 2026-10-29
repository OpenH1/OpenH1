"""A team's shared sightings (port/linux/game/bots_knowledge.c, bots.share_sightings): a bot's sighting of an enemy
reaches its teammates bots.share_delay after it, as a callout would; while one report is on its way the next waits, then goes with
the last sighting there is, so an enemy kept in sight is known a delay late and a delay apart; a sighting is of use for
BOTS_KNOWLEDGE_SEEN_MEMORY_TICKS; the team's memory is bounded, the oldest let go. What a bot knows
(bots_knowledge_slot_known) is its own sightings and the reports that reached its team, never a sighting still on its way,
and with sharing off its own alone."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run, structure  # noqa: E402

CASES = ["delay", "cadence", "no-delay", "expiry", "bounded", "known"]
FUNCTIONS = ("bots_knowledge_team_pass", "bots_knowledge_team_note", "bots_knowledge_fresh",
             "bots_knowledge_sighting_player", "bots_knowledge_known_add", "bots_knowledge_slot_known")

# a fault in a function, and the case that must catch it
NEGATIVE_CONTROLS = {
    "no-delay": ("bots_knowledge_team_pass", ("now - entry->sent_tick >= delay_ticks", "1"), "delay"),
    "each-sighting-resends": ("bots_knowledge_team_note", ("if (entry->sent.tick <= 0)", "if (1)"), "cadence"),
    "next-report-not-sent": ("bots_knowledge_team_pass", ("entry->sent = entry->latest;", ";"), "cadence"),
    "never-forgotten": ("bots_knowledge_fresh", ("&& now - sighting->tick <= BOTS_KNOWLEDGE_SEEN_MEMORY_TICKS", ""),
                        "expiry"),
    "newest-let-go": ("bots_knowledge_team_note", ("memory[index].latest.tick < memory[oldest].latest.tick",
                                                  "memory[index].latest.tick > memory[oldest].latest.tick"), "bounded"),
    "team-latest-known": ("bots_knowledge_slot_known", ("&bots_knowledge.team_seen[team][index].heard",
                                                        "&bots_knowledge.team_seen[team][index].latest"), "known"),
    "shared-when-off": ("bots_knowledge_slot_known", ("if (bots_knowledge.share && team != NONE)", "if (team != NONE)"),
                        "known"),
    "life-before-known": ("bots_knowledge_sighting_player", ("player->unit_index != sighting->unit_index", "0"),
                          "known"),
}


def generated(fault=None):
    header = read("port/linux/game/bots_knowledge.h")
    source = read("port/linux/game/bots_knowledge.c")
    config = f"enum {{ TICKS_PER_SECOND = {constant(read('source/cseries/cseries.h'), 'TICKS_PER_SECOND')} }};\n"
    for name in ("BOTS_KNOWLEDGE_SEEN_MEMORY_TICKS", "BOTS_KNOWLEDGE_MAXIMUM_KNOWN"):
        match = re.search(rf"\b{name} = ([^,]+),", header)
        assert match, f"{name} not found in bots_knowledge.h"
        config += f"enum {{ {name} = {match.group(1)} }};\n"
    for name in ("BOTS_KNOWLEDGE_BOT_MEMORY", "BOTS_KNOWLEDGE_TEAM_MEMORY"):
        found = re.search(rf"\b{name} = (\d+),", source)
        assert found, f"{name} not found in bots_knowledge.c"
        config += f"enum {{ {name} = {found.group(1)} }};\n"
    config += structure(source, "bots_knowledge_sighting") + "\n"
    config += structure(source, "bots_knowledge_team_sighting") + "\n"
    config += structure(header, "bots_known_enemy") + "\n"
    code = ""
    for name in FUNCTIONS:
        text = function(source, name)
        if fault and fault[0] == name:
            text = mutated(text, *fault[1])
        code += text + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("bots_sightings", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    name, fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("bots_sightings", generated((name, fault))), case)
    assert status == CHECK_FAILED, f"the sightings with a fault ({control}) passed '{case}': the test cannot see it"
