"""A standalone icon render of scene.blend (made by build_scene.py): the panel
alone, straight on, on a transparent background
with only a soft contact shadow, lit by a photographed hangar (Poly Haven's
hangar_interior, CC0: CREDITS.md) that the camera never sees, plus a key.

    blender -b website/logo/render/scene.blend -P website/logo/render/render_icon.py -- [--res N] [--samples N] [--device AUTO] [--out FILE]
"""

import argparse
import math
import sys
import urllib.request
from pathlib import Path

import bpy
from mathutils import Vector

HERE = Path(__file__).resolve().parent
HDRI_URL = "https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/hangar_interior_2k.hdr"
YAW_DEG = 0.0          # straight on
LEAN_DEG = 0.0         # upright
HDRI_STRENGTH = 0.35
HDRI_ROTATION_DEG = 140.0


def options():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser()
    p.add_argument("--res", type=int, default=2048)
    p.add_argument("--samples", type=int, default=256)
    p.add_argument("--device", default="AUTO")
    p.add_argument("--out", default=str(HERE / "icon-standalone.png"))
    return p.parse_args(argv)


def fetch(url):
    path = HERE / "textures" / url.rsplit("/", 1)[1]
    if not path.exists():
        path.parent.mkdir(exist_ok=True)
        urllib.request.urlretrieve(url, path)
    return path


def main():
    opt = options()
    scene = bpy.context.scene
    # (render.py's device choice)
    sys.path.insert(0, str(HERE))
    import render as shots
    shots.use_device(scene, opt.device)

    for c in bpy.data.collections:
        if c.name.startswith("Rig_") or c.name == "Studio":
            c.hide_render = True

    # the panel: turned and leaning back, standing on z = 0
    panel = bpy.data.objects["Panel"]
    panel.rotation_euler = (math.radians(90.0 - LEAN_DEG), 0.0, math.radians(YAW_DEG))
    bpy.context.view_layer.update()
    parts = [panel] + list(panel.children_recursive)
    dg = bpy.context.evaluated_depsgraph_get()
    pts = [o.matrix_world @ v.co for o in parts if o.type == "MESH" for v in o.evaluated_get(dg).data.vertices]
    zmin = min(p.z for p in pts)
    panel.location.z -= zmin
    bpy.context.view_layer.update()
    pts = [o.matrix_world @ v.co for o in parts if o.type == "MESH" for v in o.evaluated_get(dg).data.vertices]
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    centre = (lo + hi) / 2

    # a shadow catcher under it: only the contact shadow shows
    bpy.ops.mesh.primitive_plane_add(size=4.0, location=(0, 0, 0))
    floor = bpy.context.active_object
    floor.is_shadow_catcher = True

    # the hangar for light and reflections, unseen by the camera
    w = bpy.data.worlds.new("Hangar")
    w.use_nodes = True
    nt = w.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputWorld")
    bg = nt.nodes.new("ShaderNodeBackground")
    env = nt.nodes.new("ShaderNodeTexEnvironment")
    env.image = bpy.data.images.load(str(fetch(HDRI_URL)))
    mapping = nt.nodes.new("ShaderNodeMapping")
    mapping.inputs["Rotation"].default_value = (0, 0, math.radians(HDRI_ROTATION_DEG))
    tc = nt.nodes.new("ShaderNodeTexCoord")
    nt.links.new(tc.outputs["Generated"], mapping.inputs["Vector"])
    nt.links.new(mapping.outputs["Vector"], env.inputs["Vector"])
    nt.links.new(env.outputs["Color"], bg.inputs["Color"])
    bg.inputs["Strength"].default_value = HDRI_STRENGTH
    nt.links.new(bg.outputs[0], out.inputs[0])
    scene.world = w

    # a soft key from the upper left, grazing the face
    ld = bpy.data.lights.new("IconKey", "AREA")
    ld.energy = 45.0
    ld.size = 0.6
    ld.color = (1.0, 0.96, 0.9)
    key = bpy.data.objects.new("IconKey", ld)
    scene.collection.objects.link(key)
    key.location = centre + Vector((-0.9, -0.55, 0.85))
    key.rotation_euler = (centre - key.location).to_track_quat("-Z", "Y").to_euler()

    # the camera: square, a little above, framing the panel with a margin
    cd = bpy.data.cameras.new("IconCam")
    cd.lens = 100.0
    cam = bpy.data.objects.new("IconCam", cd)
    scene.collection.objects.link(cam)
    cam.location = centre + Vector((0.0, -2.2, 0.0))
    cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
    span = max(hi.x - lo.x, hi.z - lo.z) * 1.42
    cd.sensor_fit = "HORIZONTAL"
    cd.lens = cd.sensor_width * (centre - cam.location).length / span
    scene.camera = cam

    scene.render.resolution_x = scene.render.resolution_y = opt.res
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = True
    scene.cycles.samples = opt.samples
    scene.view_settings.exposure = -0.4
    scene.render.filepath = opt.out
    print(f"rendering the icon {opt.res}x{opt.res} at {opt.samples} samples -> {opt.out}", flush=True)
    bpy.ops.render.render(write_still=True)


main()
