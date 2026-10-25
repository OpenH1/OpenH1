"""The real texture_cache_map_page_count (xbox_texture_cache.c), with halo_port_capacity.h's capacities as the AArch64
guests have them and as the desktop builds do: an Xbox map keeps the Xbox's 22 MB on the AArch64 guests, a Custom
Edition map (or an Xbox map with mods' bitmaps) takes a 32nd of the computer's memory from 64 MB to the whole 256 MB,
and the desktop builds take the whole for every map."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run  # noqa: E402

CASES = ["xbox-map", "custom-edition-map", "modded-xbox-map"]
BUILDS = {"aarch64": True, "desktop": False}

# a fault in texture_cache_map_page_count, the builds whose capacities show it, and the case that must catch it
NEGATIVE_CONTROLS = {
    "xbox-map-takes-more": (("if (!custom_edition && !mod_bitmaps)", "if (0)"), "aarch64", "xbox-map"),
    "mods-ignored": (("if (!custom_edition && !mod_bitmaps)", "if (!custom_edition)"), "aarch64",
                     "modded-xbox-map"),
    "no-least": (("page_count = HALO_PORT_TEXTURE_CACHE_LEAST_PAGE_COUNT;", ";"), "aarch64",
                 "custom-edition-map"),
    "past-the-whole": (("page_count = XBOX_TEXTURE_CACHE_PAGE_COUNT;\n\t}\n\n\treturn", ";\n\t}\n\n\treturn"),
                       "aarch64", "custom-edition-map"),
    "xbox-size-everywhere": (("return HALO_PORT_TEXTURE_CACHE_XBOX_MAP_PAGE_COUNT;", "return 0x580;"), "desktop",
                             "xbox-map"),
}


def capacities(aarch64):
    """halo_port_capacity.h's textures, its test of the AArch64 guests answered"""
    header = read("port/linux/include/halo_port_capacity.h")
    section = header[header.index("/* ---------- textures"):header.index("/* ---------- sounds")]
    condition = "#if defined(HALO_GUEST) && defined(__aarch64__)\n"
    if section.count(condition) != 1:
        raise LookupError("halo_port_capacity.h's textures no longer test the AArch64 guests so")
    section = section.replace(condition, f"#if {int(aarch64)}\n")
    return f"#define AARCH64_GUEST {int(aarch64)}\n{section}\n"


def generated(build_name, fault=None):
    cache = read("source/cache/xbox_texture_cache.c")
    code = function(cache, "texture_cache_map_page_count")
    if fault:
        code = mutated(code, *fault)
    constants = enum_with(cache, "XBOX_TEXTURE_CACHE_PAGE_COUNT =")
    if not re.search(r"XBOX_TEXTURE_CACHE_PAGE_SIZE\b", constants):
        raise LookupError("the cache's page size is no longer in its enum")
    return (("config.inc", capacities(BUILDS[build_name])), ("under_test.inc", constants + "\n" + code + "\n"))


@pytest.mark.parametrize("build_name", BUILDS)
@pytest.mark.parametrize("case", CASES)
def test_case(build_name, case):
    status, output = run(build("texture_cache_size", generated(build_name)), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, build_name, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("texture_cache_size", generated(build_name, fault)), case)
    assert status == CHECK_FAILED, \
        f"texture_cache_map_page_count with a fault ({control}) passed '{case}' on the {build_name} builds"
