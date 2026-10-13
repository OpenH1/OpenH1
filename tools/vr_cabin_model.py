#!/usr/bin/env python3
"""Makes the VR mode's cabins: what a vehicle's model lacks for the view from
a seat (port/linux/game/vr_cabin.c), as tables of vertices and triangles:

    python3 tools/vr_cabin_model.py                 # port/linux/game/vr_cabin_model.h
    python3 tools/vr_cabin_model.py --obj out/c     # also out/c_<cabin>.obj and .json, to look at

Each cabin is for a seat of a vehicle's model (its tag name, the seat's
label) and, where the vehicle has a setting for it, one of its views: where
the eye is (in the model's space, world units: forward, left, up), and its
meshes, each drawn from a node of the model with one of the model's own
shaders (its textures, its lighting), so that it reads as the same vehicle:

- the Scorpion's driver, inside (vr.scorpion_view = "inside"): the model is
  a closed hull, the driver's hatch over an empty slot in its top. The
  driver sits in a compartment at the front of the hull, under the front
  plate: walls, a ceiling with the hatch, a seat, the two steering levers,
  an instrument panel, and vision blocks in the front wall and its two
  corners, which the world is seen through (the hull's own faces about it
  face out, and are not drawn from inside).
- the Scorpion's commander, in the turret's hatch (vr.scorpion_view =
  "hatch"): the eye above the turret, on its axis, so that it does not swing
  as the turret turns; a cupola ring with its lid open, on the turret.

The faces are flat, as the models' are (each face its own vertices), and
seen from inside the cabin (their outsides toward the eye). Each samples a
part of the vehicle's texture fitting what it is (a plate, a grille, a panel
of switches, dials, a hatch), mapped across the face; the coordinates are
stored as the model stores its own, divided by its base map scale, which the
game multiplies them by as it draws them.

Python 3 alone (no packages): the output is committed, and only this script
is to be edited to change a cabin.
"""

import argparse
import json
import math
import os


def sub(a, b):
	return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b):
	return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def scale(a, s):
	return (a[0] * s, a[1] * s, a[2] * s)


def dot(a, b):
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
	return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normalize(a):
	length = math.sqrt(dot(a, a))
	return scale(a, 1.0 / length) if length > 1e-12 else (0.0, 0.0, 1.0)


class Mesh:
	"""a mesh of flat faces drawn from one node with one shader"""

	def __init__(self, node, shader):
		self.node, self.shader = node, shader
		self.vertices, self.triangles = [], []

	def stored(self, base_map_scale):
		"""the texture coordinates as the model stores its own: the game scales
		them by the model's base map scale as it draws them"""
		su, sv = base_map_scale
		self.vertices = [(p, n, (uv[0] / su, uv[1] / sv)) for p, n, uv in self.vertices]

	def face(self, points, seen_from, region):
		"""a flat convex polygon, seen from the side of the point seen_from (its
		outside toward it), its texture coordinates the region (u0, v0, u1, v1)
		across it: the polygon's longer extent along the region's longer"""
		normal = normalize(cross(sub(points[1], points[0]), sub(points[2], points[0])))
		if dot(normal, sub(seen_from, points[0])) < 0.0:
			normal = scale(normal, -1.0)
		# (in the face: across along its first edge, up perpendicular to it)
		across = normalize(sub(points[1], points[0]))
		up = cross(normal, across)
		a = [dot(sub(p, points[0]), across) for p in points]
		b = [dot(sub(p, points[0]), up) for p in points]
		a0, a1, b0, b1 = min(a), max(a), min(b), max(b)
		u0, v0, u1, v1 = region
		base = len(self.vertices)
		for p, s, t in zip(points, a, b):
			fs = (s - a0) / (a1 - a0) if a1 > a0 else 0.0
			ft = (t - b0) / (b1 - b0) if b1 > b0 else 0.0
			self.vertices.append((p, normal, (u0 + (u1 - u0) * fs, v1 - (v1 - v0) * ft)))
		for k in range(1, len(points) - 1):
			t = (base, base + k, base + k + 1)
			# (wound as the model's faces are: the cross of the edges away from
			# the side seen)
			p0, p1, p2 = (self.vertices[i][0] for i in t)
			if dot(cross(sub(p1, p0), sub(p2, p0)), normal) > 0.0:
				t = (t[0], t[2], t[1])
			self.triangles.append(t)

	def box(self, lo, hi, region, faces='xXyYzZ', seen_inside=False, eye=None):
		"""an axis-aligned box's faces (x: its back, X: its front, y, Y, z, Z),
		seen from outside it (or from inside it)"""
		x0, y0, z0 = lo
		x1, y1, z1 = hi
		centre = ((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)
		quads = {
			'x': [(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)],
			'X': [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)],
			'y': [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)],
			'Y': [(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)],
			'z': [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)],
			'Z': [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)],
		}
		for f in faces:
			q = quads[f]
			if seen_inside:
				seen = centre
			else:
				fc = scale(add(add(q[0], q[1]), add(q[2], q[3])), 0.25)
				seen = add(fc, sub(fc, centre))
			self.face(q, seen, region[f] if isinstance(region, dict) else region)

	def bar(self, a, b, half, region, up=(0.0, 0.0, 1.0)):
		"""a square bar from a to b, its half width half: its four sides and
		its ends"""
		axis = normalize(sub(b, a))
		side = normalize(cross(axis, up if abs(dot(axis, up)) < 0.95 else (1.0, 0.0, 0.0)))
		top = cross(side, axis)
		corners = [add(scale(side, sx * half), scale(top, sy * half)) for sx, sy in ((1, 1), (-1, 1), (-1, -1), (1, -1))]
		middle = scale(add(a, b), 0.5)
		for k in range(4):
			c0, c1 = corners[k], corners[(k + 1) % 4]
			q = [add(a, c0), add(a, c1), add(b, c1), add(b, c0)]
			outward = scale(add(c0, c1), 0.5)
			self.face(q, add(add(middle, outward), outward), region)
		self.face([add(a, c) for c in corners], sub(a, axis), region)
		self.face([add(b, c) for c in corners], add(b, axis), region)


