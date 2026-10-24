"""Tests of the guest's base-register rewrite (tools/guest_base_register.py)
and of the import stubs' pointer translation (tools/android_imports.py)."""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from guest_base_register import BaseRegisterError, rewrite  # noqa: E402


def rewritten(*lines):
    return [line.strip() for line in rewrite([f"\t{line}" for line in lines])[0]]


@pytest.mark.parametrize("source, expected", [
    # plain loads and stores: the register-offset form, no extra instruction
    ("ldr w0, [x1]", ["ldr w0, [x21, w1, uxtw]"]),
    ("strb wzr, [x8]", ["strb wzr, [x21, w8, uxtw]"]),
    ("ldr q0, [x3]", ["ldr q0, [x21, w3, uxtw]"]),
    # an immediate offset: added in 32 bits, then the guarded form
    ("ldr w0, [x1, #8]", ["add w22, w1, #8", "ldr w0, [x21, w22, uxtw]"]),
    ("ldr x0, [x1, #16384]", ["add w22, w1, #16384", "ldr x0, [x21, w22, uxtw]"]),
    ("ldr x0, [x1, #4104]", ["add x22, x21, w1, uxtw", "ldr x0, [x22, #4104]"]),
    # (the forms without the guarded addressing mode: the base added first)
    ("ldp x0, x1, [x2, #16]", ["add x22, x21, w2, uxtw", "ldp x0, x1, [x22, #16]"]),
    ("ldur s0, [x9, #-4]", ["sub w22, w9, #4", "ldr s0, [x21, w22, uxtw]"]),
    ("sturh w8, [x0, #3]", ["add w22, w0, #3", "strh w8, [x21, w22, uxtw]"]),
    # a register offset: the address computed, then added as a 32-bit offset
    ("ldr w0, [x1, x2, lsl #2]", ["add w22, w1, w2, lsl #2", "ldr w0, [x21, w22, uxtw]"]),
    ("ldrb w0, [x1, w2, sxtw]", ["add w22, w1, w2, sxtw", "ldrb w0, [x21, w22, uxtw]"]),
    ("ldrb w0, [x9, x8]", ["add w22, w9, w8", "ldrb w0, [x21, w22, uxtw]"]),
    ("ldr s0, [x8, x9, lsl #2]", ["add w22, w8, w9, lsl #2", "ldr s0, [x21, w22, uxtw]"]),
    ("str w19, [x8, wzr, uxtw #2]", ["str w19, [x21, w8, uxtw]"]),
    # writeback
    ("ldr x0, [x1], #8", ["ldr x0, [x21, w1, uxtw]", "add x1, x1, #8"]),
    ("str w0, [x1, #-4]!", ["sub x1, x1, #4", "str w0, [x21, w1, uxtw]"]),
    ("ldp w0, w1, [x2], #8", ["add x22, x21, w2, uxtw", "ldp w0, w1, [x22]", "add x2, x2, #8"]),
    ("ld1 { v0.4s }, [x1], #16", ["add x22, x21, w1, uxtw", "ld1 { v0.4s }, [x22]", "add x1, x1, #16"]),
    ("ld1 { v0.s }[1], [x1], x2", ["add x22, x21, w1, uxtw", "ld1 { v0.s }[1], [x22]", "add x1, x1, x2"]),
    # exclusives and acquire/release: through the scratch register
    ("ldaxr w8, [x0]", ["add x22, x21, w0, uxtw", "ldaxr w8, [x22]"]),
    ("stlxr w9, w1, [x0]", ["add x22, x21, w0, uxtw", "stlxr w9, w1, [x22]"]),
    # the stack, the frame pointer and globals (through adrp) are left alone
    ("ldr w0, [sp, #12]", ["ldr w0, [sp, #12]"]),
    ("stp x29, x30, [sp, #-16]!", ["stp x29, x30, [sp, #-16]!"]),
    ("ldur w8, [x29, #-4]", ["ldur w8, [x29, #-4]"]),
    ("ldr w8, [x8, :lo12:table]", ["ldr w8, [x8, :lo12:table]"]),
    ("ldr q0, [x8, :lo12:.LCPI0_0]", ["ldr q0, [x8, :lo12:.LCPI0_0]"]),
    # literals are PC-relative
    ("ldr q0, .LCPI0_0", ["ldr q0, .LCPI0_0"]),
    # addresses taken from sp, x29 or a page: their 32-bit offsets
    ("add x0, sp, #16", ["add w0, wsp, #16"]),
    ("mov x8, sp", ["mov w8, wsp"]),
    ("sub x8, x29, #32", ["sub w8, w29, #32"]),
    ("add x8, x8, :lo12:table", ["add w8, w8, :lo12:table"]),
    ("add x30, sp, #8", ["add w30, wsp, #8"]),
    ("adr x9, .Ltmp0", ["adr x9, .Ltmp0", "mov w9, w9"]),
    ("adrp x8, table", ["adrp x8, table"]),
    # sp and x29 set from another register are rebased
    ("mov sp, x8", ["mov x22, x8", "add sp, x21, w22, uxtw"]),
    ("sub sp, x9, #16", ["sub x22, x9, #16", "add sp, x21, w22, uxtw"]),
    ("mov x29, x0", ["mov x29, x0", "add x29, x21, w29, uxtw"]),
    # from sp or x29 they stay full addresses
    ("sub sp, sp, #32", ["sub sp, sp, #32"]),
    ("mov x29, sp", ["mov x29, sp"]),
    ("add sp, x29, #16", ["add sp, x29, #16"]),
    ("sub sp, sp, x8", ["sub sp, sp, x8"]),
    # indirect branches
    ("blr x8", ["add x22, x21, w8, uxtw", "blr x22"]),
    ("br x10", ["add x22, x21, w10, uxtw", "br x22"]),
    ("ret", ["ret"]),
    # x30 as an ordinary register, between its save and its reload
    ("mov x30, x5", ["mov x30, x5"]),
    ("ldr w30, [x8]", ["ldr w30, [x21, w8, uxtw]"]),
    # 32-bit comparisons of offsets
    ("tst w29, #0x3", ["tst w29, #0x3"]),
    ("cmp w8, w9", ["cmp w8, w9"]),
])
def test_rewrite(source, expected):
    assert rewritten(source) == expected


