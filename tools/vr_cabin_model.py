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
- the Scorpion's driver in the feed cockpit (vr.scorpion_view = "feed"): a
  closed compartment inside the hull, under a hatch in its ceiling (the eye
  comes down through it: vr_seat.c), its front wall curved about the eye
  with the display's opening in it, 85 degrees across and 40 high, which
  the world is seen through as through a window (host_vulkan_vr.c draws the feed's
  look over it); a grate of bars that slides down over the opening (its own
  mesh, which vr_cabin.c moves), a dash under it with a holotable, where the
  tank's hologram stands (vr_cabin.c), the steering levers, side consoles
  and a seat.

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

# the player's body as the game seats it in the Scorpion's driver seat (the
# cyborg's "scorpion-driver unarmed idle", logged in the game: leant back, its
# legs out ahead), from the top of its neck (its neck node), in the hull's
# axes: the lowest of its seat (the hips and thighs), the back of its hips and
# of its chest (with the pack), the top of its thighs and knees, the lowest of
# its feet and their front, and its sides
SEATED_SEAT = -0.232
SEATED_HIPS_BACK = -0.005
SEATED_CHEST_BACK = -0.096
SEATED_KNEES_TOP = -0.127
SEATED_FEET = -0.347
SEATED_FEET_FRONT = 0.470
SEATED_SIDE = 0.095
# where first_person_legs.c draws that body in a cabin: the top of its neck
# 4.9 cm behind and 3.3 cm below the eye (vr.c's top of the real neck, 10 cm
# behind and below the eyes, and the body's own neck 5 cm behind that)
SEATED_NECK_BACK = (0.10 + 0.05) / 3.048
SEATED_NECK_DOWN = 0.10 / 3.048


def seated_body(eye):
	"""where the seated body's armour reaches in a cabin with this eye (the
	hull's axes): its seat, the backs of its hips and chest, the top of its
	thighs and knees, its feet's lowest and front, and its sides (right, left)"""
	x, y, z = eye[0] - SEATED_NECK_BACK, eye[1], eye[2] - SEATED_NECK_DOWN
	return {'seat': z + SEATED_SEAT, 'hips_back': x + SEATED_HIPS_BACK, 'chest_back': x + SEATED_CHEST_BACK,
		'knees': z + SEATED_KNEES_TOP, 'feet': z + SEATED_FEET, 'front': x + SEATED_FEET_FRONT,
		'sides': (y - SEATED_SIDE, y + SEATED_SIDE)}


