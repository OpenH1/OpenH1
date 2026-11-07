"""tools/capture.py without the game: its arguments, its scenes, and its pictures (the BMP files the game
writes read, made PNG files, cropped, put side by side, a Retina window's made the size asked for)."""
import struct
import sys
import zlib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture  # noqa: E402


def parse(*argv):
    parser = capture.make_parser()
    arguments = parser.parse_args(list(argv))
    capture.check_arguments(parser, arguments)
    return arguments


def test_arguments():
    arguments = parse("--scene", "sniper-zoom-10x", "--crop", "1,2,30,40", "--size", "1280x720",
                      "--set", "display.fov=100", "--frames", "4", "--every", "2")
    assert arguments.crop == (1, 2, 30, 40)
    assert arguments.size == (1280, 720)
    assert arguments.set == [("display.fov", "100")]
    assert (arguments.frames, arguments.every) == (4, 2)


@pytest.mark.parametrize("argv", [
    ["--scene", "no-such-scene"],
    [],
    ["--scene", "spawn", "--crop", "1,2,3"],
    ["--scene", "spawn", "--crop", "1,2,0,4"],
    ["--scene", "spawn", "--size", "1920"],
    ["--scene", "spawn", "--set", "display.no_such_setting=1"],
    ["--scene", "spawn", "--set", "novalue"],
    ["--scene", "spawn", "--frames", "0"],
    ["--scene", "camera:1 2 3"],
    ["--scene", "campaign:"],
])
def test_bad_arguments(argv):
    with pytest.raises(SystemExit):
        parse(*argv)


def test_every_setting_has_a_variable():
    variables = capture.setting_variables()
    assert variables["display.fov"].startswith("HALO_")
    assert variables["debug.network_test_vehicle_seat"] == "HALO_NETWORK_TEST_VEHICLE_SEAT"
    # (every variable the scenes set is a setting's)
    known = set(variables.values())
    for scene in capture.SCENES.values():
        assert set(scene.environment) <= known, scene.description


def test_scenes():
    for name, scene in capture.SCENES.items():
        assert scene.at > 0 and scene.description, name
        if scene.script:
            for event in scene.script.split(";"):
                start = event.split()[0]
                assert float(start.split("-")[-1]) < scene.at + 5, (name, event)
    campaign = capture.scene_for("campaign:a10")
    assert (campaign.map, campaign.variant) == ("levels\\a10\\a10", "coop")
    assert capture.scene_for("campaign:levels/b30/b30").map == "levels\\b30\\b30"
    camera = capture.scene_for("camera:10 -20.5 3 90 -5", "hangemhigh")
    assert camera.map == "hangemhigh"
    assert camera.environment["HALO_NETWORK_TEST_CUES"] == '1 debug_camera_set "10 -20.5 3 90 -5 0 70"'
    assert capture.scene_for("spawn", "damnation").map == "damnation"
    assert capture.SCENES["spawn"].map == "bloodgulch"


def test_file_names():
    assert capture.scene_file_name("sniper-zoom-10x") == "sniper-zoom-10x"
    assert capture.scene_file_name("campaign:a10") == "campaign-a10"
    assert capture.scene_file_name("camera:1 2 3 4 5") == "camera"


def bmp(width, height, pixel, top_down=True):
    body = b"".join(bytes(pixel(x, y if top_down else height - 1 - y)) + b"\xff"
                    for y in range(height) for x in range(width))
    header = bytearray(54)
    header[:2] = b"BM"
    struct.pack_into("<IIIIiiHH", header, 2, 54 + len(body), 0, 54, 40, width, -height if top_down else height, 1, 32)
    return bytes(header) + body


def read_png(path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    width, height = struct.unpack(">II", data[16:24])
    start = data.index(b"IDAT") + 4
    length = struct.unpack(">I", data[start - 8:start - 4])[0]
    raw = zlib.decompress(data[start:start + length])
    rows = [raw[y * (width * 3 + 1) + 1:(y + 1) * (width * 3 + 1)] for y in range(height)]
    return width, height, rows


@pytest.mark.parametrize("top_down", [True, False])
def test_bmp_to_png(tmp_path, top_down):
    # (BGRA: blue is x, green is y)
    (tmp_path / "frame.bmp").write_bytes(bmp(4, 3, lambda x, y: (x * 10, y * 20, 200), top_down))
    assert capture.bmp_complete(tmp_path / "frame.bmp")
    picture = capture.read_bmp(tmp_path / "frame.bmp")
    capture.write_png(picture, tmp_path / "frame.png")
    width, height, rows = read_png(tmp_path / "frame.png")
    assert (width, height) == (4, 3)
    assert rows[2][3 * 3:3 * 3 + 3] == bytes((200, 40, 30))
    crop = picture.crop(1, 1, 2, 2)
    assert (crop.width, crop.height) == (2, 2) and crop.rows[0][:3] == bytes((200, 20, 10))
    with pytest.raises(ValueError):
        picture.crop(10, 10, 2, 2)
    pair = capture.side_by_side(picture, crop, gap=1)
    assert (pair.width, pair.height) == (7, 3) and len(pair.rows[2]) == 21


def test_bmp_being_written(tmp_path):
    data = bmp(4, 3, lambda x, y: (0, 0, 0))
    (tmp_path / "frame.bmp").write_bytes(data[:-5])
    assert not capture.bmp_complete(tmp_path / "frame.bmp")


def test_fit():
    picture = capture.Picture(4, 2, [bytes([0, 0, 0, 100, 100, 100] * 2), bytes([100, 100, 100, 100, 100, 100] * 2)])
    small = capture.fit(picture, (2, 1))
    assert (small.width, small.height) == (2, 1) and small.rows[0] == bytes([75] * 6)
    assert capture.fit(picture, (3, 1)) is picture
