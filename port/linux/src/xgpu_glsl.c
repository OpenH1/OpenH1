/*
XGPU_GLSL.C

GLSL from the shader intermediate form (xgpu_shader.h): GLSL 4.50 for
desktop OpenGL, ESSL 3.00 or 3.10 for OpenGL ES and WebGL 2, as the backend
asks (struct xgpu_glsl_options).

Vertex programs: Xbox vertex programs finish by converting their clip-space
position to screen space with the viewport constants c[-38] and c[-37],
which Direct3D maintains. The shader written here inverts that transform to
hand OpenGL a clip-space position again.
*/

#include "xgpu.h"
#include "xgpu_shader.h"

#include <stdlib.h>

/* ---------- vertex programs */

static const char *output_name(unsigned char output)
{
	switch (output)
	{
	case XGPU_OUTPUT_POSITION: return "oPos";
	case XGPU_OUTPUT_DIFFUSE: return "oD0";
	case XGPU_OUTPUT_SPECULAR: return "oD1";
	case XGPU_OUTPUT_FOG: return "oFog";
	case XGPU_OUTPUT_POINT_SIZE: return "oPts";
	case XGPU_OUTPUT_BACK_DIFFUSE: return "oB0";
	case XGPU_OUTPUT_BACK_SPECULAR: return "oB1";
	case XGPU_OUTPUT_TEXTURE0: return "oT0";
	case XGPU_OUTPUT_TEXTURE1: return "oT1";
	case XGPU_OUTPUT_TEXTURE2: return "oT2";
	case XGPU_OUTPUT_TEXTURE3: return "oT3";
	default: return "oUnused";
	}
}

static void write_mask(unsigned long mask, char *out)
{
	/* bit 3 is x */
	int count = 0;

	if (mask & 8) out[count++] = 'x';
	if (mask & 4) out[count++] = 'y';
	if (mask & 2) out[count++] = 'z';
	if (mask & 1) out[count++] = 'w';
	out[count] = 0;
}

static void operand(struct xgpu_text *text, const struct xgpu_vertex_operand *operand, int relative)
{
	static const char swizzle_names[] = "xyzw";

	xgpu_text_append(text, "%s", operand->negate ? "-" : "");
	switch (operand->file)
	{
	case XGPU_VERTEX_TEMPORARY:
		/* r12 reads back the position output */
		if (operand->index == XGPU_VERTEX_POSITION)
			xgpu_text_append(text, "oPos");
		else
			xgpu_text_append(text, "r%lu", (unsigned long)operand->index);
		break;
	case XGPU_VERTEX_INPUT:
		xgpu_text_append(text, "v%lu", (unsigned long)operand->index);
		break;
	case XGPU_VERTEX_CONSTANT:
		if (relative)
			xgpu_text_append(text, "c[clamp(a0 + %lu, 0, %d)]", (unsigned long)operand->index,
				XGPU_VERTEX_CONSTANT_COUNT - 1);
		else
			xgpu_text_append(text, "c[%lu]", (unsigned long)operand->index);
		break;
	default:
		xgpu_text_append(text, "vec4(0.0)");
		break;
	}
	xgpu_text_append(text, ".%c%c%c%c", swizzle_names[operand->swizzle[0]], swizzle_names[operand->swizzle[1]],
		swizzle_names[operand->swizzle[2]], swizzle_names[operand->swizzle[3]]);
}