def scorpion_inside():
	"""the driver's compartment at the front of the hull, under the front
	plate, on the driver's (left) side: inside the hull (its top at up 0.64
	to 0.665 there, its front plate from forward 0.69 down to 0.78), ahead of
	the empty slot under the hatch (to forward 0.365), reaching back
	into it behind the seat; a footwell through the front wall, under the
	vision blocks, for the legs of the player's body seated (seated_body:
	its seat, its floor and the room for its knees under the panel are where
	the body is)"""
	eye = (0.440, 0.200, 0.520)
	body = seated_body(eye)
	x0, x1 = 0.280, 0.660
	y0, y1 = 0.030, 0.370
	z0, z1 = round(body['feet'] - 0.005, 3), 0.620
	# (the room for the knees: under the panel and the footwell's top; the
	# footwell's front)
	knees = round(body['knees'] + 0.015, 3)
	well = round(body['front'] + 0.030, 3)
	chamfer = 0.070
	# the vision blocks' band (up), and the posts between them
	band = (0.486, 0.560)
	walls = Mesh('frame hull', 'vehicles\\scorpion\\shaders\\hull_non_reflective')
	fittings = Mesh('frame hull', 'vehicles\\scorpion\\shaders\\hull_non_reflective')
	height = z1 - z0
	t0, t1 = band[0] - z0, band[1] - z0
	# the front wall, between the corners: three vision blocks, and below
	# them the footwell's mouth
	front_length = (y1 - chamfer) - (y0 + chamfer)
	wall_with_openings(walls, (x1, y0 + chamfer, knees), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), front_length, z1 - knees,
		[(0.010, 0.068, band[0] - knees, band[1] - knees), (0.082, 0.118, band[0] - knees, band[1] - knees),
		(0.132, 0.190, band[0] - knees, band[1] - knees)], eye, SCORPION_PLATE_WIDE,
		reveal=0.025, reveal_region=SCORPION_DARK)
	# the footwell: its sides, its front, its floor and its top
	inside_well = ((x1 + well) / 2, (y0 + y1) / 2, (z0 + knees) / 2)
	fy0, fy1 = y0 + chamfer, y1 - chamfer
	walls.face([(x1, fy0, z0), (well, fy0, z0), (well, fy0, knees), (x1, fy0, knees)], inside_well, SCORPION_PLATE)
	walls.face([(x1, fy1, z0), (well, fy1, z0), (well, fy1, knees), (x1, fy1, knees)], inside_well, SCORPION_PLATE)
	walls.face([(well, fy0, z0), (well, fy1, z0), (well, fy1, knees), (well, fy0, knees)], inside_well, SCORPION_PLATE)
	walls.face([(x1, fy0, z0), (well, fy0, z0), (well, fy1, z0), (x1, fy1, z0)], inside_well, SCORPION_DARK)
	walls.face([(x1, fy0, knees), (well, fy0, knees), (well, fy1, knees), (x1, fy1, knees)], inside_well, SCORPION_DARK)
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
	walls.face(floor, (eye[0], eye[1], z0 + 1.0), SCORPION_DARK)
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
	# wall, its face down to the room for the knees, dials on top
	px0, pz0, pz1 = 0.585, 0.400, 0.482
	fittings.face([(px0, y0 + 0.020, pz0), (px0, y1 - 0.020, pz0), (x1 - 0.004, y1 - chamfer, pz1), (x1 - 0.004, y0 + chamfer, pz1)],
		eye, SCORPION_SWITCHES)
	fittings.face([(px0, y0 + 0.020, knees), (px0, y1 - 0.020, knees), (px0, y1 - 0.020, pz0), (px0, y0 + 0.020, pz0)],
		eye, SCORPION_FUSES)
	fittings.face([(px0, y0 + 0.020, knees), (px0, y1 - 0.020, knees), (x1 - 0.004, y1 - chamfer, knees), (x1 - 0.004, y0 + chamfer, knees)],
		(px0, eye[1], knees - 1.0), SCORPION_DARK)
	for ya, yb, ra in ((y0 + 0.020, y0 + chamfer, 1), (y1 - 0.020, y1 - chamfer, -1)):
		fittings.face([(px0, ya, knees), (px0, ya, pz0), (x1 - 0.004, yb, pz1), (x1 - 0.004, yb, knees)],
			(px0, ya + ra * 0.1, 0.4), SCORPION_DARK)
	# (the dials' box on the panel, in the middle)
	fittings.box((0.600, 0.150, 0.455), (0.630, 0.250, 0.492), {'x': SCORPION_DIALS, 'X': SCORPION_DARK, 'y': SCORPION_DARK,
		'Y': SCORPION_DARK, 'z': SCORPION_DARK, 'Z': SCORPION_DARK}, faces='xyYZ')
	# the steering levers: from the floor ahead of the seat, back to the
	# hands, either side of the body's hips
	for y in (eye[1] - 0.115, eye[1] + 0.115):
		fittings.bar((0.555, y, z0), (0.515, y, 0.380), 0.007, SCORPION_DARK)
		fittings.bar((0.515, y, 0.380), (0.505, y, 0.425), 0.012, SCORPION_PLATE)
	# the seat, under the body's: its pan, its back (leaning back as the
	# body does, behind it), a headrest
	right, left = body['sides']
	seat, back = body['seat'], body['hips_back'] - 0.010
	fittings.box((back, right, seat - 0.030), (back + 0.130, left, seat), SCORPION_DARK, faces='XyYZ')
	rest = (body['chest_back'] - 0.010, seat + 0.240)
	fittings.face([(back, right, seat), (back, left, seat), (rest[0], left, rest[1]), (rest[0], right, rest[1])],
		(eye[0], eye[1], seat + 0.2), SCORPION_DARK)
	fittings.box((rest[0] - 0.004, 0.150, rest[1] - 0.010), (rest[0] + 0.011, 0.250, rest[1] + 0.070), SCORPION_DARK, faces='XyYZ')
	# a grille on the left wall, a box on the right
	fittings.face([(0.430, y0 + 0.002, 0.420), (0.560, y0 + 0.002, 0.420), (0.560, y0 + 0.002, 0.470), (0.430, y0 + 0.002, 0.470)],
		eye, SCORPION_GRILLE)
	fittings.box((0.390, y1 - 0.045, 0.380), (0.480, y1, 0.470), {'x': SCORPION_BOX, 'X': SCORPION_DARK, 'y': SCORPION_BOX,
		'Y': SCORPION_DARK, 'z': SCORPION_DARK, 'Z': SCORPION_DARK}, faces='xXyZz')
	return {'eye': eye, 'meshes': [walls, fittings]}