@pytest.mark.parametrize("source", [
    "mov x21, x0",          # the base register written
    "ldp x20, x21, [sp]",   # loaded into
    "add x0, x22, #1",      # the scratch register used
    "cmp sp, x9",           # a full address against an offset
    "mov w29, w0",          # the frame pointer cut to 32 bits
    "ldr w0, [x1, x2]!",    # an addressing form that does not exist
])
def test_rewrite_refuses(source):
    with pytest.raises(BaseRegisterError):
        rewrite([f"\t{source}"])


def test_labels_and_directives_pass_through():
    lines = [".text", "main:", "\t.p2align 2", ".LBB0_1:", "\t.long sym"]
    assert rewrite(lines)[0] == lines


def test_reading_the_base_is_allowed():
    # (the guest's C reads x21 for the host's pointer arguments)
    assert rewritten("mov x8, x21") == ["mov x8, x21"]


def clang_with_arm64_32():
    clang = shutil.which("clang")
    if not clang:
        return None
    probe = subprocess.run([clang, "--target=arm64_32-apple-watchos", "-x", "c", "-S", "-o", "-", "-"],
                           input="int f(int *p) { return *p; }", capture_output=True, text=True)
    return clang if probe.returncode == 0 else None


def test_compiled_code(tmp_path):
    """A function compiled as the guest is, with the reserved registers,
    converts and rewrites without errors, and only the base and scratch
    registers' instructions are new."""
    clang = clang_with_arm64_32()
    if not clang:
        pytest.skip("no clang with the arm64_32 target")
    source = tmp_path / "sample.c"
    source.write_text("""
struct node { struct node *next; int value; float weights[4]; };
extern void (*callbacks[4])(struct node *);
int walk(struct node *node, int index)
{
	int total = 0;
	char buffer[64];
	while (node) {
		total += node->value + buffer[index & 63];
		node->weights[index & 3] *= 2.0f;
		callbacks[index & 3](node);
		node = node->next;
	}
	switch (index) { case 0: return total; case 1: return total * 3; case 2: return total - 7;
	case 3: return total ^ 5; case 4: return total + 9; case 5: return -total; }
	return 0;
}
""")
    darwin = tmp_path / "sample.darwin.s"
    subprocess.run([clang, "--target=arm64_32-apple-watchos", "-O2", "-ffixed-x21", "-ffixed-x22",
                    "-fno-omit-frame-pointer", "-fno-stack-protector", "-mllvm",
                    "-aarch64-enable-compress-jump-tables=false", "-S", str(source), "-o", str(darwin)],
                   check=True, capture_output=True)
    output = tmp_path / "sample.s"
    subprocess.run([sys.executable, str(ROOT / "tools" / "android_asm_convert.py"), "--base-register",
                    str(darwin), str(output)], check=True)
    text = output.read_text()
    assert "[x21, w" in text and "w22, w" in text
    assert "blr\tx22" in text or "blr x22" in text