def wall_with_openings(mesh, origin, along, up, length, height, openings, eye, region, reveal=0.0, reveal_region=None):
	"""a flat wall seen from the eye, from origin, length along, height up, with
	rectangular openings (start, end along; bottom, top up) through it, each
	with a reveal of this depth away from the eye (its four sides)"""
	normal = normalize(cross(along, up))
	if dot(normal, sub(eye, origin)) < 0.0:
		normal = scale(normal, -1.0)
	away = scale(normal, -1.0)

	def point(s, t, d=0.0):
		return add(add(add(origin, scale(along, s)), scale(up, t)), scale(away, d))

	# the wall in strips: below and above the openings' band, and between them
	cuts_s = sorted({0.0, length} | {o[0] for o in openings} | {o[1] for o in openings})
	cuts_t = sorted({0.0, height} | {o[2] for o in openings} | {o[3] for o in openings})
	for i in range(len(cuts_s) - 1):
		for j in range(len(cuts_t) - 1):
			s0, s1, t0, t1 = cuts_s[i], cuts_s[i + 1], cuts_t[j], cuts_t[j + 1]
			sm, tm = (s0 + s1) / 2, (t0 + t1) / 2
			if any(o[0] <= sm <= o[1] and o[2] <= tm <= o[3] for o in openings):
				continue
			# (each cell's texture its share of the region, so the wall reads whole)
			u0, v0, u1, v1 = region
			cell = (u0 + (u1 - u0) * s0 / length, v0 + (v1 - v0) * (1 - t1 / height),
				u0 + (u1 - u0) * s1 / length, v0 + (v1 - v0) * (1 - t0 / height))
			mesh.face([point(s0, t0), point(s1, t0), point(s1, t1), point(s0, t1)], eye, cell)
	if reveal > 0.0:
		for s0, s1, t0, t1 in openings:
			r = reveal_region or region
			mid = point((s0 + s1) / 2, (t0 + t1) / 2, reveal / 2)
			mesh.face([point(s0, t0), point(s1, t0), point(s1, t0, reveal), point(s0, t0, reveal)], mid, r)
			mesh.face([point(s0, t1), point(s1, t1), point(s1, t1, reveal), point(s0, t1, reveal)], mid, r)
			mesh.face([point(s0, t0), point(s0, t1), point(s0, t1, reveal), point(s0, t0, reveal)], mid, r)
			mesh.face([point(s1, t0), point(s1, t1), point(s1, t1, reveal), point(s1, t0, reveal)], mid, r)