# the feed cockpit's surfaces: the Pelican's troop bay's (vehicles\\pelican\\
# shaders\\pelican_bay: pelican_bay, 512 x 512: gunmetal panels, bolts, a
# grating, a padded seat, machinery), where the map has the Pelican, else
# the Scorpion's own
PELICAN_BAY = 'vehicles\\pelican\\shaders\\pelican_bay'
FEED_PELICAN = {'walls': PELICAN_BAY, 'fittings': PELICAN_BAY, 'grate': PELICAN_BAY,
	'front': (0.580, 0.010, 0.980, 0.290), 'wall': (0.580, 0.010, 0.980, 0.290), 'ceiling': (0.720, 0.680, 0.900, 0.780),
	'floor': (0.500, 0.200, 0.680, 0.780), 'dark': (0.800, 0.820, 0.890, 0.890), 'well': (0.020, 0.965, 0.220, 0.995),
	'dash_top': (0.720, 0.680, 0.900, 0.780), 'dash_face': (0.370, 0.160, 0.480, 0.330), 'table': (0.800, 0.820, 0.890, 0.890),
	'chevron': (0.020, 0.965, 0.220, 0.995), 'console': (0.740, 0.300, 0.970, 0.650), 'console_top': (0.580, 0.010, 0.980, 0.290),
	'grip': (0.040, 0.280, 0.180, 0.480), 'seat': (0.040, 0.280, 0.180, 0.480), 'bar': (0.800, 0.820, 0.890, 0.890)}
FEED_SCORPION = {'walls': 'vehicles\\scorpion\\shaders\\hull_darkened',
	'fittings': 'vehicles\\scorpion\\shaders\\hull_non_reflective', 'grate': 'vehicles\\scorpion\\shaders\\hull_darkened',
	'front': SCORPION_PLATE_WIDE, 'wall': SCORPION_PLATE_WIDE, 'ceiling': SCORPION_PLATE, 'floor': SCORPION_DARK,
	'dark': SCORPION_DARK, 'well': SCORPION_DARK, 'dash_top': SCORPION_PLATE, 'dash_face': SCORPION_SWITCHES,
	'table': SCORPION_HATCH, 'chevron': SCORPION_PLATE, 'console': SCORPION_FUSES, 'console_top': SCORPION_SWITCHES,
	'grip': SCORPION_PLATE, 'seat': SCORPION_DARK, 'bar': SCORPION_DARK}


