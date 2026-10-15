"""Rewrite the guest's AArch64 assembly so that its memory lies anywhere.

The guest (ILP32 AArch64 code, port/android/README.md) keeps 32-bit
pointers, which on Linux and Android are used as addresses themselves: its
4 GB live in the low 4 GB of the host process. macOS on Apple Silicon keeps
the low 4 GB from every process (__PAGEZERO), so there the guest's 4 GB lie
at a base the host chooses, aligned to 4 GB, and every address the guest
uses is that base plus a 32-bit offset. This pass makes the code do so, as
LFI (Lightweight Fault Isolation, Yedidia, ASPLOS 2024) rewrites AArch64
code into a sandbox:

- x21 holds the base and x22 is a scratch register; the guest is compiled
  with both reserved (-ffixed-x21 -ffixed-x22), and the host puts the base in
  x21 whenever it enters guest code (port/android/host/host_enter.S).
  Neither is ever written by guest code, and both are callee-saved, so the
  host's functions keep them.
- Every general register but sp, x29 and x30 holds a pointer as its 32-bit
  offset, zero-extended, as arm64_32 code expects of pointers in registers
  (a pointer argument converted to a 64-bit integer is used as it is). The
  instructions that would otherwise put a full address (base plus offset)
  in one are made 32-bit, which costs nothing:
    add x0, sp, #16          ->  add w0, wsp, #16
    sub x8, x29, #8          ->  sub w8, w29, #8
    add x8, x8, :lo12:sym    ->  add w8, w8, :lo12:sym   (after adrp)
  An adrp result is a full address; it is only ever the base of a
  [xN, :lo12:sym] access, which is left as it is, or the source of such an
  add. An adr result (jump tables) is zero-extended after it.
- Each load and store through another general register xN adds the base to
  the 32-bit offset in wN:
    ldr w0, [x1]             ->  ldr w0, [x21, w1, uxtw]
    ldr w0, [x1, #8]         ->  add w22, w1, #8 ; ldr w0, [x21, w22, uxtw]
    ldr w0, [x1, x2, lsl #2] ->  add w22, w1, w2, lsl #2 ; ldr w0, [x21, w22, uxtw]
    ldp x0, x1, [x1, #16]    ->  add x22, x21, w1, uxtw ; ldp x0, x1, [x22, #16]
    ldp x0, x1, [x1, #16]!   ->  add x1, x1, #16 ; add x22, x21, w1, uxtw ; ldp x0, x1, [x22]
  The guarded form [x21, wN, uxtw] costs nothing more than [xN]; an offset
  is added in 32 bits first (a one-cycle add, where the extending
  add x22, x21, wN, uxtw takes two), modulo 4 GB as ILP32 pointer
  arithmetic is (an unscaled access, ldur, becomes the scaled one, ldr,
  so). The instructions without that addressing mode (pairs, exclusives,
  SIMD structures) take the extending add.
- sp and x29 always hold full addresses: accesses through them are left as
  they are (most of a function's memory accesses), and an instruction that
  sets either from another register is followed by a rebase
  (add sp, x21, wN, uxtw).
- Indirect branches go to the base plus the target's offset
  (blr x8 -> add x22, x21, w8, uxtw ; blr x22). Returns use x30 as bl and
  blr set it, or as a function's epilogue reloads it from its frame record
  (a function that uses x30 as an ordinary register in between saves it
  first), so it is a full address, maybe the host's own when the host
  called the guest; a return is left as it is.

The stack itself is in guest memory (the host makes every guest thread's
stack there), as the stack's addresses are taken and stored as pointers.

Anything this pass does not know how to rewrite (an addressing form, a
write of x21 or any use of x22 in compiled code, a 64-bit comparison with sp
or x29) is an error, never left alone.
"""

import re
from typing import List, Optional, Tuple

BASE = "x21"
SCRATCH = "x22"

# ld*/st* (loads and stores, exclusives, acquire/release, SIMD structure
# loads), the LSE atomics and prefetches
MEMORY_MNEMONIC = re.compile(
    r"^(ld|st|prfm|prfum|cas|swp)[a-z0-9]*(\.[a-z0-9]+)?$")
# the ones with a register-offset addressing mode ([xN, wM, uxtw])
REGISTER_OFFSET_MNEMONICS = {
    "ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldrsb", "ldrsh", "ldrsw", "prfm",
}
COMPARISONS = {"cmp", "cmn", "tst", "ccmp", "ccmn"}
UNSCALED = {"ldur": "ldr", "ldurb": "ldrb", "ldurh": "ldrh", "ldursb": "ldrsb", "ldursh": "ldrsh",
            "ldursw": "ldrsw", "stur": "str", "sturb": "strb", "sturh": "strh", "prfum": "prfm"}
