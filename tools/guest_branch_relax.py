"""Relax the conditional branches of the guest's converted assembly that no
longer reach their targets.

The compiler lays out each function and relaxes its own branches for the
code it emits (LLVM's branch relaxation). tools/guest_base_register.py then
adds instructions (an add before most memory accesses), and
tools/android_asm_convert.py a few (a conversion's replacement): in a large
function a branch the compiler placed within reach may end up beyond it, and
the assembler stops with "fixup value out of range" (menu_functions.c's
pc_menu_game_data_function_invoke, whose tbnz reach only 32 KB). This pass
measures the code as it is now and replaces each conditional branch whose
target is out of reach (with a margin) by the opposite branch over an
unconditional one, which reaches 128 MB:

    tbnz w8, #0, .LBB23_2761  ->  tbz w8, #0, .Lrelax_0 ; b .LBB23_2761 ; .Lrelax_0:
    cbz w0, .LBB1_4           ->  cbnz w0, .Lrelax_1 ; b .LBB1_4 ; .Lrelax_1:
    b.hi .LBB2_9              ->  b.ls .Lrelax_2 ; b .LBB2_9 ; .Lrelax_2:

It measures each line's size from above (an instruction 4 bytes, data its
largest, an alignment its most padding), so a branch it leaves alone is in
reach whatever the assembler makes of the rest; a branch it relaxes makes
the code longer, so it measures again until no other branch has to be.

A function past FUNCTION_LIMIT is an error naming it: near 1 MB, the reach
of what is not relaxed here (adr, a literal load), it should be split.
"""

import re
from typing import Dict, List, Optional, Tuple

# what each branch reaches, in bytes either way (its immediate's), less a
# margin
MARGIN = 1024
REACH = {"tbz": 32768, "tbnz": 32768, "cbz": 1048576, "cbnz": 1048576, "b.": 1048576}
# a function this large is an error (half the reach of adr and b.cond)
FUNCTION_LIMIT = 512 * 1024

OPPOSITE = {"tbz": "tbnz", "tbnz": "tbz", "cbz": "cbnz", "cbnz": "cbz"}
OPPOSITE_CONDITION = {"eq": "ne", "ne": "eq", "cs": "cc", "cc": "cs", "hs": "lo", "lo": "hs", "mi": "pl",
                      "pl": "mi", "vs": "vc", "vc": "vs", "hi": "ls", "ls": "hi", "ge": "lt", "lt": "ge",
                      "gt": "le", "le": "gt"}

LABEL = re.compile(r'^([A-Za-z_.$][\w.$]*):(.*)$')
CONDITIONAL = re.compile(r'^\s*(tbz|tbnz|cbz|cbnz|b\.([a-z]{2}))\s+(.*?)\s*$')
DATA_SIZES = {".byte": 1, ".hword": 2, ".short": 2, ".2byte": 2, ".word": 4, ".long": 4, ".4byte": 4,
              ".int": 4, ".inst": 4, ".xword": 8, ".quad": 8, ".8byte": 8, ".dword": 8}


class RelaxError(Exception):
    pass


def _code(line: str) -> str:
    """a line without its comment"""
    for marker in ("//", "#"):
        at = line.find(marker)
        # ("#" is also an immediate's: only a comment at the line's start)
        if marker == "//" and at >= 0:
            line = line[:at]
        elif marker == "#" and line.lstrip().startswith("#"):
            return ""
    return line.strip()


def _size(text: str) -> int:
    """the most bytes a line (no label, no comment) can take in its section"""
    if not text:
        return 0
    if not text.startswith("."):
        return 4
    parts = text.split(None, 1)
    directive, operands = parts[0], parts[1] if len(parts) > 1 else ""
    if directive in DATA_SIZES:
        return DATA_SIZES[directive] * (operands.count(",") + 1)
    if directive in (".p2align", ".align", ".balign"):
        value = int(operands.split(",")[0], 0)
        return value if directive == ".balign" else (1 << value)
    if directive in (".zero", ".space", ".skip"):
        return int(operands.split(",")[0], 0)
    if directive in (".ascii", ".asciz", ".string"):
        return len(operands) + 1
    return 0


