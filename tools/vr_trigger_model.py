#!/usr/bin/env python3
"""Makes the VR mode's trigger for the assault rifle's first-person model
(port/linux/game/vr_trigger.c), as a table of vertices and triangles:

    python3 tools/vr_trigger_model.py                 # port/linux/game/vr_trigger_model.h
    python3 tools/vr_trigger_model.py --obj out/ar    # also out/ar.obj and out/ar.json, to look at

The rifle's first-person model (weapons\\assault rifle\\fp\\fp) has no
trigger: the index finger of its idle animation hooks round an empty gap
under the receiver, between the pistol grip's front (forward 0.0103) and the
handguard's rear (0.0302), below the receiver's underside (up 0.0264). This
models an MA5B's trigger there, in the gun node's space and the game's units
(world units: forward, left, up):

- the blade: a curved bar swept down a profile (its front face, where the
  finger lies, at each height), of rounded section: the front face crowned
  across, flat sides, the back flat, the corners bevelled. Its top is inside
  the receiver (hidden), it narrows and curls forward to a rounded tip below
  the finger. vr_trigger.c turns it back about TRIGGER_PIVOT as the finger
  pulls: node 0.
- the housing: a low block with chamfered edges on the receiver's underside
  that the blade comes out of, so that it hangs from something. It does not
  move: node 1.

The normals are smooth round the bevels and the crown and down the blade,
flat across the sides and the back. The blade's faces sample the trigger the
rifle's texture paints (in the stock's side view, which no face of the model
samples), across its width and down its length; the housing's sample the
receiver's underside as the receiver's own faces there do, so it carries on
its texture. No trigger guard: the hand's middle finger lies where a guard
would close the gap below.

Python 3 alone (no packages): the output is committed, and only this script
is to be edited to change the trigger.
"""

import argparse
import json
import math
import os

# ---------- the blade

# its profile from the top (inside the receiver) to the tip: up, the front
# face's forward, the thickness back from the face, the half width. The face
# at the finger's heights (up 0.016 to 0.025) is where the earlier blade's
# was, which the finger's curl (vr_hands.c trigger_fingers) is fitted to.
BLADE_ROWS = [
	(0.0290, 0.01600, 0.00340, 0.00135),
	(0.0264, 0.01570, 0.00340, 0.00140),
	(0.0244, 0.01535, 0.00310, 0.00142),
	(0.0222, 0.01515, 0.00285, 0.00150),
	(0.0200, 0.01510, 0.00270, 0.00155),
	(0.0179, 0.01525, 0.00255, 0.00155),
	(0.0160, 0.01565, 0.00235, 0.00150),
	(0.0144, 0.01630, 0.00210, 0.00145),
	(0.0132, 0.01715, 0.00180, 0.00135),
	(0.0124, 0.01800, 0.00150, 0.00125),
]
# the tip, closing the last row: a smaller row this much further along, then
# its middle
TIP_STEP = 0.0005
TIP_SCALE = 0.6
# the section's bevel radius, and the front face's crown (how far its middle
# stands forward of its edges)
BEVEL = 0.00035
CROWN = 0.00018
# it turns about this point (forward, up): its middle at the receiver's
# underside, where it comes out of the housing
TRIGGER_PIVOT = None  # (from the profile: below)

# ---------- the housing

HOUSING_UP = 0.0264
HOUSING_DEPTH = 0.00060
HOUSING_CHAMFER = 0.00025
HOUSING_BACK = 0.0120
HOUSING_FRONT = 0.0205
HOUSING_HALF_WIDTH = 0.0026

# ---------- the texture (assault rifle_fp, 512 x 512)

# the painted trigger: across its width, down its length
PAINTED_U = (0.2773, 0.3086)
PAINTED_V = (0.6660, 0.7559)
# the receiver's underside, as its own faces map it (u along forward, v
# across)
def receiver_texcoord(x, y):
	return (0.1458 + (x - 0.0145) * 3.204, 0.1351 - y * 3.21)