INDIRECT_BRANCHES = {"br", "blr", "ret", "braa", "brab", "blraa", "blrab"}
GPR = re.compile(r"^(?:[xw](\d+)|sp|wsp|xzr|wzr|fp|lr)$")


class BaseRegisterError(Exception):
    pass


def split_operands(text: str) -> List[str]:
    """Top-level operands: commas inside [] and {} do not split."""
    result, depth, current = [], 0, []
    for c in text:
        if c in "[{":
            depth += 1
        elif c in "]}":
            depth -= 1
        if c == "," and depth == 0:
            result.append("".join(current).strip())
            current = []
        else:
            current.append(c)
    if current or result:
        result.append("".join(current).strip())
    return result


def register_number(name: str) -> Optional[int]:
    """The number of a general register (x/w0-30), 29 for fp, 30 for lr,
    31 for sp, None for anything else (zr, vector registers, immediates)."""
    name = name.strip().lower()
    if name in ("fp",):
        return 29
    if name in ("lr",):
        return 30
    if name in ("sp", "wsp"):
        return 31
    m = re.match(r"^[xw](\d+)$", name)
    if m and int(m.group(1)) <= 30:
        return int(m.group(1))
    return None


def parse_immediate(text: str) -> int:
    text = text.strip()
    if text.startswith("#"):
        text = text[1:]
    return int(text, 0)


def encodable(value: int) -> bool:
    """Whether add's 12-bit immediate (shifted by 12 or not) holds value."""
    return 0 <= value < 4096 or (value % 4096 == 0 and value < 4096 * 4096)


def add_immediate(register: str, amount: str) -> str:
    """add or sub of an immediate operand text (#n, #-n, #0x..)."""
    value = parse_immediate(amount)
    if value < 0:
        return f"\tsub {register}, {register}, #{-value}"
    return f"\tadd {register}, {register}, #{value}"


