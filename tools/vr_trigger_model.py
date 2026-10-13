#!/usr/bin/env python3
"""Makes the VR mode's triggers for the first-person weapons whose models
have none (port/linux/game/vr_trigger.c), as tables of vertices and
triangles:

    python3 tools/vr_trigger_model.py                 # port/linux/game/vr_trigger_model.h
    python3 tools/vr_trigger_model.py --obj out/t     # also out/t_<weapon>.obj and .json, to look at

Some first-person models have no trigger: the index finger of their idle
animation hooks round an empty gap under the receiver, ahead of the grip,
where not one vertex of the model is. Each weapon in TRIGGERS has one
modelled there, in its model's space (as the model's vertices are: world
units, forward, left, up), fitted to the finger as the game poses it
(vr_hands.c trigger_fingers):

- the blade: a curved bar swept down a profile (its front face, where the
  finger lies, at each height), of rounded section: the front face crowned
  across, flat sides, the back flat, the corners bevelled. Its top is inside
  the receiver (hidden), it narrows and curls forward to a rounded tip below
  the finger. vr_trigger.c turns it back about its pivot as the finger
  pulls: node 0.
- the housing: a low block with chamfered edges on the receiver's underside
  that the blade comes out of, so that it hangs from something. It does not
  move: node 1.

The normals are smooth round the bevels and the crown and down the blade,
flat across the sides and the back. The blade's faces sample a part of the
weapon's texture across its width and down its length (the trigger the
assault rifle's paints, which no face of its model samples; the sniper
rifle's receiver's underside); the housing's sample the receiver's
underside as the receiver's own faces there map it, so it carries on its
texture. No trigger guard: the hand's middle finger lies where a guard
would close the gap below.

Python 3 alone (no packages): the output is committed, and only this script
is to be edited to change a trigger.
"""

import argparse
import json
import math
import os

# ---------- the triggers

# Each weapon's: its first-person model, the node it hangs from and the
# shader it is drawn with (by tag name); the blade's profile from the top
# (inside the receiver) to the tip: up, the front face's forward, the
# thickness back from the face, the half width; its tip (a smaller row this
# much further along, closing the last); its section's bevel radius and its
# face's crown (how far its middle stands forward of its edges); its pivot
# (forward, up; None: its middle at the receiver's underside, the second
# row) and the height of the finger on its face (which goes back by the
# finger's travel there); the housing (its top, depth, chamfer, back, front
# and half width); the texture: the blade's (across its width, down its
# length) and the receiver's underside's mapping (u at forward x0 and per
# forward, v at the middle and per left).
TRIGGERS = [
	{
		'name': 'assault_rifle',
		'title': "the assault rifle's: an MA5B's",
		'model': 'weapons\\assault rifle\\fp\\fp',
		'node': 'frame gun',
		'shader': 'weapons\\assault rifle\\fp\\shaders\\gun',
		# (the face at the finger's heights, up 0.016 to 0.025, where the earlier
		# blade's was, which the finger's curl is fitted to)
		'rows': [
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
		],
		'tip_step': 0.0005, 'tip_scale': 0.6,
		'bevel': 0.00035, 'crown': 0.00018,
		'pivot': None, 'pull_up': 0.0200,
		'housing': {'up': 0.0264, 'depth': 0.00060, 'chamfer': 0.00025, 'back': 0.0120, 'front': 0.0205, 'half_width': 0.0026},
		# (assault rifle_fp, 512 x 512: the painted trigger)
		'blade_uv': (0.2773, 0.3086, 0.6660, 0.7559),
		'receiver_uv': (0.1458, 0.0145, 3.204, 0.1351, 3.21),
	},
	{
		'name': 'sniper_rifle',
		'title': "the sniper rifle's",
		'model': 'weapons\\sniper rifle\\fp\\fp',
		'node': 'frame gun',
		'shader': 'weapons\\sniper rifle\\shaders\\sniper rifle grip',
		# (under the receiver's flat underside, up 0.0239, between the grip's
		# front, forward 0.0100, and the block ahead, 0.0299: slimmer than the
		# rifle's, its face where the finger's crook is)
		'rows': [
			(0.0265, 0.01470, 0.00300, 0.00120),
			(0.0239, 0.01450, 0.00300, 0.00125),
			(0.0222, 0.01425, 0.00270, 0.00130),
			(0.0205, 0.01410, 0.00250, 0.00135),
			(0.0188, 0.01410, 0.00235, 0.00135),
			(0.0172, 0.01425, 0.00220, 0.00135),
			(0.0157, 0.01465, 0.00205, 0.00130),
			(0.0144, 0.01530, 0.00185, 0.00125),
			(0.0134, 0.01610, 0.00165, 0.00118),
			(0.0127, 0.01700, 0.00145, 0.00110),
		],
		'tip_step': 0.0005, 'tip_scale': 0.6,
		'bevel': 0.00030, 'crown': 0.00015,
		# (above the underside, inside the receiver: the blade swings from a pin)
		'pivot': (0.0131, 0.0255), 'pull_up': 0.0195,
		'housing': {'up': 0.0239, 'depth': 0.00050, 'chamfer': 0.00020, 'back': 0.0108, 'front': 0.0162, 'half_width': 0.0030},
		# (sniper rifle, 512 x 512: the receiver's underside, even dark metal)
		'blade_uv': (0.6150, 0.6550, 0.2580, 0.2840),
		'receiver_uv': (0.6037, 0.0100, 3.13, 0.2709, 3.12),
	},
]