def scorpion_feed(surfaces):
	"""the feed cockpit: a round compartment about the eye at the driver's
	place, its wall a cylinder about the eye's upright, the display's opening
	all the way round it from 20 degrees below the eye to 45 above, which the
	world is seen through (the eye rides in and out with the body's climb:
	vr_seat.c); ribs every 45 degrees across the opening (the panels' seams:
	the front panel's, from 45 degrees left to 45 right, and the hull
	cameras' about it), a ceiling over it, the floor, a dash before the eye
	with the holotable, the steering levers, side consoles and a seat within
	it; the grate (the last mesh) over the front panel as it is closed
	(vr_cabin.c lifts it above the ceiling, GRATE_TRAVEL up)"""
	S = surfaces
	eye = (0.465, 0.200, 0.520)
	body = seated_body(eye)
	cx, cy, r = eye[0], eye[1], 0.200
	z0, z1 = round(body['feet'] - 0.005, 3), 0.730
	# (the room for the knees: under the dash and the footwell's top; the
	# footwell's front, and how far about the front its mouth is cut in the
	# wall, degrees)
	knees = round(body['knees'] + 0.015, 3)
	well, mouth = round(body['front'] + 0.030, 3), 33.75
	bottom = eye[2] - r * math.tan(math.radians(20.0))
	top = eye[2] + r * math.tan(math.radians(45.0))
	segments, half = 32, 180.0
	dash = 0.400
	walls = Mesh('frame hull', S['walls'])
	fittings = Mesh('frame hull', S['fittings'])
	grate = Mesh('frame hull', S['grate'])

	def arc(a, radius=r):
		return (cx + radius * math.cos(math.radians(a)), cy + radius * math.sin(math.radians(a)))

	angles = [-180.0 + 360.0 * k / segments for k in range(segments + 1)]
	for k in range(segments):
		a0, a1 = angles[k], angles[k + 1]
		(xa, ya), (xb, yb) = arc(a0), arc(a1)
		u0 = S['front'][0] + (S['front'][2] - S['front'][0]) * (k % 4) / 4
		u1 = S['front'][0] + (S['front'][2] - S['front'][0]) * (k % 4 + 1) / 4
		# (ahead, the footwell's mouth below the room for the knees)
		low = knees if -mouth <= a0 and a1 <= mouth else z0
		for za, zb in ((low, bottom), (top, z1)):
			v0 = S['front'][1] + (S['front'][3] - S['front'][1]) * (z1 - zb) / (z1 - z0)
			v1 = S['front'][1] + (S['front'][3] - S['front'][1]) * (z1 - za) / (z1 - z0)
			walls.face([(xa, ya, za), (xb, yb, za), (xb, yb, zb), (xa, ya, zb)], eye, (u0, v0, u1, v1))
		# (the opening's reveal: its sill and lintel, a little deep)
		(xc, yc), (xd, yd) = arc(a0, r + 0.012), arc(a1, r + 0.012)
		fittings.face([(xa, ya, bottom), (xb, yb, bottom), (xd, yd, bottom), (xc, yc, bottom)], eye, S['dark'])
		fittings.face([(xa, ya, top), (xb, yb, top), (xd, yd, top), (xc, yc, top)], eye, S['dark'])
	# the floor and the ceiling
	ring = [arc(a) for a in angles[:-1]]
	walls.face([(x, y, z0) for x, y in ring], (cx, cy, z0 + 1.0), S['floor'])
	walls.face([(x, y, z1) for x, y in ring], eye, S['ceiling'])
	# the footwell, ahead through the wall's mouth: its sides, its front, its
	# floor and its top, for the body's legs
	(mx, my0), (_, my1) = arc(-mouth), arc(mouth)
	inside_well = ((mx + well) / 2, cy, (z0 + knees) / 2)
	walls.face([(mx, my0, z0), (well, my0, z0), (well, my0, knees), (mx, my0, knees)], inside_well, S['wall'])
	walls.face([(mx, my1, z0), (well, my1, z0), (well, my1, knees), (mx, my1, knees)], inside_well, S['wall'])
	walls.face([(well, my0, z0), (well, my1, z0), (well, my1, knees), (well, my0, knees)], inside_well, S['wall'])
	mouth_arc = [arc(-mouth + 2 * mouth * k / 6) for k in range(7)]
	walls.face([(x, y, z0) for x, y in mouth_arc] + [(well, my1, z0), (well, my0, z0)], inside_well, S['floor'])
	walls.face([(x, y, knees) for x, y in mouth_arc] + [(well, my1, knees), (well, my0, knees)], inside_well, S['dark'])
	# the ribs across the opening, every 45 degrees but ahead and behind (the
	# panels' seams: the front panel from 45 degrees left to 45 right, the
	# rear one as wide behind), a little within it
	for k in range(8):
		if k in (0, 4):
			continue
		xr, yr = arc(-180.0 + 45.0 * k, r - 0.012)
		fittings.bar((xr, yr, bottom - 0.004), (xr, yr, top + 0.004), 0.004, S['dark'])
	# the dash before the eye: its top from its edge to the wall, its face
	# down to the knees
	edge = 0.545
	reach = math.degrees(math.acos((edge - cx) / r))
	front = [arc(-reach + 2 * reach * k / 12) for k in range(13)]
	fittings.face([(edge, front[0][1], dash)] + [(x, y, dash) for x, y in front] + [(edge, front[-1][1], dash)], eye,
		S['dash_top'])
	fittings.face([(edge, front[0][1], knees), (edge, front[-1][1], knees), (edge, front[-1][1], dash),
		(edge, front[0][1], dash)], eye, S['dash_face'])
	# the holotable on the dash, ahead of the eye: an eight-sided pedestal
	tx, ty, tr, th = 0.630, 0.200, 0.030, 0.008
	table = [(tx + tr * math.cos(math.pi * k / 4 + math.pi / 8), ty + tr * math.sin(math.pi * k / 4 + math.pi / 8)) for k in range(8)]
	for k in range(8):
		(xa, ya), (xb, yb) = table[k], table[(k + 1) % 8]
		fittings.face([(xa, ya, dash), (xb, yb, dash), (xb, yb, dash + th), (xa, ya, dash + th)],
			((xa + xb) / 2 + (xa + xb - 2 * tx), (ya + yb) / 2 + (ya + yb - 2 * ty), dash + th / 2), S['dark'])
	fittings.face([(x, y, dash + th) for x, y in table], (tx, ty, 1.0), S['table'])
	# the hull's chevron on it, pointing forward (the hologram's frame)
	fittings.face([(tx + 0.028, ty, dash + th + 0.001), (tx + 0.019, ty + 0.009, dash + th + 0.001),
		(tx + 0.019, ty - 0.009, dash + th + 0.001)], (tx, ty, 1.0), S['chevron'])
	# the side consoles, a box each side of the knees, up from the floor
	for ya, yb, yf in ((0.030, 0.066, 'Y'), (0.334, 0.370, 'y')):
		fittings.box((0.400, ya, z0), (0.540, yb, 0.420), {'x': S['dark'], 'X': S['dark'], 'y': S['console'],
			'Y': S['console'], 'z': S['dark'], 'Z': S['console_top']}, faces='xX' + yf + 'Z')
	# the steering levers: from the floor before the dash, back to the
	# hands, either side of the body's hips
	for y in (cy - 0.114, cy + 0.114):
		fittings.bar((0.515, y, z0), (0.480, y, 0.360), 0.007, S['dark'])
		fittings.bar((0.480, y, 0.360), (0.470, y, 0.405), 0.012, S['grip'])
	# the seat, under the body's: its pan, its back (leaning back as the body
	# does, behind it)
	right, left = body['sides']
	seat, back = body['seat'], body['hips_back'] - 0.010
	fittings.box((back, right, seat - 0.030), (back + 0.130, left, seat), S['seat'], faces='XyYZ')
	rest = (body['chest_back'] - 0.010, seat + 0.240)
	fittings.face([(back, right, seat), (back, left, seat), (rest[0], left, rest[1]), (rest[0], right, rest[1])],
		(eye[0], eye[1], seat + 0.2), S['seat'])
	# the grate over the front panel, closed: rails along it and bars down
	# it every 7.5 degrees, just within the opening
	gr = r - 0.004
	for k in range(12):
		a0, a1 = -45.0 + 90.0 * k / 12, -45.0 + 90.0 * (k + 1) / 12
		(xc, yc), (xd, yd) = arc(a0, gr), arc(a1, gr)
		for zc in (bottom + 0.004, bottom + (top - bottom) / 3, bottom + 2 * (top - bottom) / 3, top - 0.004):
			grate.bar((xc, yc, zc), (xd, yd, zc), 0.003, S['bar'])
	for k in range(1, 12):
		xc, yc = arc(-45.0 + 90.0 * k / 12, gr)
		grate.bar((xc, yc, bottom), (xc, yc, top), 0.0025, S['bar'])
	return {'eye': eye, 'meshes': [walls, fittings, grate], 'grate': 2,
		'inside': (cx - r, cy - r, z0, cx + r, cy + r, z1),
		'display': (cx, cy, r, half, bottom, top), 'hologram': (tx, ty, dash + th + 0.002, 0.070)}


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
	# (the player's body stands in the hatch, its legs down in the turret)
	return {'eye': eye, 'meshes': [ring, inner_ring], 'standing': True}