def _section(text: str, current: str, stack: List[str]) -> str:
    """the section a directive line switches to (current if none)"""
    parts = text.split(None, 1)
    directive = parts[0] if parts else ""
    if directive in (".text", ".data", ".bss"):
        return directive
    if directive == ".section":
        return parts[1].split(",")[0].strip()
    if directive == ".pushsection":
        stack.append(current)
        return parts[1].split(",")[0].strip()
    if directive == ".popsection":
        return stack.pop() if stack else current
    return current


def _branch(text: str) -> Optional[Tuple[str, str, str]]:
    """a conditional branch's kind (its REACH key), its operands but the
    target, and its target"""
    match = CONDITIONAL.match(text)
    if not match:
        return None
    mnemonic, condition, operands = match.group(1), match.group(2), match.group(3)
    pieces = [piece.strip() for piece in operands.split(",")]
    if condition is not None:
        if condition not in OPPOSITE_CONDITION or len(pieces) != 1:
            return None
        return "b.", condition, pieces[0]
    return mnemonic, ", ".join(pieces[:-1]), pieces[-1]


def _measure(lines: List[str], relaxed: set) -> Tuple[Dict[str, Tuple[str, int]],
                                                     List[Tuple[int, str, int, str, str]],
                                                     List[Tuple[str, int, int]]]:
    """each label's section and offset; each conditional branch's line,
    section, offset, target and kind; each function's name, start and end"""
    labels: Dict[str, Tuple[str, int]] = {}
    branches: List[Tuple[int, str, int, str, str]] = []
    functions: List[Tuple[str, int, int]] = []
    offsets: Dict[str, int] = {}
    section, stack = ".text", []
    function: Optional[Tuple[str, int]] = None
    for number, line in enumerate(lines):
        text = _code(line)
        match = LABEL.match(text)
        if match:
            name, text = match.group(1), match.group(2).strip()
            labels[name] = (section, offsets.get(section, 0))
            if section == ".text" and not name.startswith(".L"):
                if function:
                    functions.append((function[0], function[1], offsets.get(section, 0)))
                function = (name, offsets.get(section, 0))
        if text.startswith("."):
            section = _section(text, section, stack)
        branch = _branch(text) if not text.startswith(".") else None
        if branch:
            branches.append((number, section, offsets.get(section, 0), branch[2], branch[0]))
        offsets[section] = offsets.get(section, 0) + (8 if number in relaxed else _size(text))
    if function:
        functions.append((function[0], function[1], offsets.get(".text", 0)))
    return labels, branches, functions


def relax(lines: List[str]) -> Tuple[List[str], int]:
    """the lines with each conditional branch out of reach relaxed, and how
    many were; RelaxError for a function past FUNCTION_LIMIT"""
    relaxed: set = set()
    while True:
        labels, branches, functions = _measure(lines, relaxed)
        added = 0
        for number, section, offset, target, kind in branches:
            if number in relaxed or target not in labels or labels[target][0] != section:
                continue
            if abs(labels[target][1] - offset) > REACH[kind] - MARGIN:
                relaxed.add(number)
                added += 1
        if not added:
            break
    for name, start, end in functions:
        if end - start > FUNCTION_LIMIT:
            raise RelaxError(f"function {name} is {(end - start) // 1024} KB of code, near the 1 MB that its adr "
                             f"and literal loads reach: split it (tools/guest_branch_relax.py)")
    out: List[str] = []
    for number, line in enumerate(lines):
        if number not in relaxed:
            out.append(line)
            continue
        text = _code(line)
        match = LABEL.match(text)
        if match:
            out.append(f"{match.group(1)}:")
            text = match.group(2).strip()
        kind, operands, target = _branch(text)
        skip = f".Lrelax_{len(out)}_{number}"
        if kind == "b.":
            out.append(f"\tb.{OPPOSITE_CONDITION[operands]} {skip}")
        else:
            mnemonic = text.split(None, 1)[0]
            out.append(f"\t{OPPOSITE[mnemonic]} {operands}, {skip}")
        out.append(f"\tb {target}")
        out.append(f"{skip}:")
    return out, len(relaxed)