def normalize(v):
	length = math.sqrt(sum(c * c for c in v))
	return tuple(c / length for c in v) if length > 1e-12 else (0.0, 0.0, 1.0)


def section(spec):
	"""the section's outline round from the front face's middle, to the left
	first: (back from the face, across, the outline's normal (back, across)).
	Each flat run's ends carry its normal, so that the flat faces shade flat
	and the bevels and the crown smooth."""
	def half(sign, t, w):
		# (smaller on a small row, the tip's)
		r = min(spec['bevel'], 0.3 * t, 0.4 * w)
		crown = spec['crown'] * min(1.0, w / 0.0015)
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


def blade(spec):
	"""the blade's vertices (position, normal, texcoord, node) and triangles"""
	half = section(spec)
	u0, u1, v0, v1 = spec['blade_uv']
	rows = list(spec['rows'])
	# the face's run down the blade at each row (forward, up), from its
	# neighbours
	def tangent(i, rows):
		a, b = rows[max(i - 1, 0)], rows[min(i + 1, len(rows) - 1)]
		return normalize((b[1] - a[1], b[0] - a[0]))
	# the tip's row: a step further along the last row's run, smaller
	t_last = tangent(len(rows) - 1, rows)
	up, front, t, w = rows[-1]
	step, scale = spec['tip_step'], spec['tip_scale']
	rows.append((up + t_last[1] * step, front + t_last[0] * step, t * scale, w * scale))
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
		v = v0 + (v1 - v0) * i / (len(rows) - 1)
		for k, (back, across, (nb, na)) in enumerate(points):
			x = front + face[0] * -back
			z = up + face[1] * -back
			normal = normalize((face[0] * -nb, na, face[1] * -nb))
			# (across the texture: the front's middle at its middle, the back's
			# middle at its edges, both ways round)
			f = perimeter[k] / perimeter[ring]
			u = u0 + (u1 - u0) * (0.5 + (f if k < len(outline) else f - 1.0))
			vertices.append(((x, across, z), normal, (u, v), 0))
	# the tip's middle
	up, front, t, w = rows[-1]
	run = tangent(len(rows) - 1, rows)
	face = normalize((-run[1], run[0]))
	tip = (front - face[0] * t * 0.5 + run[0] * step * 0.5, 0.0, up - face[1] * t * 0.5 + run[1] * step * 0.5)
	vertices.append((tip, normalize((run[0], 0.0, run[1])), ((u0 + u1) * 0.5, v1), 0))

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