# ---------- the Banshee (vehicles\banshee\banshee: banshee, 512 x 512)

BANSHEE_BASE_MAP_SCALE = (1.989500, 0.996510)
BANSHEE_CONSOLE = (0.360, 0.915, 0.750, 0.995)
BANSHEE_GLOW = (0.580, 0.555, 0.660, 0.600)
BANSHEE_PLATE = (0.270, 0.620, 0.400, 0.700)
BANSHEE_MACHINE = (0.430, 0.705, 0.750, 0.830)
BANSHEE_SHELL = (0.700, 0.560, 0.970, 0.700)
BANSHEE_DARK = (0.440, 0.730, 0.560, 0.800)

# the canopy (its node "frame cab") as the game poses it in flight, closed
# over the pilot, from the hull's node (logged on the simulated headset: its
# axes, forward, left and up, and its place): it hinges at its front, turned
# 26.5 degrees from the model's pose, in which it stands open
BANSHEE_HULL = (-0.020, 0.000, 0.425)
BANSHEE_CAB = (0.416763, 0.000000, 0.641160)
BANSHEE_CAB_CLOSED = ((0.89494, 0.0, 0.44620), (0.0, 1.0, 0.0), (-0.44620, 0.0, 0.89494), (0.43676, 0.0, 0.21616))