def normalize(v):
	length = math.sqrt(sum(c * c for c in v))
	return tuple(c / length for c in v) if length > 1e-12 else (0.0, 0.0, 1.0)


def section():
	"""the section's outline round from the front face's middle, to the left
	first: (back from the face, across, the outline's normal (back, across),
	its distance along the outline, for the texture). Each flat run's ends
	carry its normal, so that the flat faces shade flat and the bevels and the
	crown smooth."""
	def half(sign, t, w):
		# (smaller on a small row, the tip's)
		r = min(BEVEL, 0.3 * t, 0.4 * w)
		crown = CROWN * min(1.0, w / 0.0015)
		points = []
		# the crowned front face: its middle and its edge
		points.append((0.0, 0.0, (-1.0, 0.0)))
		edge = w - r
		slope = 2.0 * crown / edge  # of the crown at its edge (back per across)
		points.append((crown, sign * edge, normalize((-1.0, sign * slope))))
		# the front bevel, the side, the back bevel, the back
		for a in (45.0,):
			c, s = math.cos(math.radians(a)), math.sin(math.radians(a))
			points.append((crown + r * (1.0 - c), sign * (edge + r * s), (-c, sign * s)))
		points.append((crown + r, sign * w, (0.0, sign * 1.0)))
		points.append((t - r, sign * w, (0.0, sign * 1.0)))
		for a in (45.0,):
			c, s = math.cos(math.radians(a)), math.sin(math.radians(a))
			points.append((t - r + r * s, sign * (w - r + r * c), (s, sign * c)))
		points.append((t, sign * (w - r), (1.0, 0.0)))
		points.append((t, 0.0, (1.0, 0.0)))
		return points
	return half


def blade():
	"""the blade's vertices (position, normal, texcoord, node) and triangles"""
	half = section()
	rows = list(BLADE_ROWS)
	# the face's run down the blade at each row (forward, up), from its
	# neighbours
	def tangent(i, rows):
		a, b = rows[max(i - 1, 0)], rows[min(i + 1, len(rows) - 1)]
		return normalize((b[1] - a[1], b[0] - a[0]))
	# the tip's row: a step further along the last row's run, smaller
	t_last = tangent(len(rows) - 1, rows)
	up, front, t, w = rows[-1]
	rows.append((up + t_last[1] * TIP_STEP, front + t_last[0] * TIP_STEP, t * TIP_SCALE, w * TIP_SCALE))
	vertices = []
	ring = None
	for i, (up, front, t, w) in enumerate(rows):
		run = tangent(i, rows)
		# the face's normal (forward, out of the face) and the way back into the
		# blade, in the forward/up plane
		face = normalize((-run[1], run[0]))
		if face[0] < 0.0:
			face = (-face[0], -face[1])
		outline = half(1.0, t, w)
		right = half(-1.0, t, w)
		# round: the front's middle, the left side to the back's middle, then the
		# right side from the back's middle (again: there the texture's left
		# edge meets its right) back to the front
		points = outline + list(reversed(right[1:]))
		ring = len(points)
		perimeter = [0.0]
		for k in range(1, ring + 1):
			p, q = points[k - 1], points[k % ring]
			perimeter.append(perimeter[-1] + math.hypot(q[0] - p[0], q[1] - p[1]))
		v = PAINTED_V[0] + (PAINTED_V[1] - PAINTED_V[0]) * i / (len(rows) - 1)
		for k, (back, across, (nb, na)) in enumerate(points):
			x = front + face[0] * -back
			z = up + face[1] * -back
			normal = normalize((face[0] * -nb, na, face[1] * -nb))
			# (across the painted trigger: the front's middle at its middle, the
			# back's middle at its edges, both ways round)
			f = perimeter[k] / perimeter[ring]
			u = PAINTED_U[0] + (PAINTED_U[1] - PAINTED_U[0]) * (0.5 + (f if k < len(outline) else f - 1.0))
			vertices.append(((x, across, z), normal, (u, v), 0))
	# the tip's middle
	up, front, t, w = rows[-1]
	run = tangent(len(rows) - 1, rows)
	face = normalize((-run[1], run[0]))
	tip = (front - face[0] * t * 0.5 + run[0] * TIP_STEP * 0.5, 0.0, up - face[1] * t * 0.5 + run[1] * TIP_STEP * 0.5)
	vertices.append((tip, normalize((run[0], 0.0, run[1])), ((PAINTED_U[0] + PAINTED_U[1]) * 0.5, PAINTED_V[1]), 0))

	triangles = []
	for i in range(len(rows) - 1):
		for k in range(ring):
			a = i * ring + k
			b = i * ring + (k + 1) % ring
			c, d = a + ring, b + ring
			# (none across the back's middle, where its two vertices are one)
			if vertices[a][0] != vertices[b][0]:
				triangles += [(a, b, c), (b, d, c)]
	last = (len(rows) - 1) * ring
	for k in range(ring):
		if vertices[last + k][0] != vertices[last + (k + 1) % ring][0]:
			triangles.append((last + k, last + (k + 1) % ring, len(vertices) - 1))
	return vertices, triangles, rows