# ---------- the Scorpion (vehicles\scorpion\scorpion: scorpion_body, 512 x 512)

# the model's base map scale (the game scales its texture coordinates by it)
SCORPION_BASE_MAP_SCALE = (4.148820, 0.999792)
# the texture's parts (u0, v0, u1, v1)
SCORPION_PLATE = (0.840, 0.505, 0.935, 0.615)
SCORPION_PLATE_WIDE = (0.765, 0.515, 0.995, 0.700)
SCORPION_GRILLE = (0.245, 0.655, 0.415, 0.730)
SCORPION_SWITCHES = (0.020, 0.865, 0.160, 0.990)
SCORPION_DIALS = (0.195, 0.835, 0.310, 0.870)
SCORPION_FUSES = (0.170, 0.270, 0.345, 0.345)
SCORPION_HATCH = (0.445, 0.880, 0.660, 0.990)
SCORPION_DARK = (0.000, 0.700, 0.120, 0.760)
SCORPION_BOX = (0.000, 0.000, 0.105, 0.115)


def scorpion_inside():
	"""the driver's compartment at the front of the hull, under the front
	plate, on the driver's (left) side: inside the hull (its top at up 0.64
	to 0.665 there, its front plate from forward 0.69 down to 0.78), ahead of
	the empty slot under the hatch (to forward 0.365)"""
	eye = (0.440, 0.200, 0.520)
	x0, x1 = 0.370, 0.660
	y0, y1 = 0.030, 0.370
	z0, z1 = 0.190, 0.620
	chamfer = 0.070
	# the vision blocks' band (up), and the posts between them
	band = (0.486, 0.560)
	walls = Mesh('frame hull', 'vehicles\\scorpion\\shaders\\hull_non_reflective')
	fittings = Mesh('frame hull', 'vehicles\\scorpion\\shaders\\hull_non_reflective')
	height = z1 - z0
	t0, t1 = band[0] - z0, band[1] - z0
	# the front wall, between the corners: three vision blocks
	front_length = (y1 - chamfer) - (y0 + chamfer)
	wall_with_openings(walls, (x1, y0 + chamfer, z0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), front_length, height,
		[(0.010, 0.068, t0, t1), (0.082, 0.118, t0, t1), (0.132, 0.190, t0, t1)], eye, SCORPION_PLATE_WIDE,
		reveal=0.025, reveal_region=SCORPION_DARK)
	# the corners, each with a vision block
	for (ya, yb), (xa, xb) in (((y0, y0 + chamfer), (x1 - chamfer, x1)), ((y1, y1 - chamfer), (x1 - chamfer, x1))):
		along = normalize((xb - xa, yb - ya, 0.0))
		length = math.hypot(xb - xa, yb - ya)
		wall_with_openings(walls, (xa, ya, z0), along, (0.0, 0.0, 1.0), length, height,
			[(0.018, length - 0.018, t0, t1)], eye, SCORPION_PLATE, reveal=0.025, reveal_region=SCORPION_DARK)
	# the side walls, the back, the floor and the ceiling (the hatch in it)
	walls.face([(x0, y0, z0), (x1 - chamfer, y0, z0), (x1 - chamfer, y0, z1), (x0, y0, z1)], eye, SCORPION_PLATE_WIDE)
	walls.face([(x0, y1, z0), (x1 - chamfer, y1, z0), (x1 - chamfer, y1, z1), (x0, y1, z1)], eye, SCORPION_PLATE_WIDE)
	walls.face([(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)], eye, SCORPION_PLATE_WIDE)
	floor = [(x0, y0, z0), (x1 - chamfer, y0, z0), (x1, y0 + chamfer, z0), (x1, y1 - chamfer, z0),
		(x1 - chamfer, y1, z0), (x0, y1, z0)]
	walls.face(floor, eye, SCORPION_DARK)
	hatch = (0.395, 0.535, 0.110, 0.290)
	ceiling = [(x0, y0, z1), (x1 - chamfer, y0, z1), (x1, y0 + chamfer, z1), (x1, y1 - chamfer, z1),
		(x1 - chamfer, y1, z1), (x0, y1, z1)]
	# (the ceiling about the hatch: the strips round it)
	hx0, hx1, hy0, hy1 = hatch
	walls.face([(x0, y0, z1), (hx0, y0, z1), (hx0, y1, z1), (x0, y1, z1)], eye, SCORPION_PLATE)
	walls.face([(hx1, y0, z1), (x1 - chamfer, y0, z1), (x1, y0 + chamfer, z1), (x1, y1 - chamfer, z1),
		(x1 - chamfer, y1, z1), (hx1, y1, z1)], eye, SCORPION_PLATE)
	walls.face([(hx0, y0, z1), (hx1, y0, z1), (hx1, hy0, z1), (hx0, hy0, z1)], eye, SCORPION_PLATE)
	walls.face([(hx0, hy1, z1), (hx1, hy1, z1), (hx1, y1, z1), (hx0, y1, z1)], eye, SCORPION_PLATE)
	# the hatch: recessed into the ceiling, its frame's sides, a handle across
	recess = 0.015
	fittings.face([(hx0, hy0, z1 + recess), (hx1, hy0, z1 + recess), (hx1, hy1, z1 + recess), (hx0, hy1, z1 + recess)],
		eye, SCORPION_HATCH)
	for a, b in (((hx0, hy0), (hx1, hy0)), ((hx1, hy0), (hx1, hy1)), ((hx1, hy1), (hx0, hy1)), ((hx0, hy1), (hx0, hy0))):
		fittings.face([(a[0], a[1], z1), (b[0], b[1], z1), (b[0], b[1], z1 + recess), (a[0], a[1], z1 + recess)],
			((hx0 + hx1) / 2, (hy0 + hy1) / 2, z1), SCORPION_DARK)
	fittings.bar((hx0 + 0.030, (hy0 + hy1) / 2 - 0.040, z1 + recess - 0.012), (hx0 + 0.030, (hy0 + hy1) / 2 + 0.040,
		z1 + recess - 0.012), 0.005, SCORPION_DARK)
	for y in ((hy0 + hy1) / 2 - 0.040, (hy0 + hy1) / 2 + 0.040):
		fittings.bar((hx0 + 0.030, y, z1 + recess - 0.012), (hx0 + 0.030, y, z1 + recess), 0.004, SCORPION_DARK)
	# the instrument panel under the vision blocks: sloped back from the front
	# wall, its face down to the floor's level of the legs, dials on top
	px0, pz0, pz1 = 0.585, 0.400, 0.482
	fittings.face([(px0, y0 + 0.020, pz0), (px0, y1 - 0.020, pz0), (x1 - 0.004, y1 - chamfer, pz1), (x1 - 0.004, y0 + chamfer, pz1)],
		eye, SCORPION_SWITCHES)
	fittings.face([(px0, y0 + 0.020, 0.330), (px0, y1 - 0.020, 0.330), (px0, y1 - 0.020, pz0), (px0, y0 + 0.020, pz0)],
		eye, SCORPION_FUSES)
	fittings.face([(px0, y0 + 0.020, 0.330), (px0, y1 - 0.020, 0.330), (x1 - 0.004, y1 - chamfer, 0.330), (x1 - 0.004, y0 + chamfer, 0.330)],
		eye, SCORPION_DARK)
	for ya, yb, ra in ((y0 + 0.020, y0 + chamfer, 1), (y1 - 0.020, y1 - chamfer, -1)):
		fittings.face([(px0, ya, 0.330), (px0, ya, pz0), (x1 - 0.004, yb, pz1), (x1 - 0.004, yb, 0.330)],
			(px0, ya + ra * 0.1, 0.4), SCORPION_DARK)
	# (the dials' box on the panel, in the middle)
	fittings.box((0.600, 0.150, 0.455), (0.630, 0.250, 0.492), {'x': SCORPION_DIALS, 'X': SCORPION_DARK, 'y': SCORPION_DARK,
		'Y': SCORPION_DARK, 'z': SCORPION_DARK, 'Z': SCORPION_DARK}, faces='xyYZ')
	# the steering levers: from the floor ahead of the seat, back to the hands
	for y in (0.105, 0.295):
		fittings.bar((0.555, y, z0), (0.515, y, 0.380), 0.007, SCORPION_DARK)
		fittings.bar((0.515, y - 0.006, 0.380), (0.505, y - 0.006, 0.425), 0.012, SCORPION_PLATE)
	# the seat: its pan, its back (leaning back), a headrest
	fittings.box((0.385, 0.080, 0.300), (0.490, 0.320, 0.330), SCORPION_DARK, faces='XyYZ')
	fittings.face([(0.392, 0.085, 0.330), (0.392, 0.315, 0.330), (0.378, 0.315, 0.470), (0.378, 0.085, 0.470)], eye, SCORPION_DARK)
	fittings.box((0.371, 0.150, 0.480), (0.386, 0.250, 0.560), SCORPION_DARK, faces='XyYZ')
	# a grille on the left wall, a box on the right
	fittings.face([(0.430, y0 + 0.002, 0.420), (0.560, y0 + 0.002, 0.420), (0.560, y0 + 0.002, 0.470), (0.430, y0 + 0.002, 0.470)],
		eye, SCORPION_GRILLE)
	fittings.box((0.430, y1 - 0.045, 0.380), (0.520, y1, 0.470), {'x': SCORPION_BOX, 'X': SCORPION_DARK, 'y': SCORPION_BOX,
		'Y': SCORPION_DARK, 'z': SCORPION_DARK, 'Z': SCORPION_DARK}, faces='xXyZz')
	return {'eye': eye, 'meshes': [walls, fittings]}