def pivot(spec, rows):
	"""where the blade turns (forward, up): the spec's, or its middle at the
	receiver's underside"""
	if spec['pivot']:
		return spec['pivot']
	up, front, t, w = rows[1]
	return (front - t * 0.5, up)


def housing(spec):
	"""the housing: a block on the receiver's underside, its lower face and
	its four sides chamfered; flat faces (each face its own vertices)"""
	h = spec['housing']
	c = h['chamfer']
	x0, x1, y = h['back'], h['front'], h['half_width']
	top, bottom = h['up'] + 0.0003, h['up'] - h['depth']
	u0, ux, du, v0, dv = spec['receiver_uv']
	def texcoord(x, y):
		return (u0 + (x - ux) * du, v0 - y * dv)
	# the outline at the top (inside the receiver) and at the chamfer, and the
	# lower face inset by the chamfer
	outer = [(x0, -y), (x1, -y), (x1, y), (x0, y)]
	inner = [(x0 + c, -y + c), (x1 - c, -y + c), (x1 - c, y - c), (x0 + c, y - c)]
	vertices, triangles = [], []
	def quad(points, normal):
		base = len(vertices)
		for p in points:
			vertices.append((p, normal, texcoord(p[0], p[1]), 1))
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


def model(spec):
	bv, bt, rows = blade(spec)
	hv, ht = housing(spec)
	vertices = bv + hv
	triangles = bt + [tuple(i + len(bv) for i in t) for t in ht]
	return vertices, triangles, pivot(spec, rows), rows


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

The VR mode's triggers on the first-person weapons whose models have none
(vr_trigger.c): each one's vertices and triangles, in its model's space
(world units: forward, left, up), and what it is drawn on and with.
Made by tools/vr_trigger_model.py; edit that, not this.
*/

#ifndef __VR_TRIGGER_MODEL_H
#define __VR_TRIGGER_MODEL_H
#pragma once

/* position, normal, texture coordinates and node (0 the blade, turned; 1 the
housing, still) */
struct vr_trigger_model_vertex
{
	real position[3];
	real normal[3];
	real texcoord[2];
	short node;
};

/* a weapon's trigger: the first-person model it is drawn on, the node it
hangs from and the shader it is drawn with (tag names); its vertices and
triangles (wound as the model's faces are, their outsides drawn); where the
blade turns back as the finger pulls (forward, up), the height of the
finger on its face (which goes back as far as the finger's weapon has its
trigger go), and the blade's middle, for the lighting */
struct vr_trigger_model
{
	char const *model_name;
	char const *node_name;
	char const *shader_name;
	struct vr_trigger_model_vertex const *vertices;
	short vertex_count;
	word const *triangles;
	short triangle_count;
	real pivot_forward;
	real pivot_up;
	real pull_up;
	real middle_forward;
	real middle_up;
};

/* the most vertices and triangles of any (for the buffers they are built in) */
#define VR_TRIGGER_MODEL_MAXIMUM_VERTICES %(maximum_vertices)d
#define VR_TRIGGER_MODEL_MAXIMUM_TRIANGLES %(maximum_triangles)d
%(weapons)s
static struct vr_trigger_model const vr_trigger_models[] =
{
%(table)s
};

#endif // __VR_TRIGGER_MODEL_H
'''

WEAPON = '''
/* %(title)s */
static struct vr_trigger_model_vertex const vr_trigger_%(name)s_vertices[%(vertex_count)d] =
{
%(vertices)s
};