def banshee_closed_to_model(mesh):
	"""a mesh made about the closed canopy, put in the model's pose (the
	canopy's node's vertices are open there): the closed pose's inverse"""
	f, l, u, p = BANSHEE_CAB_CLOSED
	def back(v):
		# (the rotation's transpose)
		return (dot(v, f), dot(v, l), dot(v, u))
	moved = []
	for pos, n, uv in mesh.vertices:
		rel = sub(sub(pos, BANSHEE_HULL), p)
		moved.append((add(BANSHEE_CAB, back(rel)), back(n), uv))
	mesh.vertices = moved


def arc(cx, cy, cz, ry, rz, a0, a1, segments):
	"""points on an ellipse across the Banshee (y, z) at forward cx, from angle
	a0 to a1 (0 to the left, pi/2 up)"""
	return [(cx, cy + ry * math.cos(a0 + (a1 - a0) * k / segments), cz + rz * math.sin(a0 + (a1 - a0) * k / segments))
		for k in range(segments + 1)]


def banshee_cockpit():
	"""the pilot's cockpit, inside the canopy as it closes: on the hull, a sill
	about the pilot at the shoulders, a console ahead with the Covenant's
	glowing panels, two handles; on the canopy (made closed, put open as the
	model has it, so that it opens with the canopy), a rail along each side,
	a spine overhead, a bow ahead framing the view, and a shell behind the
	head. The view stays open ahead and above, as through the canopy: its own
	faces face out, and are not drawn from inside"""
	eye = (0.131, 0.000, 0.920)
	shader = 'vehicles\\banshee\\shaders\\banshee'
	hull = Mesh('frame hull', shader)
	canopy = Mesh('frame cab', shader)
	# the sill: a ledge each side and its inner face down to the pilot's sides
	for sign in (1.0, -1.0):
		yi, yo = 0.165 * sign, 0.205 * sign
		hull.face([(-0.120, yi, 0.860), (0.260, yi, 0.860), (0.260, yo, 0.868), (-0.120, yo, 0.868)], eye, BANSHEE_PLATE)
		hull.face([(-0.120, yi, 0.790), (0.260, yi, 0.790), (0.260, yi, 0.860), (-0.120, yi, 0.860)], eye, BANSHEE_MACHINE)
	# the console: across ahead, its face to the pilot sloping back, its top
	segments = 6
	for k in range(segments):
		a0, a1 = math.pi * k / segments, math.pi * (k + 1) / segments
		def at(x, z, a):
			return (x - 0.05 * math.sin(a), 0.165 * math.cos(a), z)
		# (its face, the glowing panels across it; its top)
		hull.face([at(0.255, 0.760, a0), at(0.255, 0.760, a1), at(0.235, 0.832, a1), at(0.235, 0.832, a0)], eye,
			(BANSHEE_CONSOLE[0] + (BANSHEE_CONSOLE[2] - BANSHEE_CONSOLE[0]) * k / segments, BANSHEE_CONSOLE[1],
			BANSHEE_CONSOLE[0] + (BANSHEE_CONSOLE[2] - BANSHEE_CONSOLE[0]) * (k + 1) / segments, BANSHEE_CONSOLE[3]))
		hull.face([at(0.235, 0.832, a0), at(0.235, 0.832, a1), at(0.330, 0.842, a1), at(0.330, 0.842, a0)], eye, BANSHEE_PLATE)
	# a glowing eye on the console's middle
	hull.box((0.228, -0.025, 0.836), (0.250, 0.025, 0.848), {'x': BANSHEE_GLOW, 'X': BANSHEE_DARK, 'y': BANSHEE_DARK,
		'Y': BANSHEE_DARK, 'z': BANSHEE_DARK, 'Z': BANSHEE_GLOW}, faces='xyYZ')
	# the handles, either side ahead of the pilot, leaning back to the hands
	for sign in (1.0, -1.0):
		hull.bar((0.230, 0.115 * sign, 0.790), (0.205, 0.120 * sign, 0.850), 0.008, BANSHEE_DARK)
		hull.bar((0.205, 0.120 * sign, 0.850), (0.195, 0.120 * sign, 0.880), 0.013, BANSHEE_MACHINE)
	# on the canopy, closed: the rails along its lower sides
	for sign in (1.0, -1.0):
		canopy.bar((-0.200, 0.190 * sign, 0.900), (0.380, 0.180 * sign, 0.905), 0.010, BANSHEE_PLATE)
	# the spine overhead, and the bow ahead (from rail to rail over the top)
	canopy.bar((-0.200, 0.0, 1.030), (0.400, 0.0, 1.045), 0.010, BANSHEE_PLATE)
	bow = arc(0.360, 0.0, 0.905, 0.180, 0.140, 0.0, math.pi, 8)
	for k in range(8):
		canopy.bar(bow[k], bow[k + 1], 0.010, BANSHEE_PLATE)
	# the shell behind the head: across the canopy from rail to rail over the
	# top, its inside toward the pilot, from behind the head back
	back0 = arc(-0.060, 0.0, 0.900, 0.188, 0.135, 0.0, math.pi, 8)
	back1 = arc(-0.260, 0.0, 0.900, 0.188, 0.130, 0.0, math.pi, 8)
	for k in range(8):
		canopy.face([back0[k], back0[k + 1], back1[k + 1], back1[k]], eye,
			(BANSHEE_SHELL[0] + (BANSHEE_SHELL[2] - BANSHEE_SHELL[0]) * k / 8, BANSHEE_SHELL[1],
			BANSHEE_SHELL[0] + (BANSHEE_SHELL[2] - BANSHEE_SHELL[0]) * (k + 1) / 8, BANSHEE_SHELL[3]))
	canopy.face(list(reversed(back1)), eye, BANSHEE_SHELL)
	banshee_closed_to_model(canopy)
	return {'eye': eye, 'meshes': [hull, canopy]}