def scorpion_hatch():
	"""the commander's place: the eye on the turret's axis (its rotator's node,
	at forward -0.72), a third of a metre above the turret's roof (up 1.337),
	so that the turret turns under it without swinging it; a cupola ring about
	it on the turret's roof, its lid open behind"""
	eye = (-0.720, 0.000, 1.500)
	ring = Mesh('frame turret', 'vehicles\\scorpion\\shaders\\hull_non_reflective')
	inner_ring = Mesh('frame turret', 'vehicles\\scorpion\\shaders\\hull_darkened')
	sides = 10
	cx, cy, top = -0.720, 0.000, 1.330
	r_in, r_out, h = 0.140, 0.175, 0.045
	for k in range(sides):
		a0, a1 = 2 * math.pi * k / sides, 2 * math.pi * (k + 1) / sides
		p = lambda r, a, z: (cx + r * math.cos(a), cy + r * math.sin(a), z)
		mid = (a0 + a1) / 2
		# its outside, its top, its inside (seen from the eye)
		ring.face([p(r_out, a0, top), p(r_out, a1, top), p(r_out, a1, top + h), p(r_out, a0, top + h)],
			p(r_out + 1.0, mid, top + h / 2), SCORPION_PLATE)
		ring.face([p(r_in, a0, top + h), p(r_in, a1, top + h), p(r_out, a1, top + h), p(r_out, a0, top + h)],
			p((r_in + r_out) / 2, mid, top + 1.0), SCORPION_PLATE)
		inner_ring.face([p(r_in, a0, top - 0.06), p(r_in, a1, top - 0.06), p(r_in, a1, top + h), p(r_in, a0, top + h)],
			(cx, cy, top), SCORPION_DARK)
	# the well's floor inside the ring, over the turret's roof (which stands
	# up to 1.337 there)
	inner_ring.face([(cx + r_in * math.cos(2 * math.pi * k / sides), cy + r_in * math.sin(2 * math.pi * k / sides), top + 0.012)
		for k in range(sides)], (cx, cy, top + 1.0), SCORPION_HATCH)
	# the lid, open, hinged at the ring's back and leaning back
	hinge_x, lid = cx - r_out, 0.160
	h0 = (hinge_x, -0.110, top + h)
	h1 = (hinge_x, 0.110, top + h)
	# (open past upright, 110 degrees from closed)
	back = (math.cos(math.radians(110.0)), 0.0, math.sin(math.radians(110.0)))
	f0, f1 = add(h0, scale(back, lid * 2)), add(h1, scale(back, lid * 2))
	ring.box((hinge_x - 0.012, -0.110, top + h - 0.002), (hinge_x + 0.004, 0.110, top + h + 0.012), SCORPION_DARK, faces='xXyYZ')
	ring.face([h0, h1, f1, f0], add(scale(add(h0, f1), 0.5), (0.5, 0.0, 0.2)), SCORPION_HATCH)
	ring.face([h0, h1, f1, f0], add(scale(add(h0, f1), 0.5), (-0.5, 0.0, -0.2)), SCORPION_PLATE)
	return {'eye': eye, 'meshes': [ring, inner_ring]}