static const char vertex_prologue[] =
	"uniform vec4 c[192];\n"
	"uniform vec4 viewport_scale;\n"
	"uniform vec4 viewport_offset;\n"
	"uniform float point_size;\n"
	/* columns the menus shift by to center on a wide screen (d3d8_gl.c) */
	"uniform float screen_offset;\n"
	"out vec4 xD0;\n"
	"out vec4 xD1;\n"
	"out vec4 xB0;\n"
	"out vec4 xB1;\n"
	"out vec4 xT0;\n"
	"out vec4 xT1;\n"
	"out vec4 xT2;\n"
	"out vec4 xT3;\n"
	"out float xFog;\n"
	"invariant gl_Position;\n"
	"vec4 unpack_normpacked3(uint p)\n"
	"{\n"
	"	int x = int(p << 21) >> 21;\n"
	"	int y = int(p << 10) >> 21;\n"
	"	int z = int(p) >> 22;\n"
	"	return vec4(float(x) / 1023.0, float(y) / 1023.0, float(z) / 511.0, 1.0);\n"
	"}\n"
	"vec4 nv2a_rcc(float x)\n"
	"{\n"
	"	float r = 1.0 / x;\n"
	"	if (r > 0.0) r = clamp(r, 5.42101e-20, 1.884467e+19);\n"
	"	else r = clamp(r, -1.884467e+19, -5.42101e-20);\n"
	"	return vec4(r);\n"
	"}\n"
	"vec4 nv2a_exp(float x)\n"
	"{\n"
	"	return vec4(exp2(floor(x)), fract(x), exp2(x), 1.0);\n"
	"}\n"
	"vec4 nv2a_log(float x)\n"
	"{\n"
	"	x = abs(x);\n"
	"	if (x == 0.0) return vec4(-1.0e30, 1.0, -1.0e30, 1.0);\n"
	"	float e = floor(log2(x));\n"
	"	return vec4(e, x / exp2(e), log2(x), 1.0);\n"
	"}\n"
	"vec4 nv2a_lit(vec4 s)\n"
	"{\n"
	"	float specular = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)) : 0.0;\n"
	"	return vec4(1.0, max(s.x, 0.0), specular, 1.0);\n"
	"}\n";

