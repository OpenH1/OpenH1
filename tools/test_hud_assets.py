"""The high-res HUD's committed assets (port/assets/hud, port/assets/buttons):
what tools/hud_assets.py and button_assets.py wrote, checked without the
game's data.

    python -m pytest -q tools/test_hud_assets.py
"""

import json
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image
from scipy import ndimage

ROOT = Path(__file__).resolve().parent.parent
HUD = ROOT / "port/assets/hud"
BUTTONS = ROOT / "port/assets/buttons"
sys.path.insert(0, str(ROOT / "tools"))

# (port/linux/game/hud_hires_tags.c: MAXIMUM_HIRES_SPRITE_BITMAPS; hud_hires.h:
# a bit of 32 for each sequence)
MAXIMUM_SPRITE_BITMAPS = 8


def entries(listing: Path) -> list:
    return json.loads(listing.read_text())["assets"]


def holes(alpha: np.ndarray) -> int:
    """The transparent texels a shape encloses (a letter cut out of an
    oval)."""
    covered = alpha > 127
    filled = ndimage.binary_fill_holes(covered)
    return int((filled & (alpha < 32)).sum())


def test_every_cell_has_its_source():
    for entry in entries(HUD / "layout.json"):
        for cell in entry["cells"]:
            if "art" in cell:
                assert (HUD / "art" / cell["art"]).is_file(), (entry["name"], cell["art"])
            else:
                assert (HUD / "svg" / cell["svg"]).is_file(), (entry["name"], cell["svg"])
        png = Image.open(HUD / f"{entry['name']}.png")
        assert png.size == (entry["width"] * entry["scale"], entry["height"] * entry["scale"]), entry["name"]


def test_art_is_used_and_credited():
    used = {cell["art"] for entry in entries(HUD / "layout.json") for cell in entry["cells"] if "art" in cell}
    from hud_assets import BUTTON_ART

    on_disk = {path.name for path in (HUD / "art").glob("*.png")}
    assert on_disk == used | set(BUTTON_ART)
    notice = (HUD / "NOTICE.md").read_text()
    assert "Aerocatia" in notice and "Jessie" in notice and "r220" in notice
    assert "Halo PC Restored" in (ROOT / "AUTHORS.md").read_text()


def test_restored_art_is_the_hud_and_redraws_only_where_it_has_nothing_larger():
    redrawn = {entry["name"] for entry in entries(HUD / "layout.json")
               if all("svg" in cell for cell in entry["cells"])}
    assert redrawn == {"hud_health_base__0", "hud_sensor_blip__0", "hud_sensor_blip_custom__0", "hud_sweeper__0",
                       "hud_sweeper_mask__0", "vehicle_wrench_outline__0", "passenger_health_outline__0"}
    notice = (HUD / "NOTICE.md").read_text()
    for name in redrawn:
        assert f"{name}.png" in notice, name


def test_sprite_sheets_fit_the_game():
    sheets = [entry for listing in (HUD / "layout.json", BUTTONS / "buttons.json")
              for entry in entries(listing) if entry.get("sprites")]
    assert len(sheets) <= MAXIMUM_SPRITE_BITMAPS
    for entry in sheets:
        assert all(0 <= sprite["sequence"] < 32 for sprite in entry["sprites"]), entry["name"]
    # (one texture for a bitmap)
    stood_for = [(entry["tag"], entry["bitmap"]) for listing in (HUD / "layout.json", BUTTONS / "buttons.json")
                 for entry in entries(listing) if entry.get("crc")]
    assert len(stood_for) == len(set(stood_for))


@pytest.mark.parametrize("button", ["a", "b", "x", "y"])
def test_classic_buttons_have_their_letters_cut_out(button):
    """#755: the classic faces' letters are transparent, as the Xbox's
    stencils are, not black."""
    image = np.asarray(Image.open(BUTTONS / f"classic_{button}__0.png").convert("RGBA"))
    assert holes(image[..., 3]) > 500
    # (no black letter drawn opaque on the face)
    opaque = image[image[..., 3] > 200, :3]
    assert (opaque.max(axis=1) > 60).all()


def test_holes_finds_a_letter_and_not_a_plain_oval():
    """The negative control: a filled oval has no holes, one with a letter
    cut out has."""
    ys, xs = np.mgrid[0:200, 0:200]
    oval = ((((xs - 100) / 80.0) ** 2 + ((ys - 100) / 95.0) ** 2) <= 1) * 255
    assert holes(oval.astype(np.uint8)) == 0
    cut = oval.copy()
    cut[70:130, 90:110] = 0
    assert holes(cut.astype(np.uint8)) > 500