CABINS = [
	{'name': 'scorpion_inside', 'title': "the Scorpion's driver, inside", 'model': 'vehicles\\scorpion\\scorpion',
		'base_map_scale': SCORPION_BASE_MAP_SCALE,
		'seat': 'scorpion-driver', 'setting': 'vr.scorpion_view', 'view': 'inside', 'make': scorpion_inside},
	{'name': 'scorpion_hatch', 'title': "the Scorpion's commander, in the turret's hatch",
		'base_map_scale': SCORPION_BASE_MAP_SCALE,
		'model': 'vehicles\\scorpion\\scorpion', 'seat': 'scorpion-driver', 'setting': 'vr.scorpion_view', 'view': 'hatch',
		'make': scorpion_hatch},
]


def check_winding(mesh):
	bad = 0
	for t in mesh.triangles:
		p = [mesh.vertices[i][0] for i in t]
		n = cross(sub(p[1], p[0]), sub(p[2], p[0]))
		if dot(n, mesh.vertices[t[0]][1]) > 0.0:
			bad += 1
	return bad


HEADER = '''/*
VR_CABIN_MODEL.H

The VR mode's cabins (vr_cabin.c): for a seat of a vehicle's model and one
of its views, where the eye is and what is drawn about it that the model
lacks: each mesh's vertices and triangles, in the model's space (world
units: forward, left, up), the node it is drawn from and the shader it is
drawn with. Made by tools/vr_cabin_model.py; edit that, not this.
*/

#ifndef __VR_CABIN_MODEL_H
#define __VR_CABIN_MODEL_H
#pragma once

/* position, normal, texture coordinates */
struct vr_cabin_model_vertex
{
	real position[3];
	real normal[3];
	real texcoord[2];
};

/* a mesh: the model's node it is drawn from and its shader (tag names), its
vertices and triangles (wound as the model's faces are, their outsides
drawn) */
struct vr_cabin_model_mesh
{
	char const *node_name;
	char const *shader_name;
	struct vr_cabin_model_vertex const *vertices;
	short vertex_count;
	word const *triangles;
	short triangle_count;
};

/* a cabin: the vehicle's model and the seat's label it is for, the setting
choosing its view and the value for this one (NULL, NULL: always), the eye
(in the model's space) and its meshes */
struct vr_cabin_model
{
	char const *model_name;
	char const *seat_label;
	char const *setting;
	char const *view;
	real eye[3];
	struct vr_cabin_model_mesh const *meshes;
	short mesh_count;
};

/* the most vertices and triangles of any mesh (for the buffers they are
built in), and of meshes of any cabin */
#define VR_CABIN_MODEL_MAXIMUM_VERTICES %(maximum_vertices)d
#define VR_CABIN_MODEL_MAXIMUM_TRIANGLES %(maximum_triangles)d
#define VR_CABIN_MODEL_MAXIMUM_MESHES %(maximum_meshes)d
%(cabins)s
static struct vr_cabin_model const vr_cabin_models[] =
{
%(table)s
};

#endif // __VR_CABIN_MODEL_H
'''