class Rewriter:
    def __init__(self):
        self.added = 0
        self.rewritten = 0

    def full_base(self, number: int) -> bool:
        """sp and x29 hold full addresses."""
        return number in (29, 31)

    def rebase(self, register: str) -> str:
        n = register_number(register)
        if n == 31:
            raise BaseRegisterError("rebase of sp needs a scratch register")
        return f"\tadd x{n}, {BASE}, w{n}, uxtw"

    def check_reserved(self, mnemonic: str, operands: List[str], line: str):
        """x22 is the rewrite's own; x21 may be read (the guest's C reads the
        base for the host's pointers) but not written."""
        if any(re.search(r"\b[xw]22\b", op) for op in operands):
            raise BaseRegisterError(f"x22 is reserved for the base register: {line.strip()}")
        stores = mnemonic.startswith("st") or mnemonic.startswith("prf")
        written = [] if stores or mnemonic in COMPARISONS else operands[:1]
        if mnemonic.startswith("ld") or mnemonic.startswith(("cas", "swp")):
            written = [op for op in operands if not op.startswith("[")]
        if any(re.search(r"\b[xw]21\b", op) for op in written):
            raise BaseRegisterError(f"a write to x21, the base register: {line.strip()}")

    def memory(self, mnemonic: str, operands: List[str], line: str) -> List[str]:
        # the memory operand, and a post-index offset after it
        index = next((i for i, op in enumerate(operands) if op.startswith("[")), None)
        if index is None:
            # a literal load (ldr x0, label): PC-relative, inside the image
            return [line]
        memory = operands[index]
        post = operands[index + 1:]
        if len(post) > 1:
            raise BaseRegisterError(f"unexpected operands after the address: {line.strip()}")
        pre_index = memory.endswith("!")
        if pre_index:
            memory = memory[:-1].strip()
        if not memory.endswith("]"):
            raise BaseRegisterError(f"cannot parse the address: {line.strip()}")
        parts = [p.strip() for p in split_operands(memory[1:-1])]
        base = parts[0]
        number = register_number(base)
        if number is None:
            raise BaseRegisterError(f"unexpected base register: {line.strip()}")
        others = operands[:index]
        loads_x29 = mnemonic.startswith("ld") and any(register_number(o) == 29 for o in others)
        # (an index of zero: the base alone)
        if len(parts) == 3 and parts[1].lower() in ("wzr", "xzr"):
            parts = [base]
        if len(parts) == 2 and parts[1].startswith(":lo12:") and not pre_index and not post:
            # a global, through the adrp of its page (a full address)
            return [line]
        if self.full_base(number):
            # a frame record or a spill: left as it is
            out = [line]
            if loads_x29 and number != 31:
                out.append(self.rebase("x29"))
                self.added += 1
            return out
        head = f"\t{mnemonic} " + ", ".join(others)
        before: List[str] = []
        after: List[str] = []
        xn, wn = f"x{number}", f"w{number}"

        if pre_index:
            if len(parts) != 2 or post or not parts[1].startswith("#"):
                raise BaseRegisterError(f"unexpected pre-index form: {line.strip()}")
            before.append(add_immediate(xn, parts[1]))
            parts = [base]
        elif post:
            amount = post[0]
            if amount.startswith("#"):
                after.append(add_immediate(xn, amount))
            elif register_number(amount) is not None:
                after.append(f"\tadd {xn}, {xn}, {amount}")
            else:
                raise BaseRegisterError(f"unexpected post-index form: {line.strip()}")
            if len(parts) != 1:
                raise BaseRegisterError(f"unexpected post-index form: {line.strip()}")

        register_offset = mnemonic in REGISTER_OFFSET_MNEMONICS
        # (an unscaled access, ldur or stur, at wN + i is the scaled one's at
        # [x21, w22, uxtw] once w22 = wN + i)
        unscaled = UNSCALED.get(mnemonic)
        if len(parts) == 1 or (len(parts) == 2 and re.fullmatch(r"#(0x0+|0+)", parts[1])):
            if register_offset:
                address = f"[{BASE}, {wn}, uxtw]"
            else:
                before.append(f"\tadd {SCRATCH}, {BASE}, {wn}, uxtw")
                address = f"[{SCRATCH}]"
        elif len(parts) == 2 and parts[1].startswith("#"):
            value = parse_immediate(parts[1])
            if (register_offset or unscaled) and encodable(abs(value)):
                before.append(f"\t{'sub' if value < 0 else 'add'} w22, {wn}, #{abs(value)}")
                address = f"[{BASE}, w22, uxtw]"
                if unscaled:
                    head = f"\t{unscaled} " + ", ".join(others)
            else:
                before.append(f"\tadd {SCRATCH}, {BASE}, {wn}, uxtw")
                address = f"[{SCRATCH}, {parts[1]}]"
        elif len(parts) in (2, 3) and register_number(parts[1]) is not None:
            if not register_offset:
                raise BaseRegisterError(f"register offset on {mnemonic}: {line.strip()}")
            extend = parts[2] if len(parts) == 3 else ""
            index_register = parts[1]
            if index_register.startswith("w") and not extend:
                raise BaseRegisterError(f"w index without extend: {line.strip()}")
            if extend:
                m = re.fullmatch(r"(lsl|uxtw|sxtw|sxtx|uxtx)(?:\s+#?(\d+))?", extend)
                if not m:
                    raise BaseRegisterError(f"unexpected extend: {line.strip()}")
                kind, shift = m.group(1), m.group(2)
                if kind == "lsl" and shift is None:
                    raise BaseRegisterError(f"lsl without an amount: {line.strip()}")
                # (in 32 bits: the low half of a 64-bit index's sum is the
                # same)
                kind = {"sxtx": "lsl", "uxtx": "lsl"}.get(kind, kind)
                operand = f"w{register_number(index_register)}, {kind}" + (f" #{shift}" if shift is not None else "")
                before.append(f"\tadd w22, {wn}, {operand}")
            else:
                before.append(f"\tadd w22, {wn}, w{register_number(index_register)}")
            address = f"[{BASE}, w22, uxtw]"
        else:
            raise BaseRegisterError(f"unexpected addressing form: {line.strip()}")

        out = before + [f"{head}, {address}" if others else f"\t{mnemonic} {address}"] + after
        if loads_x29:
            out.append(self.rebase("x29"))
        self.added += len(out) - 1
        self.rewritten += 1
        return out

    def narrowed(self, mnemonic: str, operands: List[str], line: str) -> List[str]:
        """The 32-bit form of an add, sub or mov whose result is a pointer:
        its offset, zero-extended."""
        if mnemonic not in ("add", "sub", "mov", "adds", "subs"):
            raise BaseRegisterError(f"an address taken by an unexpected instruction: {line.strip()}")
        out = []
        for operand in operands:
            number = register_number(operand)
            if number is not None:
                out.append("wsp" if number == 31 else f"w{number}")
                continue
            m = re.fullmatch(r"(uxtx|sxtx|lsl)(\s+#?\d+)?", operand)
            if m:
                kind = {"uxtx": "uxtw", "sxtx": "sxtw", "lsl": "lsl"}[m.group(1)]
                out.append(kind + (m.group(2) or ""))
                continue
            out.append(operand)
        self.rewritten += 1
        return [f"\t{mnemonic} " + ", ".join(out)]

    def writes(self, mnemonic: str, operands: List[str], line: str) -> List[str]:
        """A data-processing instruction: rebase what it writes to sp or x29,
        and keep what it writes elsewhere an offset."""
        if not operands:
            return [line]
        if mnemonic == "adrp":
            return [line]
        destination = register_number(operands[0])
        if mnemonic == "adr":
            self.added += 1
            return [line, f"\tmov w{destination}, w{destination}"]
        if mnemonic in COMPARISONS:
            # (their low halves are the offsets: a 32-bit comparison is one
            # of offsets, a 64-bit one would not be)
            if any(register_number(o) in (29, 31) and o.lower().startswith(("x", "sp", "fp")) for o in operands):
                raise BaseRegisterError(f"a 64-bit comparison with sp or x29: {line.strip()}")
            return [line]
        if destination is not None and destination != 29 and destination != 31 and operands[0].lower().startswith("x"):
            sources = [register_number(o) for o in operands[1:]]
            if any(source in (29, 31) for source in sources) or any(":lo12:" in o for o in operands[1:]):
                return self.narrowed(mnemonic, operands, line)
        destination = register_number(operands[0])
        if destination not in (29, 31) or mnemonic in COMPARISONS:
            return [line]
        if operands[0].lower().startswith("w"):
            raise BaseRegisterError(f"a 32-bit write to the frame pointer: {line.strip()}")
        # from sp or x29, plus or minus an amount (sub sp, sp, #n; mov x29,
        # sp; add sp, x29, #n; sub sp, sp, x8): full already. Anything else
        # is rebased, whatever it holds.
        if len(operands) > 1 and register_number(operands[1]) in (29, 31):
            return [line]
        if destination == 31:
            body = ", ".join([SCRATCH] + operands[1:])
            self.added += 1
            self.rewritten += 1
            return [f"\t{mnemonic} {body}", f"\tadd sp, {BASE}, w22, uxtw"]
        self.added += 1
        self.rewritten += 1
        return [line, self.rebase("x29")]

    def branch(self, mnemonic: str, operands: List[str], line: str) -> List[str]:
        if mnemonic == "ret" and (not operands or register_number(operands[0]) == 30):
            return [line]
        if len(operands) != 1 or register_number(operands[0]) in (None, 31):
            raise BaseRegisterError(f"unexpected indirect branch: {line.strip()}")
        number = register_number(operands[0])
        self.added += 1
        self.rewritten += 1
        return [f"\tadd {SCRATCH}, {BASE}, w{number}, uxtw", f"\t{mnemonic} {SCRATCH}"]

    def instruction(self, line: str) -> List[str]:
        text = line.strip()
        parts = text.split(None, 1)
        mnemonic = parts[0].lower()
        operands = split_operands(parts[1]) if len(parts) > 1 else []
        self.check_reserved(mnemonic, operands, line)
        if MEMORY_MNEMONIC.match(mnemonic) and any(op.startswith("[") for op in operands):
            return self.memory(mnemonic, operands, line)
        if mnemonic in INDIRECT_BRANCHES:
            return self.branch(mnemonic, operands, line)
        if mnemonic in ("bl", "b", "cbz", "cbnz", "tbz", "tbnz") or mnemonic.startswith("b."):
            return [line]
        if mnemonic in ("hint", "nop", "brk", "udf", "isb", "dmb", "dsb", "yield", "clrex", "svc", "hlt",
                        "msr", "mrs", "paciasp", "autiasp", "bti"):
            if mnemonic in ("msr", "mrs") and operands and register_number(operands[0]) in (29, 30, 31):
                raise BaseRegisterError(f"unexpected system register move: {text}")
            return [line]
        return self.writes(mnemonic, operands, line)


# the instructions the rewrite adds that may run twice to the same effect
# (rewrite's double: tools/base_register_bench measures their cost so)
IDEMPOTENT = re.compile(r"^\t(add x22, |add w22, |sub w22, |add sp, x21, |add x29, x21, |mov (w\d+), \2$)")


def rewrite(lines: List[str], double: bool = False) -> Tuple[List[str], Rewriter]:
    """Rewrites the instructions of ELF assembly (tools/android_asm_convert.py's
    output); labels and directives pass through. double emits each added
    instruction that can run twice to the same effect twice: code that does
    the same work, with twice the rewrite's added instructions."""
    rewriter = Rewriter()
    out: List[str] = []
    for number, line in enumerate(lines):
        text = line.strip()
        if not text or text.startswith(".") or text.endswith(":") or text.startswith("#") or text.startswith("//"):
            out.append(line)
            continue
        try:
            result = rewriter.instruction(line)
        except BaseRegisterError as error:
            raise BaseRegisterError(f"line {number + 1}: {error}") from None
        for added in result:
            out.append(added)
            if double and added != line and IDEMPOTENT.match(added):
                out.append(added)
    return out, rewriter