char *xgpu_glsl_vertex_shader(const struct xgpu_vertex_program *program, unsigned long packed_attribute_mask,
	const struct nv2a_vertex_lighting *lighting, int multiview, const struct xgpu_glsl_options *options)
{
	struct xgpu_text text = { 0 };
	unsigned long index;

	xgpu_text_append(&text, "#version %s\n", options->version);
	if (multiview && options->foveated)
	{
		/* foveated (xgpu.h, xgpu_foveated): both eyes' two layers, or one
		eye's, in one draw */
		xgpu_text_append(&text,
			"#extension GL_OVR_multiview2 : require\n"
			"layout(num_views = %d) in;\n"
			"#define VIEW (%s + int(gl_ViewID_OVR))\n"
			"#define EYE (VIEW >> 1)\n"
			"uniform vec4 fovea_view[4];\n",
			multiview == XGPU_MULTIVIEW_EYES ? 4 : 2, multiview == XGPU_MULTIVIEW_RIGHT_EYE ? "2" : "0");
	}
	else if (multiview == XGPU_MULTIVIEW_EYES)
	{
		/* both eyes in one draw: each eye's clip position is a fixed
		transform of the centre view's, for the projection the game has set
		(d3d8_gl.c, halo_vr_set_eye_transforms) */
		xgpu_text_append(&text,
			"#extension GL_OVR_multiview2 : require\n"
			"layout(num_views = 2) in;\n"
			"#define VIEW gl_ViewID_OVR\n");
	}
	else if (multiview == XGPU_MULTIVIEW_LEFT_EYE)
	{
		/* the left eye's alone (a visibility test, the crosshairs) */
		xgpu_text_append(&text, "#define VIEW 0\n");
	}
	else if (multiview == XGPU_MULTIVIEW_RIGHT_EYE)
	{
		/* the right eye's alone (the crosshairs) */
		xgpu_text_append(&text, "#define VIEW 1\n");
	}
	if (multiview && !options->foveated)
		xgpu_text_append(&text, "#define EYE VIEW\n");
	if (multiview)
		xgpu_text_append(&text, "uniform mat4 eye_correction[2];\nuniform vec4 screen_correction[2];\n"
			"uniform float screen_points;\n");
	if (options->es)
		xgpu_text_append(&text, "precision highp float;\nprecision highp int;\n");
	xgpu_text_append(&text, "%s", vertex_prologue);
	/* (the pixel shader's model_lighting; the normal's length in w) */
	if (lighting)
		xgpu_text_append(&text, "out vec4 xWorldNormal;\n");
	if (lighting && (lighting->lights == 2 || lighting->world_found || lighting->lights == XGPU_ENVIRONMENT_LIGHTING))
		xgpu_text_append(&text, "out vec3 xWorldPosition;\n");
	if (lighting && lighting->lights == XGPU_ENVIRONMENT_LIGHTING)
		xgpu_text_append(&text, "out vec3 xWorldBinormal;\nout vec3 xWorldTangent;\n");
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (packed_attribute_mask & (1UL << index))
			xgpu_text_append(&text, "layout(location = %lu) in uint v%lu_packed;\n", index, index);
		else
			xgpu_text_append(&text, "layout(location = %lu) in vec4 v%lu_in;\n", index, index);
	}

	xgpu_text_append(&text, "void main()\n{\n");
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (packed_attribute_mask & (1UL << index))
			xgpu_text_append(&text, "\tvec4 v%lu = unpack_normpacked3(v%lu_packed);\n", index, index);
		else
			xgpu_text_append(&text, "\tvec4 v%lu = v%lu_in;\n", index, index);
	}
	xgpu_text_append(&text,
		"\tvec4 r0 = vec4(0.0), r1 = vec4(0.0), r2 = vec4(0.0), r3 = vec4(0.0);\n"
		"\tvec4 r4 = vec4(0.0), r5 = vec4(0.0), r6 = vec4(0.0), r7 = vec4(0.0);\n"
		"\tvec4 r8 = vec4(0.0), r9 = vec4(0.0), r10 = vec4(0.0), r11 = vec4(0.0);\n"
		"\tvec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n"
		"\tvec4 oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
		"\tvec4 oB0 = vec4(0.0, 0.0, 0.0, 1.0), oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
		"\tvec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0), oT1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
		"\tvec4 oT2 = vec4(0.0, 0.0, 0.0, 1.0), oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
		"\tvec4 oFog = vec4(1.0), oPts = vec4(point_size), oUnused = vec4(0.0);\n"
		"\tint a0 = 0;\n"
		"\tvec4 A, B, C, mac, ilu;\n");
	xgpu_text_append(&text, "\tvec4 clip_position = vec4(0.0);\n\tbool clip_captured = false;\n");
	/* a structure's vertices are in the world: as they are (the enhanced
	dynamic lights, XGPU_ENVIRONMENT_LIGHTING) */
	if (lighting && lighting->lights == XGPU_ENVIRONMENT_LIGHTING)
	{
		xgpu_text_append(&text, "\txWorldPosition = v0.xyz;\n\txWorldNormal = vec4(v1.xyz, 1.0);\n"
			"\txWorldBinormal = v2.xyz;\n\txWorldTangent = v3.xyz;\n");
	}

	for (index = 0; index < program->instruction_count; index++)
	{
		const struct xgpu_vertex_instruction *instruction = &program->instructions[index];
		char mask[5];

		xgpu_text_append(&text, "\t/* %lu */\n", index);
		/* the lighting's normal and position, before the program reuses them */
		if (lighting && lighting->lights != XGPU_ENVIRONMENT_LIGHTING && index == lighting->normal_instruction)
		{
			xgpu_text_append(&text, "\txWorldNormal = vec4(r%lu.xyz, length(r%lu.xyz));\n",
				lighting->normal_register, lighting->normal_register);
		}
		if (lighting && lighting->lights == 2 && index == lighting->position_instruction)
			xgpu_text_append(&text, "\txWorldPosition = r%lu.xyz;\n", lighting->position_register);
		else if (lighting && lighting->lights == 1 && lighting->world_found && index == lighting->world_instruction)
			xgpu_text_append(&text, "\txWorldPosition = r%lu.xyz;\n", lighting->world_register);
		xgpu_text_append(&text, "\tA = "); operand(&text, &instruction->a, instruction->relative); xgpu_text_append(&text, ";\n");
		xgpu_text_append(&text, "\tB = "); operand(&text, &instruction->b, instruction->relative); xgpu_text_append(&text, ";\n");
		xgpu_text_append(&text, "\tC = "); operand(&text, &instruction->c, instruction->relative); xgpu_text_append(&text, ";\n");

		switch (instruction->mac)
		{
		case XGPU_MAC_NOP: break;
		case XGPU_MAC_MOV: xgpu_text_append(&text, "\tmac = A;\n"); break;
		case XGPU_MAC_MUL: xgpu_text_append(&text, "\tmac = A * B;\n"); break;
		case XGPU_MAC_ADD: xgpu_text_append(&text, "\tmac = A + C;\n"); break;
		case XGPU_MAC_MAD: xgpu_text_append(&text, "\tmac = A * B + C;\n"); break;
		case XGPU_MAC_DP3: xgpu_text_append(&text, "\tmac = vec4(dot(A.xyz, B.xyz));\n"); break;
		case XGPU_MAC_DPH: xgpu_text_append(&text, "\tmac = vec4(dot(A.xyz, B.xyz) + B.w);\n"); break;
		case XGPU_MAC_DP4: xgpu_text_append(&text, "\tmac = vec4(dot(A, B));\n"); break;
		case XGPU_MAC_DST: xgpu_text_append(&text, "\tmac = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;
		case XGPU_MAC_MIN: xgpu_text_append(&text, "\tmac = min(A, B);\n"); break;
		case XGPU_MAC_MAX: xgpu_text_append(&text, "\tmac = max(A, B);\n"); break;
		case XGPU_MAC_SLT: xgpu_text_append(&text, "\tmac = vec4(lessThan(A, B));\n"); break;
		case XGPU_MAC_SGE: xgpu_text_append(&text, "\tmac = vec4(greaterThanEqual(A, B));\n"); break;
		case XGPU_MAC_ARL: xgpu_text_append(&text, "\tmac = A;\n"); break;
		default: xgpu_text_append(&text, "\tmac = vec4(0.0);\n"); break;
		}
		switch (instruction->ilu)
		{
		case XGPU_ILU_NOP: break;
		case XGPU_ILU_MOV: xgpu_text_append(&text, "\tilu = C;\n"); break;
		case XGPU_ILU_RCP: xgpu_text_append(&text, "\tilu = vec4(1.0 / C.x);\n"); break;
		case XGPU_ILU_RCC: xgpu_text_append(&text, "\tilu = nv2a_rcc(C.x);\n"); break;
		case XGPU_ILU_RSQ: xgpu_text_append(&text, "\tilu = vec4(inversesqrt(abs(C.x)));\n"); break;
		case XGPU_ILU_EXP: xgpu_text_append(&text, "\tilu = nv2a_exp(C.x);\n"); break;
		case XGPU_ILU_LOG: xgpu_text_append(&text, "\tilu = nv2a_log(C.x);\n"); break;
		case XGPU_ILU_LIT: xgpu_text_append(&text, "\tilu = nv2a_lit(C);\n"); break;
		default: xgpu_text_append(&text, "\tilu = vec4(0.0);\n"); break;
		}
		/* the screen-space conversion takes the reciprocal of the clip-space
		position's w (rcc of r12.w); keep the position it converts */
		if (instruction->captures_clip)
			xgpu_text_append(&text, "\tclip_position = oPos;\n\tclip_captured = true;\n");

		/* results are written only after both units have read their inputs */
		if (instruction->mac == XGPU_MAC_ARL)
		{
			xgpu_text_append(&text, "\ta0 = int(floor(mac.x + 0.001));\n");
		}
		else if (instruction->mac != XGPU_MAC_NOP && instruction->mac_mask)
		{
			write_mask(instruction->mac_mask, mask);
			if (instruction->mac_temporary == XGPU_VERTEX_POSITION)
				xgpu_text_append(&text, "\toPos.%s = mac.%s;\n", mask, mask);
			else
				xgpu_text_append(&text, "\tr%lu.%s = mac.%s;\n", (unsigned long)instruction->mac_temporary, mask, mask);
		}
		if (instruction->ilu != XGPU_ILU_NOP && instruction->ilu_mask)
		{
			write_mask(instruction->ilu_mask, mask);
			if (instruction->ilu_temporary == XGPU_VERTEX_POSITION)
				xgpu_text_append(&text, "\toPos.%s = ilu.%s;\n", mask, mask);
			else
				xgpu_text_append(&text, "\tr%lu.%s = ilu.%s;\n", (unsigned long)instruction->ilu_temporary, mask, mask);
		}
		if (instruction->writes_output)
		{
			write_mask(instruction->output_mask, mask);
			xgpu_text_append(&text, "\t%s.%s = %s.%s;\n", output_name(instruction->output), mask,
				instruction->output_from_ilu ? "ilu" : "mac", mask);
		}
	}

	if (multiview)
	{
		/* a draw that went through clip space (the world's) moves to each
		eye; one drawn in screen space (the HUD's, full-screen passes) is the
		same in both */
		/* (a point the game projected itself is drawn in screen space,
		whether or not the program divides by w) */
		xgpu_text_append(&text, "\tif (screen_points != 0.0)\n\t\tclip_captured = false;\n");
		xgpu_text_append(&text, "\tif (clip_captured)\n\t\tclip_position = eye_correction[EYE] * clip_position;\n");
		/* except where the game projected a point itself (lens flares,
		sprites: vr_render_screen_point), which moves to each eye in the
		screen by a scale and offset; else none */
		xgpu_text_append(&text, "\telse\n\t\toPos.xy = oPos.xy * screen_correction[EYE].xy + "
			"screen_correction[EYE].zw * oPos.w;\n");
	}
	xgpu_text_append(&text,
		"\t/* undo the screen-space conversion done with c[-38] and c[-37] */\n"
		"\tvec3 scale = vec3(viewport_scale.x != 0.0 ? viewport_scale.x : 1.0,\n"
		"\t\tviewport_scale.y != 0.0 ? viewport_scale.y : 1.0,\n"
		"\t\tviewport_scale.z != 0.0 ? viewport_scale.z : 1.0);\n");
	/* Direct3D 8 puts pixel centres on integer screen coordinates (the
	game offsets its screen-space quads by -0.5 to match), OpenGL on
	half-integers */
	/* The conversion is screen = clip * c[-38] * rcc(w) + c[-37]; undoing it
	by multiplying by w again is lossy near the camera plane, where rcc clamps
	and 1/w rounds differently on each GPU (Mali put vertices of the
	first-person weapon at the vanishing point). Where the clip position was
	kept, the same result is computed without dividing. */
	xgpu_text_append(&text,
		"\tvec4 position;\n"
		"\tif (clip_captured)\n"
		"\t\tposition = vec4((clip_position.xyz * c[%d].xyz + (c[%d].xyz + vec3(0.5 + screen_offset, 0.5, 0.0)\n"
		"\t\t\t- viewport_offset.xyz) * clip_position.w) / scale, clip_position.w);\n"
		"\telse\n"
		"\t{\n"
		"\t\tvec3 ndc = (vec3(oPos.xy + vec2(0.5 + screen_offset, 0.5), oPos.z) - viewport_offset.xyz) / scale;\n"
		"\t\tposition = vec4(ndc * oPos.w, oPos.w);\n"
		"\t}\n"
		/* A position whose w is zero, or is not a number, is the clip-space
		origin: the screen conversion's reciprocal is clamped rather than
		infinite, so a large position times a w of zero is exactly zero, and
		the origin is inside the frustum. Nothing then clips the triangle
		away and OpenGL divides zero by zero there: the vertex lands on the
		middle of the screen and the triangle is drawn out to it from the
		first-person weapon, whose pose follows the camera and so reaches the
		camera plane. The divide on the Xbox sends such a vertex to infinity
		and the clipper takes the triangle; put it behind the camera instead,
		which the clipper also takes. */
		"\tif (!(abs(position.w) > 0.0))\n"
		"\t\tposition = vec4(0.0, 0.0, 0.0, -1.0);\n"
		"\tgl_Position = position;\n",
		XGPU_VERTEX_CONSTANT_BIAS - 38, XGPU_VERTEX_CONSTANT_BIAS - 37);
	if (multiview)
	{
		/* the eyes' images have their first row at the bottom (OpenXR's GL
		images), the game's targets at the top: the eye pass draws upside
		down (d3d8_gl.c, vr_flip_rows) */
		xgpu_text_append(&text, "\tgl_Position.y = -gl_Position.y;\n");
	}
	if (multiview && options->foveated)
	{
		/* an inset's view: its part of the eye magnified to fill its layer
		(the whole eye's views: the identity), points with it */
		xgpu_text_append(&text,
			"\tgl_Position.xy = gl_Position.xy * fovea_view[VIEW].xy + fovea_view[VIEW].zw * gl_Position.w;\n"
			"\toPts.x *= fovea_view[VIEW].x;\n");
	}
	/* what glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE) does on desktop
	GL: rows from the top, depth 0..1 */
	if (options->es)
		xgpu_text_append(&text, "\tgl_Position.y = -gl_Position.y;\n\tgl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n");
	xgpu_text_append(&text,
		"\tgl_PointSize = oPts.x;\n"
		"\txD0 = clamp(oD0, 0.0, 1.0);\n"
		"\txD1 = clamp(oD1, 0.0, 1.0);\n"
		"\txB0 = clamp(oB0, 0.0, 1.0);\n"
		"\txB1 = clamp(oB1, 0.0, 1.0);\n"
		"\txT0 = oT0;\n"
		"\txT1 = oT1;\n"
		"\txT2 = oT2;\n"
		"\txT3 = oT3;\n"
		"\txFog = oFog.x;\n"
		"}\n");
	return text.buffer;
}