CABIN = '''
/* %(title)s */
%(meshes)s
static struct vr_cabin_model_mesh const vr_cabin_%(name)s_meshes[] =
{
%(mesh_table)s
};
'''

MESH = '''static struct vr_cabin_model_vertex const vr_cabin_%(name)s_vertices[%(vertex_count)d] =
{
%(vertices)s
};

static word const vr_cabin_%(name)s_triangles[%(triangle_count)d * 3] =
{
%(triangles)s
};
'''

MESH_ENTRY = '''\t{ "%(node)s", "%(shader)s", vr_cabin_%(name)s_vertices, NUMBEROF(vr_cabin_%(name)s_vertices),
\t\tvr_cabin_%(name)s_triangles, NUMBEROF(vr_cabin_%(name)s_triangles) / 3 },'''

ENTRY = '''\t{ "%(model)s", "%(seat)s", %(setting)s, %(view)s, { %(ex)s, %(ey)s, %(ez)s },
\t\tvr_cabin_%(name)s_meshes, NUMBEROF(vr_cabin_%(name)s_meshes) },'''


def f(x):
	s = '%.6f' % x
	return (s[1:] if s == '-0.000000' else s) + 'f'


def c_string(s):
	return s.replace('\\', '\\\\')


def header(cabins):
	parts, table = [], []
	all_meshes = []
	for spec, cabin in cabins:
		mesh_texts, mesh_entries = [], []
		for k, mesh in enumerate(cabin['meshes']):
			name = '%s_%d' % (spec['name'], k)
			vlines = ['\t{ { %s, %s, %s }, { %s, %s, %s }, { %s, %s } },' % (
				f(p[0]), f(p[1]), f(p[2]), f(n[0]), f(n[1]), f(n[2]), f(uv[0]), f(uv[1])) for p, n, uv in mesh.vertices]
			tlines = ['\t' + ' '.join('%d, %d, %d,' % t for t in mesh.triangles[i:i + 6]) for i in range(0, len(mesh.triangles), 6)]
			mesh_texts.append(MESH % {'name': name, 'vertex_count': len(mesh.vertices), 'triangle_count': len(mesh.triangles),
				'vertices': '\n'.join(vlines), 'triangles': '\n'.join(tlines)})
			mesh_entries.append(MESH_ENTRY % {'node': mesh.node, 'shader': c_string(mesh.shader), 'name': name})
			all_meshes.append(mesh)
		parts.append(CABIN % {'title': spec['title'], 'name': spec['name'], 'meshes': '\n'.join(mesh_texts),
			'mesh_table': '\n'.join(mesh_entries)})
		e = cabin['eye']
		table.append(ENTRY % {'model': c_string(spec['model']), 'seat': spec['seat'],
			'setting': '"%s"' % spec['setting'] if spec.get('setting') else 'NULL',
			'view': '"%s"' % spec['view'] if spec.get('view') else 'NULL', 'ex': f(e[0]), 'ey': f(e[1]), 'ez': f(e[2]),
			'name': spec['name']})
	return HEADER % {'cabins': ''.join(parts), 'table': '\n'.join(table),
		'maximum_vertices': max(len(m.vertices) for m in all_meshes),
		'maximum_triangles': max(len(m.triangles) for m in all_meshes),
		'maximum_meshes': max(len(c['meshes']) for s, c in cabins)}