def pivot(rows):
	"""the blade's middle at the receiver's underside (forward, up)"""
	up, front, t, w = rows[1]
	return (front - t * 0.5, up)


def housing():
	"""the housing: a block on the receiver's underside, its lower face and
	its four sides chamfered; flat faces (each face its own vertices)"""
	c = HOUSING_CHAMFER
	x0, x1, y = HOUSING_BACK, HOUSING_FRONT, HOUSING_HALF_WIDTH
	top, bottom = HOUSING_UP + 0.0003, HOUSING_UP - HOUSING_DEPTH
	# the outline at the top (inside the receiver) and at the chamfer, and the
	# lower face inset by the chamfer
	outer = [(x0, -y), (x1, -y), (x1, y), (x0, y)]
	inner = [(x0 + c, -y + c), (x1 - c, -y + c), (x1 - c, y - c), (x0 + c, y - c)]
	vertices, triangles = [], []
	def quad(points, normal):
		base = len(vertices)
		for p in points:
			vertices.append((p, normal, receiver_texcoord(p[0], p[1]), 1))
		triangles.extend([(base, base + 1, base + 2), (base, base + 2, base + 3)])
	# the lower face (seen from below: wound as the model's outside faces are)
	quad([(p[0], p[1], bottom) for p in inner], (0.0, 0.0, -1.0))
	for k in range(4):
		a, b = outer[k], outer[(k + 1) % 4]
		ia, ib = inner[k], inner[(k + 1) % 4]
		out = normalize(((a[1] - b[1]) * -1.0, (b[0] - a[0]) * -1.0, 0.0))
		# its side, down to the chamfer, then the chamfer to the lower face
		chamfer_up = bottom + c
		quad([(a[0], a[1], top), (a[0], a[1], chamfer_up), (b[0], b[1], chamfer_up), (b[0], b[1], top)][::-1], out)
		quad([(a[0], a[1], chamfer_up), (ia[0], ia[1], bottom), (ib[0], ib[1], bottom), (b[0], b[1], chamfer_up)][::-1],
			normalize((out[0], out[1], -1.0)))
	return vertices, triangles


def model():
	bv, bt, rows = blade()
	hv, ht = housing()
	vertices = bv + hv
	triangles = bt + [tuple(i + len(bv) for i in t) for t in ht]
	return vertices, triangles, pivot(rows), rows


def check_winding(vertices, triangles):
	"""each triangle's face normal against its vertices' (wound outward)"""
	bad = 0
	for t in triangles:
		p = [vertices[i][0] for i in t]
		e1 = [p[1][k] - p[0][k] for k in range(3)]
		e2 = [p[2][k] - p[0][k] for k in range(3)]
		n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
		avg = [sum(vertices[i][1][k] for i in t) for k in range(3)]
		if sum(n[k] * avg[k] for k in range(3)) > 0.0:
			bad += 1
	return bad