def test_import_stubs_translate_pointers(tmp_path):
    header = tmp_path / "imports.h"
    header.write_text("void host_copy(char *buffer, unsigned int size, const char *name);\n"
                      "int host_scale(float factor, int *result) __attribute__((noreturn));\n")
    names = tmp_path / "imports.list"
    names.write_text("host_copy\nhost_scale\nhostposix_stat\n")
    output = tmp_path / "imports.s"
    subprocess.run([sys.executable, str(ROOT / "tools" / "android_imports.py"), "--arch", "arm64_32",
                    "--base-register", "--prototypes", str(header), str(output), str(names)], check=True)
    text = output.read_text()
    copy = text.split("host_copy:")[1].split("br x16")[0]
    assert "add x0, x21, w0, uxtw" in copy and "add x2, x21, w2, uxtw" in copy and "w1" not in copy
    # (a float takes no integer register)
    scale = text.split("host_scale:")[1].split("br x16")[0]
    assert "add x0, x21, w0, uxtw" in scale and "w1" not in scale
    # (the posix functions' wrappers translate their own)
    assert "x21" not in text.split("hostposix_stat:")[1].split("br x16")[0]
    # without the base register, the stubs are as before
    plain = tmp_path / "plain.s"
    subprocess.run([sys.executable, str(ROOT / "tools" / "android_imports.py"), "--arch", "arm64_32", str(plain),
                    str(names)], check=True)
    assert "x21" not in plain.read_text()
    # (x86-64's guest has no base register: tools/guest_arch.py)
    refused = subprocess.run([sys.executable, str(ROOT / "tools" / "android_imports.py"), "--arch", "x32",
                              "--base-register", "--prototypes", str(header), str(output), str(names)],
                             capture_output=True, text=True)
    assert refused.returncode != 0 and "no base register" in refused.stderr


def test_posix_wrappers_keep_directory_handles(tmp_path):
    """the posix wrappers make guest pointers host addresses, but not the
    directory handles posix_directory_open returns (1 to 64 on a 64-bit
    host): made addresses, every directory listed empty on macOS"""
    output = tmp_path / "posix.c"
    names = tmp_path / "posix.list"
    subprocess.run([sys.executable, str(ROOT / "tools" / "android_posix_stubs.py"), "--base-register",
                    str(ROOT / "port" / "linux" / "src" / "posix.h"), str(output), str(names)], check=True)
    text = output.read_text()
    for name in ("posix_directory_next", "posix_directory_close"):
        body = text.split(f" {name}(")[1].split("}")[0]
        assert "host_address(directory)" not in body and "(unsigned int)directory" in body, body
    # (its name is still a guest pointer)
    assert "host_address(name)" in text.split(" posix_directory_next(")[1].split("}")[0]
    assert "host_address(path)" in text.split(" posix_directory_open(")[1].split("}")[0]