def main():
	here = os.path.dirname(os.path.abspath(__file__))
	parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
	parser.add_argument('--out', default=os.path.join(here, '..', 'port', 'linux', 'game', 'vr_cabin_model.h'))
	parser.add_argument('--obj', help='also write <prefix>_<cabin>.obj and .json')
	args = parser.parse_args()
	cabins = []
	for spec in CABINS:
		cabin = spec['make']()
		for mesh in cabin['meshes']:
			mesh.stored(spec['base_map_scale'])
			bad = check_winding(mesh)
			if bad:
				raise SystemExit('%s: %d triangles wound inward' % (spec['name'], bad))
		cabins.append((spec, cabin))
		print('%s: %s' % (spec['name'], ', '.join('%s %s %d vertices %d triangles' % (m.node, m.shader.split('\\')[-1],
			len(m.vertices), len(m.triangles)) for m in cabin['meshes'])))
		if args.obj:
			prefix = '%s_%s' % (args.obj, spec['name'])
			out = {'eye': cabin['eye'], 'base_map_scale': spec['base_map_scale'], 'meshes': [{'node': m.node, 'shader': m.shader, 'vertices': m.vertices,
				'triangles': m.triangles} for m in cabin['meshes']]}
			json.dump(out, open(prefix + '.json', 'w'))
			with open(prefix + '.obj', 'w') as o:
				base = 1
				for k, m in enumerate(cabin['meshes']):
					o.write('o mesh%d_%s\n' % (k, m.shader.split('\\')[-1]))
					for p, n, uv in m.vertices:
						o.write('v %.7f %.7f %.7f\n' % p)
					for p, n, uv in m.vertices:
						o.write('vt %.6f %.6f\n' % uv)
					for t in m.triangles:
						o.write('f %s\n' % ' '.join('%d/%d' % (base + i, base + i) for i in t))
					base += len(m.vertices)
	with open(args.out, 'w', newline='\n') as o:
		o.write(header(cabins))


if __name__ == '__main__':
	main()
