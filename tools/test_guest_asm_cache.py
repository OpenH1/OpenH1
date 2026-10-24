"""The arm64_32 guests' object cache (tools/guest_asm_cache.py): a hit gives
the object the conversion and assembly made, byte for byte, without running
either; a different input, flag or assembler misses. (The assembler here is
a stand-in that copies the ELF assembly it is given.)"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tools" / "guest_asm_cache.py"

DARWIN = """\t.section\t__TEXT,__text,regular,pure_instructions
\t.globl\t_f
\t.p2align\t2
_f:
\tmov\tw0, #1
\tret
.subsections_via_symbols
"""

# (counts its runs in runs.txt beside the object)
ASSEMBLER = """import sys
from pathlib import Path
source, out = Path(sys.argv[1]), Path(sys.argv[2])
out.write_bytes(b"OBJ" + source.read_bytes())
with open(out.parent / "runs.txt", "a") as f:
    f.write("run\\n")
"""


def build(tmp_path, darwin=DARWIN, flags=()):
    work = tmp_path / "work"
    work.mkdir(exist_ok=True)
    (work / "as.py").write_text(ASSEMBLER)
    darwin_s, elf_s, obj = work / "f.o.darwin.s", work / "f.o.s", work / "f.o"
    darwin_s.write_text(darwin)
    for stale in (elf_s, obj):
        stale.unlink(missing_ok=True)
    subprocess.run([sys.executable, SCRIPT, tmp_path / "cache", *flags, darwin_s, elf_s, obj, "--",
                    sys.executable, work / "as.py", elf_s, obj], check=True)
    runs = (work / "runs.txt").read_text().count("run") if (work / "runs.txt").exists() else 0
    return obj.read_bytes(), runs


def test_a_hit_is_the_same_object_without_converting_or_assembling(tmp_path):
    first, runs = build(tmp_path)
    # (converted: the Darwin-only directive dropped)
    assert runs == 1 and first.startswith(b"OBJ") and b".subsections_via_symbols" not in first
    again, runs = build(tmp_path)
    assert again == first and runs == 1
    assert not (tmp_path / "work" / "f.o.s").exists()


def test_other_input_or_flags_miss(tmp_path):
    first, _ = build(tmp_path)
    other, runs = build(tmp_path, DARWIN.replace("#1", "#2"))
    assert other != first and runs == 2
    _, runs = build(tmp_path, flags=["--base-register"])
    assert runs == 3
    _, runs = build(tmp_path)
    assert runs == 3


def test_a_converter_change_misses(tmp_path):
    tools = tmp_path / "tools"
    tools.mkdir()
    for name in ("guest_asm_cache.py", "android_asm_convert.py", "guest_base_register.py"):
        shutil.copy(ROOT / "tools" / name, tools / name)
    global SCRIPT
    saved, SCRIPT = SCRIPT, tools / "guest_asm_cache.py"
    try:
        _, runs = build(tmp_path)
        _, runs = build(tmp_path)
        assert runs == 1
        with open(tools / "android_asm_convert.py", "a") as f:
            f.write("\n# changed\n")
        _, runs = build(tmp_path)
        assert runs == 2
    finally:
        SCRIPT = saved