CABINS = [
	{'name': 'scorpion_inside', 'title': "the Scorpion's driver, inside", 'model': 'vehicles\\scorpion\\scorpion',
		'base_map_scale': SCORPION_BASE_MAP_SCALE,
		'seat': 'scorpion-driver', 'setting': 'vr.scorpion_view', 'view': 'inside', 'make': scorpion_inside},
	{'name': 'scorpion_hatch', 'title': "the Scorpion's commander, in the turret's hatch",
		'base_map_scale': SCORPION_BASE_MAP_SCALE,
		'model': 'vehicles\\scorpion\\scorpion', 'seat': 'scorpion-driver', 'setting': 'vr.scorpion_view', 'view': 'hatch',
		'make': scorpion_hatch},
	{'name': 'scorpion_feed', 'title': "the Scorpion's driver, in the feed cockpit (the Pelican's bay's surfaces)",
		'base_map_scale': SCORPION_BASE_MAP_SCALE,
		'model': 'vehicles\\scorpion\\scorpion', 'seat': 'scorpion-driver', 'setting': 'vr.scorpion_view', 'view': 'feed',
		'make': lambda: scorpion_feed(FEED_PELICAN)},
	{'name': 'scorpion_feed_own', 'title': "the Scorpion's driver, in the feed cockpit (its own surfaces: no Pelican in the map)",
		'base_map_scale': SCORPION_BASE_MAP_SCALE,
		'model': 'vehicles\\scorpion\\scorpion', 'seat': 'scorpion-driver', 'setting': 'vr.scorpion_view', 'view': 'feed',
		'make': lambda: scorpion_feed(FEED_SCORPION)},
	{'name': 'banshee_cockpit', 'title': "the Banshee's pilot", 'model': 'vehicles\\banshee\\banshee',
		'base_map_scale': BANSHEE_BASE_MAP_SCALE, 'seat': 'B-driver', 'make': banshee_cockpit},
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
(in the model's space) and its meshes; a feed cockpit's display (else all
0): the axis its wall curves about (forward, left), its radius, the display's
half angle about it (degrees) and its bottom and top (up); its grate's mesh
(else -1), and its hologram's place on the holotable (its middle's foot)
and length; the box the cockpit is (the least and the most forward, left
and up: the vehicle is not drawn about an eye in it); and whether the
player's body drawn in the seat stands in it (first_person_legs.c), else it
sits, its hips on the cabin's seat and its feet on its floor */
struct vr_cabin_model
{
	char const *model_name;
	char const *seat_label;
	char const *setting;
	char const *view;
	real eye[3];
	struct vr_cabin_model_mesh const *meshes;
	short mesh_count;
	real display[6];
	short grate_mesh;
	real hologram[4];
	real inside[6];
	short standing;
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
\t\tvr_cabin_%(name)s_meshes, NUMBEROF(vr_cabin_%(name)s_meshes),
\t\t{ %(display)s }, %(grate)d, { %(hologram)s }, { %(inside)s }, %(standing)d },'''


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
			'name': spec['name'], 'display': ', '.join(f(x) for x in cabin.get('display', (0,) * 6)),
			'grate': cabin.get('grate', -1), 'hologram': ', '.join(f(x) for x in cabin.get('hologram', (0,) * 4)),
			'inside': ', '.join(f(x) for x in cabin.get('inside', (0,) * 6)), 'standing': int(cabin.get('standing', False))})
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