HEADER = '''/*
VR_TRIGGER_MODEL.H

The assault rifle's trigger in the VR mode (vr_trigger.c): its vertices and
triangles, in the rifle's gun node's space (world units: forward, left, up).
Made by tools/vr_trigger_model.py; edit that, not this.
*/

#ifndef __VR_TRIGGER_MODEL_H
#define __VR_TRIGGER_MODEL_H
#pragma once

/* where the blade turns back as the finger pulls (forward, up): its middle
at the receiver's underside */
#define VR_TRIGGER_PIVOT_FORWARD %(pivot_forward).5ff
#define VR_TRIGGER_PIVOT_UP %(pivot_up).5ff
/* its middle, for the lighting */
#define VR_TRIGGER_MIDDLE_FORWARD %(middle_forward).5ff
#define VR_TRIGGER_MIDDLE_UP %(middle_up).5ff

/* position, normal, texture coordinates and node (0 the blade, turned; 1 the
housing, still) */
struct vr_trigger_model_vertex
{
	real position[3];
	real normal[3];
	real texcoord[2];
	short node;
};

static struct vr_trigger_model_vertex const vr_trigger_model_vertices[%(vertex_count)d] =
{
%(vertices)s
};

/* wound as the model's faces are, their outsides drawn */
static word const vr_trigger_model_triangles[%(triangle_count)d * 3] =
{
%(triangles)s
};

#endif // __VR_TRIGGER_MODEL_H
'''


def header(vertices, triangles, pivot_point, rows):
	def f(x):
		s = '%.6f' % x
		return (s[1:] if s == '-0.000000' else s) + 'f'
	vlines = []
	for (p, n, uv, node) in vertices:
		vlines.append('\t{ { %s, %s, %s }, { %s, %s, %s }, { %s, %s }, %d },' % (
			f(p[0]), f(p[1]), f(p[2]), f(n[0]), f(n[1]), f(n[2]), f(uv[0]), f(uv[1]), node))
	tlines = []
	for k in range(0, len(triangles), 6):
		tlines.append('\t' + ' '.join('%d, %d, %d,' % t for t in triangles[k:k + 6]))
	middle = rows[len(rows) // 2]
	return HEADER % {
		'pivot_forward': pivot_point[0], 'pivot_up': pivot_point[1],
		'middle_forward': middle[1] - middle[2] * 0.5, 'middle_up': middle[0],
		'vertex_count': len(vertices), 'triangle_count': len(triangles),
		'vertices': '\n'.join(vlines), 'triangles': '\n'.join(tlines)}


def main():
	here = os.path.dirname(os.path.abspath(__file__))
	parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
	parser.add_argument('--out', default=os.path.join(here, '..', 'port', 'linux', 'game', 'vr_trigger_model.h'))
	parser.add_argument('--obj', help='also write <prefix>.obj and <prefix>.json')
	args = parser.parse_args()
	vertices, triangles, pivot_point, rows = model()
	bad = check_winding(vertices, triangles)
	if bad:
		raise SystemExit('%d triangles wound inward' % bad)
	with open(args.out, 'w', newline='\n') as f:
		f.write(header(vertices, triangles, pivot_point, rows))
	if args.obj:
		with open(args.obj + '.obj', 'w') as f:
			for p, n, uv, node in vertices:
				f.write('v %.7f %.7f %.7f\n' % p)
			for p, n, uv, node in vertices:
				f.write('vt %.6f %.6f\n' % uv)
			for p, n, uv, node in vertices:
				f.write('vn %.6f %.6f %.6f\n' % n)
			for t in triangles:
				f.write('f %s\n' % ' '.join('%d/%d/%d' % (i + 1, i + 1, i + 1) for i in t))
		json.dump({'vertices': vertices, 'triangles': triangles, 'pivot': pivot_point}, open(args.obj + '.json', 'w'))
	print('%d vertices, %d triangles, pivot %.5f %.5f' % (len(vertices), len(triangles), pivot_point[0], pivot_point[1]))


if __name__ == '__main__':
	main()
