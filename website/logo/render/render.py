"""Renders the shots of scene.blend (made by build_scene.py) into this folder:

    blender -b website/logo/render/scene.blend -P website/logo/render/render.py -- [options]

Options (defaults are the settings just below):
    --shots hero,front,icon,detail   which shots
    --device AUTO|OPTIX|CUDA|HIP|METAL|ONEAPI|CPU
                                     AUTO tries OptiX, CUDA, HIP, Metal, oneAPI, then the CPU
    --samples N                      samples per pixel for every shot (default: per shot)
    --res N                          long edge in pixels for every shot (default: per shot)
    --scale F                        a fraction of the size, for quick previews
    --denoiser OIDN|OPTIX|NONE
    --no-dof                         turn off the depth of field on the detail shot
    --out DIR                        where to write (default: this folder)

Shots:
    hero.png      2400 x 1600  head-on on a dark sweep, soft key, rims, backdrop glow
    front.png     2048 x 2048  straight on, evenly lit, transparent background
    icon-1024.png 1024 x 1024  as front, tighter: the panel fills about 85 %
    detail.png    2400 x 1600  near-frontal macro of a fastener and worn paint
"""

import argparse
import sys
from pathlib import Path

import bpy
from mathutils import Vector

HERE = Path(__file__).resolve().parent

# ----------------------------------------------------------------- settings
DEVICE = "AUTO"
DENOISER = "OIDN"
SHOTS = {
    #          resolution     samples transparent studio exposure f-stop (None: no DOF)
    "hero":   ((2400, 1600), 256, False, True, -0.6, None),
    "front":  ((2048, 2048), 192, True, False, -0.9, None),
    "icon":   ((1024, 1024), 256, True, False, -0.9, None),
    "detail": ((2400, 1600), 256, False, True, -0.7, 8.0),
}
FILES = {"hero": "hero.png", "front": "front.png", "icon": "icon-1024.png", "detail": "detail.png"}
RIGS = {"hero": "Rig_hero", "front": "Rig_front", "icon": "Rig_front", "detail": "Rig_detail"}
# ---------------------------------------------------------------------------


def args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser()
    p.add_argument("--shots", default=",".join(SHOTS))
    p.add_argument("--device", default=DEVICE)
    p.add_argument("--samples", type=int)
    p.add_argument("--res", type=int)
    p.add_argument("--scale", type=float, default=1.0)
    p.add_argument("--denoiser", default=DENOISER)
    p.add_argument("--no-dof", dest="dof", action="store_false", default=True)
    p.add_argument("--out", default=str(HERE))
    return p.parse_args(argv)


def use_device(scene, wanted):
    prefs = bpy.context.preferences.addons["cycles"].preferences
    order = ["OPTIX", "CUDA", "HIP", "METAL", "ONEAPI"] if wanted.upper() == "AUTO" else [wanted.upper()]
    for kind in order:
        if kind == "CPU":
            break
        try:
            prefs.compute_device_type = kind
        except TypeError:
            continue
        prefs.get_devices()
        gpus = [d for d in prefs.devices if d.type == kind]
        if gpus:
            for d in prefs.devices:
                d.use = d.type == kind
            scene.cycles.device = "GPU"
            print(f"rendering with {kind} on", ", ".join(d.name for d in gpus), flush=True)
            return kind
    scene.cycles.device = "CPU"
    print("rendering on the CPU", flush=True)
    return "CPU"


def render(shot, opt):
    scene = bpy.context.scene
    (w, h), samples, transparent, studio, exposure, fstop = SHOTS[shot]
    if opt.res:
        k = opt.res / max(w, h)
        w, h = round(w * k), round(h * k)
    for c in bpy.data.collections:
        if c.name.startswith("Rig_"):
            c.hide_render = c.name not in (RIGS[shot], f"Rig_{shot}")
    bpy.data.collections["Studio"].hide_render = not studio
    cam = bpy.data.objects[f"Cam_{shot}"]
    scene.camera = cam
    scene.render.resolution_x, scene.render.resolution_y = w, h
    scene.render.resolution_percentage = max(1, round(opt.scale * 100))
    scene.render.film_transparent = transparent
    scene.cycles.samples = opt.samples or samples
    scene.view_settings.exposure = exposure
    dof = cam.data.dof
    if fstop and opt.dof:
        dof.use_dof = True
        dof.aperture_fstop = fstop
        focus = bpy.data.objects.get(f"Focus_{shot}")
        if focus:
            dof.focus_object = focus
        else:
            centre = Vector(scene.get("panel_centre"))
            fwd = cam.matrix_world.to_3x3() @ Vector((0, 0, -1))
            dof.focus_distance = (centre - cam.matrix_world.translation).dot(fwd)
    else:
        dof.use_dof = False
    out = Path(opt.out) / FILES[shot]
    scene.render.filepath = str(out)
    print(f"rendering {shot} {w}x{h} at {scene.cycles.samples} samples -> {out}", flush=True)
    bpy.ops.render.render(write_still=True)


def main():
    opt = args()
    scene = bpy.context.scene
    kind = use_device(scene, opt.device)
    den = opt.denoiser.upper()
    scene.cycles.use_denoising = den != "NONE"
    if den == "OPTIX" and kind != "OPTIX":
        den = "OIDN"
    if scene.cycles.use_denoising:
        scene.cycles.denoiser = "OPTIX" if den == "OPTIX" else "OPENIMAGEDENOISE"
        try:
            scene.cycles.denoising_use_gpu = kind != "CPU"
        except AttributeError:
            pass
    for shot in opt.shots.split(","):
        render(shot.strip(), opt)


if __name__ == "__main__":
    main()