static word const vr_trigger_%(name)s_triangles[%(triangle_count)d * 3] =
{
%(triangles)s
};
'''

ENTRY = '''\t{
\t\t"%(model)s", "%(node)s", "%(shader)s",
\t\tvr_trigger_%(name)s_vertices, NUMBEROF(vr_trigger_%(name)s_vertices),
\t\tvr_trigger_%(name)s_triangles, NUMBEROF(vr_trigger_%(name)s_triangles) / 3,
\t\t%(pivot_forward)s, %(pivot_up)s, %(pull_up)s, %(middle_forward)s, %(middle_up)s,
\t},'''


def f(x):
	s = '%.6f' % x
	return (s[1:] if s == '-0.000000' else s) + 'f'


def f5(x):
	return '%.5ff' % x


def c_string(s):
	return s.replace('\\', '\\\\')


def header(models):
	weapons, table = [], []
	for spec, (vertices, triangles, pivot_point, rows) in models:
		vlines = []
		for (p, n, uv, node) in vertices:
			vlines.append('\t{ { %s, %s, %s }, { %s, %s, %s }, { %s, %s }, %d },' % (
				f(p[0]), f(p[1]), f(p[2]), f(n[0]), f(n[1]), f(n[2]), f(uv[0]), f(uv[1]), node))
		tlines = []
		for k in range(0, len(triangles), 6):
			tlines.append('\t' + ' '.join('%d, %d, %d,' % t for t in triangles[k:k + 6]))
		middle = rows[len(rows) // 2]
		weapons.append(WEAPON % {
			'title': spec['title'], 'name': spec['name'],
			'vertex_count': len(vertices), 'triangle_count': len(triangles),
			'vertices': '\n'.join(vlines), 'triangles': '\n'.join(tlines)})
		table.append(ENTRY % {
			'model': c_string(spec['model']), 'node': spec['node'], 'shader': c_string(spec['shader']),
			'name': spec['name'],
			'pivot_forward': f5(pivot_point[0]), 'pivot_up': f5(pivot_point[1]), 'pull_up': f5(spec['pull_up']),
			'middle_forward': f5(middle[1] - middle[2] * 0.5), 'middle_up': f5(middle[0])})
	return HEADER % {'weapons': ''.join(weapons), 'table': '\n'.join(table),
		'maximum_vertices': max(len(m[1][0]) for m in models), 'maximum_triangles': max(len(m[1][1]) for m in models)}


def main():
	here = os.path.dirname(os.path.abspath(__file__))
	parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
	parser.add_argument('--out', default=os.path.join(here, '..', 'port', 'linux', 'game', 'vr_trigger_model.h'))
	parser.add_argument('--obj', help='also write <prefix>_<weapon>.obj and .json')
	args = parser.parse_args()
	models = []
	for spec in TRIGGERS:
		vertices, triangles, pivot_point, rows = model(spec)
		bad = check_winding(vertices, triangles)
		if bad:
			raise SystemExit('%s: %d triangles wound inward' % (spec['name'], bad))
		models.append((spec, (vertices, triangles, pivot_point, rows)))
		print('%s: %d vertices, %d triangles, pivot %.5f %.5f' % (
			spec['name'], len(vertices), len(triangles), pivot_point[0], pivot_point[1]))
		if args.obj:
			prefix = '%s_%s' % (args.obj, spec['name'])
			with open(prefix + '.obj', 'w') as out:
				for p, n, uv, node in vertices:
					out.write('v %.7f %.7f %.7f\n' % p)
				for p, n, uv, node in vertices:
					out.write('vt %.6f %.6f\n' % uv)
				for p, n, uv, node in vertices:
					out.write('vn %.6f %.6f %.6f\n' % n)
				for t in triangles:
					out.write('f %s\n' % ' '.join('%d/%d/%d' % (i + 1, i + 1, i + 1) for i in t))
			json.dump({'vertices': vertices, 'triangles': triangles, 'pivot': pivot_point, 'pull_up': spec['pull_up']},
				open(prefix + '.json', 'w'))
	with open(args.out, 'w', newline='\n') as out:
		out.write(header(models))


if __name__ == '__main__':
	main()
