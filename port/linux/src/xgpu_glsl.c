/*
XGPU_GLSL.C

GLSL from the shader intermediate form (xgpu_shader.h): GLSL 4.50 for
desktop OpenGL, ESSL 3.00 or 3.10 for OpenGL ES and WebGL 2, as the backend
asks (struct xgpu_glsl_options).

Pixel shaders: the texture stages, the general combiners with their values
clamped to [-1, 1] between stages, as on the hardware, and the final
combiner; then the features the port adds (per-pixel model lighting, the
enhanced dynamic lights, the HUD meters' coverage, visibility counting).

Vertex programs: Xbox vertex programs finish by converting their clip-space
position to screen space with the viewport constants c[-38] and c[-37],
which Direct3D maintains. The shader written here inverts that transform to
hand OpenGL a clip-space position again.
*/

#include "xgpu_gl.h"
#include "xgpu_shader.h"

#include <stdio.h>
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
	/* columns the menus shift by to center on a wide screen (d3d8_device.c) */
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
		(xgpu_gl.c, halo_vr_set_eye_transforms) */
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
		/* Keep the captured position independent of the eye. NVIDIA's OVR
		multiview compiler otherwise reuses the left eye's modified value
		when it evaluates the right eye. */
		xgpu_text_append(&text, "\tvec4 eye_clip_position = eye_correction[EYE] * clip_position;\n");
		/* except where the game projected a point itself (lens flares,
		sprites: vr_render_screen_point), which moves to each eye in the
		screen by a scale and offset; else none */
		xgpu_text_append(&text, "\tif (!clip_captured)\n\t\toPos.xy = oPos.xy * screen_correction[EYE].xy + "
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
		"\t\tposition = vec4((%s.xyz * c[%d].xyz + (c[%d].xyz + vec3(0.5 + screen_offset, 0.5, 0.0)\n"
		"\t\t\t- viewport_offset.xyz) * %s.w) / scale, %s.w);\n"
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
		multiview ? "eye_clip_position" : "clip_position",
		XGPU_VERTEX_CONSTANT_BIAS - 38, XGPU_VERTEX_CONSTANT_BIAS - 37,
		multiview ? "eye_clip_position" : "clip_position",
		multiview ? "eye_clip_position" : "clip_position");
	if (multiview)
	{
		/* the eyes' images have their first row at the bottom (OpenXR's GL
		images), the game's targets at the top: the eye pass draws upside
		down (xgpu_gl.c, vr_flip_rows) */
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

/* ---------- pixel programs: combiner inputs */

static const char *register_expression(unsigned long reg, int stage, const struct xgpu_combiner_stage *combiner)
{
	static char buffer[4][32];
	static int next = 0;
	char *result = buffer[next++ & 3];

	switch (reg)
	{
	case XGPU_REGISTER_C0:
		if (stage < 0)
			return "ps_final_c0";
		snprintf(result, sizeof(buffer[0]), "ps_c0[%d]", combiner->c0);
		return result;
	case XGPU_REGISTER_C1:
		if (stage < 0)
			return "ps_final_c1";
		snprintf(result, sizeof(buffer[0]), "ps_c1[%d]", combiner->c1);
		return result;
	case XGPU_REGISTER_FOG: return "fog";
	case XGPU_REGISTER_V0: return "v0";
	case XGPU_REGISTER_V1: return "v1";
	case XGPU_REGISTER_T0: return "t0";
	case XGPU_REGISTER_T1: return "t1";
	case XGPU_REGISTER_T2: return "t2";
	case XGPU_REGISTER_T3: return "t3";
	case XGPU_REGISTER_R0: return "r0";
	case XGPU_REGISTER_R1: return "r1";
	case XGPU_REGISTER_V1R0_SUM: return stage < 0 ? "v1r0_sum" : "vec4(0.0)";
	case XGPU_REGISTER_EF_PRODUCT: return stage < 0 ? "ef_product" : "vec4(0.0)";
	default: return "vec4(0.0)";
	}
}

/* an input's value: a vec3 (rgb) or float (alpha) expression */
static void input_value(char *value, unsigned long size, const char *source, const struct xgpu_combiner_input *input,
	BOOL alpha_portion)
{
	if (alpha_portion)
		snprintf(value, size, "%s.%s", source, input->alpha ? "a" : "b");
	else if (input->alpha)
		snprintf(value, size, "vec3(%s.a)", source);
	else
		snprintf(value, size, "%s.rgb", source);
}

/* one combiner input as a vec3 (rgb) or float (alpha) expression */
static void combiner_input(struct xgpu_text *text, const struct xgpu_combiner_input *input, BOOL alpha_portion, int stage,
	const struct xgpu_combiner_stage *combiner)
{
	char value[64];

	input_value(value, sizeof(value), register_expression(input->reg, stage, combiner), input, alpha_portion);
	switch (input->mapping)
	{
	case XGPU_MAP_UNSIGNED_IDENTITY: xgpu_text_append(text, "max(%s, 0.0)", value); break;
	case XGPU_MAP_UNSIGNED_INVERT: xgpu_text_append(text, "(1.0 - clamp(%s, 0.0, 1.0))", value); break;
	case XGPU_MAP_EXPAND_NORMAL: xgpu_text_append(text, "(2.0 * max(%s, 0.0) - 1.0)", value); break;
	case XGPU_MAP_EXPAND_NEGATE: xgpu_text_append(text, "(1.0 - 2.0 * max(%s, 0.0))", value); break;
	case XGPU_MAP_HALFBIAS_NORMAL: xgpu_text_append(text, "(max(%s, 0.0) - 0.5)", value); break;
	case XGPU_MAP_HALFBIAS_NEGATE: xgpu_text_append(text, "(0.5 - max(%s, 0.0))", value); break;
	case XGPU_MAP_SIGNED_IDENTITY: xgpu_text_append(text, "(%s)", value); break;
	default: xgpu_text_append(text, "(-%s)", value); break;
	}
}

/* final combiner inputs only have the unsigned identity and invert mappings */
static void final_input(struct xgpu_text *text, const struct xgpu_combiner_input *input, BOOL alpha_portion)
{
	char value[64];

	input_value(value, sizeof(value), register_expression(input->reg, -1, NULL), input, alpha_portion);
	if (input->mapping == XGPU_MAP_UNSIGNED_INVERT)
		xgpu_text_append(text, "(1.0 - clamp(%s, 0.0, 1.0))", value);
	else
		xgpu_text_append(text, "clamp(%s, 0.0, 1.0)", value);
}

static const char *destination_name(unsigned long reg)
{
	switch (reg)
	{
	case XGPU_REGISTER_V0: return "v0";
	case XGPU_REGISTER_V1: return "v1";
	case XGPU_REGISTER_T0: return "t0";
	case XGPU_REGISTER_T1: return "t1";
	case XGPU_REGISTER_T2: return "t2";
	case XGPU_REGISTER_T3: return "t3";
	case XGPU_REGISTER_R0: return "r0";
	case XGPU_REGISTER_R1: return "r1";
	default: return NULL;
	}
}

static const char *output_mapping(unsigned char mapping)
{
	switch (mapping)
	{
	case XGPU_OUTPUT_BIAS: return "(%s - 0.5)";
	case XGPU_OUTPUT_SHIFT_LEFT_1: return "(%s * 2.0)";
	case XGPU_OUTPUT_SHIFT_LEFT_1_BIAS: return "((%s - 0.5) * 2.0)";
	case XGPU_OUTPUT_SHIFT_LEFT_2: return "(%s * 4.0)";
	case XGPU_OUTPUT_SHIFT_RIGHT_1: return "(%s * 0.5)";
	default: return "(%s)";
	}
}

/* ---------- one general combiner stage */

static void combiner_stage(struct xgpu_text *text, const struct xgpu_pixel_program *program, int stage)
{
	const struct xgpu_combiner_stage *combiner = &program->combiners[stage];
	int portion;

	xgpu_text_append(text, "\t/* combiner stage %d */\n\t{\n", stage);
	for (portion = 0; portion < 2; portion++)
	{
		const struct xgpu_combiner_portion *part = &combiner->portions[portion];
		BOOL alpha = portion == 1;
		const char *type = alpha ? "float" : "vec3";
		const char *prefix = alpha ? "a" : "c";
		const char *mapping = output_mapping(part->output_mapping);
		char mapped[64];

		xgpu_text_append(text, "\t\t%s %sA = ", type, prefix);
		combiner_input(text, &part->inputs[0], alpha, stage, combiner);
		xgpu_text_append(text, ";\n\t\t%s %sB = ", type, prefix);
		combiner_input(text, &part->inputs[1], alpha, stage, combiner);
		xgpu_text_append(text, ";\n\t\t%s %sC = ", type, prefix);
		combiner_input(text, &part->inputs[2], alpha, stage, combiner);
		xgpu_text_append(text, ";\n\t\t%s %sD = ", type, prefix);
		combiner_input(text, &part->inputs[3], alpha, stage, combiner);
		xgpu_text_append(text, ";\n");

		if (part->ab_dot)
			xgpu_text_append(text, "\t\tvec3 cAB = vec3(dot(cA, cB));\n");
		else
			xgpu_text_append(text, "\t\t%s %sAB = %sA * %sB;\n", type, prefix, prefix, prefix);
		if (part->cd_dot)
			xgpu_text_append(text, "\t\tvec3 cCD = vec3(dot(cC, cD));\n");
		else
			xgpu_text_append(text, "\t\t%s %sCD = %sC * %sD;\n", type, prefix, prefix, prefix);
		if (part->mux)
		{
			if (program->mux_msb)
				xgpu_text_append(text, "\t\t%s %sSUM = r0.a >= 0.5 ? %sCD : %sAB;\n", type, prefix, prefix, prefix);
			else
				xgpu_text_append(text, "\t\t%s %sSUM = (int(r0.a * 255.0 + 0.5) & 1) != 0 ? %sCD : %sAB;\n",
					type, prefix, prefix, prefix);
		}
		else
		{
			xgpu_text_append(text, "\t\t%s %sSUM = %sAB + %sCD;\n", type, prefix, prefix, prefix);
		}
		snprintf(mapped, sizeof(mapped), mapping, "%sAB");
		xgpu_text_append(text, "\t\t%sAB = clamp(", prefix);
		xgpu_text_append(text, mapped, prefix);
		xgpu_text_append(text, ", -1.0, 1.0);\n");
		snprintf(mapped, sizeof(mapped), mapping, "%sCD");
		xgpu_text_append(text, "\t\t%sCD = clamp(", prefix);
		xgpu_text_append(text, mapped, prefix);
		xgpu_text_append(text, ", -1.0, 1.0);\n");
		snprintf(mapped, sizeof(mapped), mapping, "%sSUM");
		xgpu_text_append(text, "\t\t%sSUM = clamp(", prefix);
		xgpu_text_append(text, mapped, prefix);
		xgpu_text_append(text, ", -1.0, 1.0);\n");
	}

	/* write back only after both portions have read their inputs */
	for (portion = 0; portion < 2; portion++)
	{
		const struct xgpu_combiner_portion *part = &combiner->portions[portion];
		BOOL alpha = portion == 1;
		const char *prefix = alpha ? "a" : "c";
		const char *component = alpha ? "a" : "rgb";
		const char *ab = destination_name(part->ab);
		const char *cd = destination_name(part->cd);
		const char *sum = destination_name(part->sum);

		if (ab)
		{
			xgpu_text_append(text, "\t\t%s.%s = %sAB;\n", ab, component, prefix);
			if (part->ab_blue_to_alpha)
				xgpu_text_append(text, "\t\t%s.a = cAB.b;\n", ab);
		}
		if (cd)
		{
			xgpu_text_append(text, "\t\t%s.%s = %sCD;\n", cd, component, prefix);
			if (part->cd_blue_to_alpha)
				xgpu_text_append(text, "\t\t%s.a = cCD.b;\n", cd);
		}
		if (sum)
			xgpu_text_append(text, "\t\t%s.%s = %sSUM;\n", sum, component, prefix);
	}
	xgpu_text_append(text, "\t}\n");
}

/* ---------- texture stages */

static const char *sampler_declaration(unsigned char type)
{
	switch (type)
	{
	case _xgpu_sampler_3d: return "sampler3D";
	case _xgpu_sampler_cube: return "samplerCube";
	case _xgpu_sampler_2d_layered: return "sampler2DArray";
	default: return "sampler2D";
	}
}

/* the input texel of a dot product stage, mapped per PSDotMapping */
static void dot_input(struct xgpu_text *text, const struct xgpu_texture_stage *stage)
{
	switch (stage->dot_mapping)
	{
	case XGPU_DOT_ZERO_TO_ONE: xgpu_text_append(text, "t%d.rgb", stage->input); break;
	case XGPU_DOT_MINUS_ONE_TO_ONE_D3D: xgpu_text_append(text, "((t%d.rgb * 255.0 - 128.0) / 127.0)", stage->input); break;
	case XGPU_DOT_MINUS_ONE_TO_ONE: xgpu_text_append(text, "signed_bytes(t%d.rgb)", stage->input); break;
	default: xgpu_text_append(text, "(t%d.rgb * 2.0 - 1.0)", stage->input); break;
	}
}

/* a lookup's arguments' end: ES samplers have no LOD bias, so
D3DTSS_MIPMAPLODBIAS goes to the lookup */
static void sample_end(struct xgpu_text *text, int stage, const struct xgpu_glsl_options *options)
{
	if (options->es)
		xgpu_text_append(text, ", texture_lod_bias[%d])", stage);
	else
		xgpu_text_append(text, ")");
}

static void sample(struct xgpu_text *text, const struct xgpu_pixel_program *program, int stage, const char *coordinates,
	const struct xgpu_glsl_options *options)
{
	switch (program->stages[stage].sampler)
	{
	case _xgpu_sampler_3d:
	case _xgpu_sampler_cube:
		xgpu_text_append(text, "texture(tex%d, (%s).xyz", stage, coordinates);
		break;
	case _xgpu_sampler_2d_layered:
		if (options->foveated)
		{
			/* foveated: the view's layer, an inset's through its fovea_sample
			(fovea_layer) */
			xgpu_text_append(text, "texture(tex%d, fovea_layer((%s).xy * texture_scale[%d].xy, %s, textureSize(tex%d, 0))",
				stage, coordinates, stage, program->multiview == XGPU_MULTIVIEW_RIGHT_EYE ?
					"2 + int(gl_ViewID_OVR)" : program->multiview ? "int(gl_ViewID_OVR)" : "0", stage);
			break;
		}
		/* the VR mode's eye pass: the eye's own layer (in a draw into one
		layer, that eye's), drawn upside down (xgpu_gl.c, vr_flip_rows) */
		xgpu_text_append(text, "texture(tex%d, vec3((%s).xy * texture_scale[%d].xy * vec2(1.0, -1.0) + vec2(0.0, 1.0), %s)",
			stage, coordinates, stage, program->multiview == XGPU_MULTIVIEW_EYES ? "float(gl_ViewID_OVR)" :
				program->multiview == XGPU_MULTIVIEW_RIGHT_EYE ? "1.0" : "0.0");
		break;
	default:
		xgpu_text_append(text, "texture(tex%d, (%s).xy * texture_scale[%d].xy", stage, coordinates, stage);
		break;
	}
	sample_end(text, stage, options);
}

static void texture_stage(struct xgpu_text *text, const struct xgpu_pixel_program *program, int stage,
	const struct xgpu_glsl_options *options)
{
	const struct xgpu_texture_stage *decoded = &program->stages[stage];
	unsigned long mode = decoded->mode;
	char coordinates[96];

	xgpu_text_append(text, "\t/* texture stage %d, mode %lu */\n", stage, (unsigned long)decoded->declared_mode);
	switch (mode)
	{
	case XGPU_TEXTURE_PROJECT2D:
	case XGPU_TEXTURE_PROJECT3D:
		snprintf(coordinates, sizeof(coordinates), "vec4(xT%d.xyz / (xT%d.w != 0.0 ? xT%d.w : 1.0), 1.0)", stage, stage, stage);
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, program, stage, coordinates, options);
		xgpu_text_append(text, ";\n");
		if (stage == 0 && program->point_threshold && decoded->sampler == _xgpu_sampler_2d)
		{
			/* IDs must not be averaged with another segment or corrupted by
			low-alpha artwork. Zero marks the atlas's continuous meters. */
			xgpu_text_append(text,
				"\t{\n\t\tvec2 uv = (%s).xy * texture_scale[0].xy;\n"
				"\t\tivec2 size = textureSize(tex0, 0);\n"
				"\t\tivec2 pixel = clamp(ivec2(floor(uv * vec2(size))), ivec2(0), size - 1);\n"
				"\t\tfloat threshold = texelFetch(tex0, pixel, 0).r;\n"
				"\t\tif (threshold > 0.0) t0.rb = vec2(threshold);\n\t}\n", coordinates);
		}
		break;
	case XGPU_TEXTURE_CUBEMAP:
		snprintf(coordinates, sizeof(coordinates), "xT%d", stage);
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, program, stage, coordinates, options);
		xgpu_text_append(text, ";\n");
		break;
	case XGPU_TEXTURE_PASSTHRU:
		xgpu_text_append(text, "\tt%d = clamp(xT%d, 0.0, 1.0);\n", stage, stage);
		break;
	case XGPU_TEXTURE_CLIPPLANE:
	{
		static const char components[] = "xyzw";
		int component;

		for (component = 0; component < 4; component++)
		{
			xgpu_text_append(text, "\tif (xT%d.%c %s 0.0) discard;\n", stage, components[component],
				(decoded->compare & (1 << component)) ? ">=" : "<");
		}
		xgpu_text_append(text, "\tt%d = vec4(0.0);\n", stage);
		break;
	}
	case XGPU_TEXTURE_BUMPENVMAP:
	case XGPU_TEXTURE_BUMPENVMAP_LUMINANCE:
	{
		int input = decoded->previous;

		xgpu_text_append(text, "\t{\n\t\tvec2 d = signed_bytes(t%d.rgb).rg;\n", input);
		xgpu_text_append(text, "\t\tvec2 coordinates = xT%d.xy + vec2(bump_matrix[%d].x * d.x + bump_matrix[%d].z * d.y,"
			" bump_matrix[%d].y * d.x + bump_matrix[%d].w * d.y);\n", stage, stage, stage, stage, stage);
		xgpu_text_append(text, "\t\tt%d = ", stage);
		sample(text, program, stage, "vec4(coordinates, 0.0, 1.0)", options);
		xgpu_text_append(text, ";\n");
		if (mode == XGPU_TEXTURE_BUMPENVMAP_LUMINANCE)
		{
			xgpu_text_append(text, "\t\tt%d.rgb *= clamp(bump_luminance[%d].x * t%d.b + bump_luminance[%d].y, 0.0, 1.0);\n",
				stage, stage, input, stage);
		}
		xgpu_text_append(text, "\t}\n");
		break;
	}
	case XGPU_TEXTURE_DOT_PRODUCT:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, decoded);
		xgpu_text_append(text, ");\n\tt%d = vec4(0.0);\n", stage);
		break;
	case XGPU_TEXTURE_DOT_ST:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, decoded);
		xgpu_text_append(text, ");\n\tt%d = ", stage);
		snprintf(coordinates, sizeof(coordinates), "vec4(dot%d, dot%d, 0.0, 1.0)", stage - 1, stage);
		sample(text, program, stage, coordinates, options);
		xgpu_text_append(text, ";\n");
		break;
	case XGPU_TEXTURE_DOT_ZW:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, decoded);
		xgpu_text_append(text, ");\n\tt%d = vec4(0.0);\n", stage);
		/* the depth it replaces the fragment's with (xgpu_shader.h,
		xgpu_pixel_program_depth_stage): a divisor that is not positive (a
		fragment behind the eye) gives the far plane */
		if (stage == xgpu_pixel_program_depth_stage(program))
		{
			xgpu_text_append(text, "\treplaced_depth = dot%d > 0.0 ? clamp(dot%d / dot%d / %.1f, 0.0, 1.0) : 1.0;\n",
				stage, stage - 1, stage, (double)XGPU_REPLACED_DEPTH_UNITS);
		}
		break;
	case XGPU_TEXTURE_DOT_REFLECT_DIFFUSE:
		/* the normal takes its third component from stage 3's dot product */
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, decoded);
		xgpu_text_append(text, ");\n\tdot3 = dot(xT3.xyz, ");
		dot_input(text, &program->stages[3]);
		xgpu_text_append(text, ");\n\tt%d = ", stage);
		sample(text, program, stage, "vec4(dot1, dot2, dot3, 1.0)", options);
		xgpu_text_append(text, ";\n");
		break;
	case XGPU_TEXTURE_DOT_REFLECT_SPECULAR:
	case XGPU_TEXTURE_DOT_REFLECT_SPECULAR_CONSTANT:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, decoded);
		xgpu_text_append(text, ");\n\t{\n\t\tvec3 n = vec3(dot1, dot2, dot3);\n");
		if (mode == XGPU_TEXTURE_DOT_REFLECT_SPECULAR)
			xgpu_text_append(text, "\t\tvec3 e = vec3(xT1.w, xT2.w, xT3.w);\n");
		else
			xgpu_text_append(text, "\t\tvec3 e = ps_c0[0].xyz;\n");
		xgpu_text_append(text, "\t\tvec3 r = 2.0 * n * dot(n, e) / max(dot(n, n), 1.0e-20) - e;\n\t\tt%d = ", stage);
		sample(text, program, stage, "vec4(r, 1.0)", options);
		xgpu_text_append(text, ";\n\t}\n");
		break;
	case XGPU_TEXTURE_DOT_STR_3D:
	case XGPU_TEXTURE_DOT_STR_CUBE:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, decoded);
		xgpu_text_append(text, ");\n\tt%d = ", stage);
		sample(text, program, stage, "vec4(dot1, dot2, dot3, 1.0)", options);
		xgpu_text_append(text, ";\n");
		break;
	case XGPU_TEXTURE_DEPENDENT_AR:
		snprintf(coordinates, sizeof(coordinates), "vec4(t%d.a, t%d.r, 0.0, 1.0)", decoded->input, decoded->input);
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, program, stage, coordinates, options);
		xgpu_text_append(text, ";\n");
		break;
	case XGPU_TEXTURE_DEPENDENT_GB:
		snprintf(coordinates, sizeof(coordinates), "vec4(t%d.g, t%d.b, 0.0, 1.0)", decoded->input, decoded->input);
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, program, stage, coordinates, options);
		xgpu_text_append(text, ";\n");
		break;
	default:
		xgpu_text_append(text, "\tt%d = vec4(0.0);\n", stage);
		break;
	}

	/* channels the application marked signed (D3DTSS_COLORSIGN) */
	if (decoded->color_sign && mode != XGPU_TEXTURE_NONE)
	{
		static const char channels[] = "argb";
		int bit;

		for (bit = 0; bit < 4; bit++)
		{
			if (decoded->color_sign & (1 << bit))
				xgpu_text_append(text, "\tt%d.%c = signed_byte(t%d.%c);\n", stage, channels[bit], stage, channels[bit]);
		}
	}
	if (decoded->alpha_kill && mode != XGPU_TEXTURE_NONE)
		xgpu_text_append(text, "\tif (t%d.a == 0.0) discard;\n", stage);
}

/* ---------- the whole pixel shader */

static const char *comparison_operator(unsigned long function)
{
	switch (function)
	{
	case D3DCMP_NEVER: return NULL;
	case D3DCMP_LESS: return "<";
	case D3DCMP_EQUAL: return "==";
	case D3DCMP_LESSEQUAL: return "<=";
	case D3DCMP_GREATER: return ">";
	case D3DCMP_NOTEQUAL: return "!=";
	case D3DCMP_GREATEREQUAL: return ">=";
	default: return "";
	}
}

/* ---------- per-pixel model lighting

The game lights its models for each vertex: its model lighting programs
(nv2a_vsh.c nv2a_vertex_shader_lighting) sum the ambient light, two distant
lights and two point lights into the diffuse color v0. Halo's models have
few vertices, so the light across a curved surface comes out in facets, and
a point light that passes close lights only the vertices it reaches.
model_lighting computes the same sum for each pixel, from the normal and
world position the vertex program hands on and the same constants
(XGPU_MODEL_LIGHT_COUNT): the same falloff, facing, cone and translucency
terms, clamped where the programs clamp them. The diffuse alpha, which the
programs write apart (0, or planar fog's), is kept. */

static void model_lighting(struct xgpu_text *text, BOOL point_lights, BOOL dynamic_lights)
{
	int light;

	xgpu_text_append(text,
		"uniform vec4 model_lights[%d];\n"
		"in vec4 xWorldNormal;\n"
		"%s"
		"vec3 model_lighting()\n"
		"{\n"
		/* the normal's direction between the vertices, at the length the
		vertices' normals have, as the programs light by it unnormalized
		(shorter than 1 on some models: the first-person weapons) */
		"\tvec3 n = xWorldNormal.xyz * (xWorldNormal.w * inversesqrt(max(dot(xWorldNormal.xyz, xWorldNormal.xyz), 1.0e-24)));\n"
		/* the ambient light, the first distant light, which also lights the
		back by the translucency, and the second */
		"\tfloat facing = dot(n, -model_lights[7].xyz);\n"
		"\tvec3 light = model_lights[11].xyz + max(max(facing, -facing * model_lights[0].z), 0.0) * model_lights[8].xyz +\n"
		"\t\tmax(dot(n, -model_lights[9].xyz), 0.0) * model_lights[10].xyz;\n",
		XGPU_MODEL_LIGHT_COUNT, point_lights || dynamic_lights ? "in vec3 xWorldPosition;\n" : "");
	/* each point light: its position and 1 / radius squared, its cone's axis
	and falloff scale, its color and falloff offset (none with the enhanced
	dynamic lights, which light the model in their place: the same lights
	among the rest) */
	for (light = 0; point_lights && !dynamic_lights && light < 2; light++)
	{
		int first = 1 + light * 3;

		xgpu_text_append(text,
			"\t{\n"
			"\t\tvec3 to_light = model_lights[%d].xyz - xWorldPosition;\n"
			"\t\tfloat distance_squared = dot(to_light, to_light);\n"
			"\t\tvec3 l = to_light * inversesqrt(max(distance_squared, 1.0e-12));\n"
			"\t\tlight += max(1.0 - distance_squared * model_lights[%d].w, 0.0) * clamp(dot(l, n), 0.0, 1.0) *\n"
			"\t\t\tclamp(dot(l, -model_lights[%d].xyz) * model_lights[%d].w + model_lights[%d].w, 0.0, 1.0) *\n"
			"\t\t\tmodel_lights[%d].xyz;\n"
			"\t}\n",
			first, first, first + 1, first + 1, first + 2, first + 2);
	}
	xgpu_text_append(text, "\treturn clamp(light, 0.0, 1.0);\n}\n");
}

/* ---------- enhanced dynamic lights

display.dynamic_lights "enhanced" (xgpu.h): the lights near a pixel, found
in its grid cell, light it, in linear space: the light a pixel had (the
lightmap's, the model's ambient and distant lights), taken out of the
display's gamma, gets the dynamic light added under a soft shoulder that
never passes white (the framebuffer holds no more), so that a light's core
comes up bright and its color fades softly outward instead of clipping to a
flat disc. Each light falls off as a small sphere's light does, as the
inverse square of the distance with a soft core (1 / (1 + d^2 / c^2), d the
distance, c the light's radius over the square root of the look's core, and
no more than XGPU_DYNAMIC_LIGHTS_CORE: an explosion's light, reaching ten
world units, is bright about it, not over all it reaches), windowed smoothly
to nothing at the radius, so that its pool is crisp in the middle and has no
edge. The light's color and radius are its
tag's (the game's art), its color taken out of gamma and scaled by the
look's brightness; past white, a little of a channel spills into the others
(the look's spill), as a bright light's core goes white-hot. The structure
is lit through its bump map (its tangent space: the normal map's red along
the tangent, green along the binormal), the light's facing taken off toward
the surface's own horizon so that a bump never lights a face turned away;
models through their normal, a little wrapped (the look's wrap) as the
game's soft lights are. */
void xgpu_dynamic_lights_glsl(struct xgpu_text *text)
{
	xgpu_text_append(text,
		"layout(std140, binding = %d) uniform dynamic_lights_block\n"
		"{\n"
		"\tvec4 dynamic_light_grid[3];\n"
		"\tvec4 dynamic_light_position[%d];\n"
		"\tvec4 dynamic_light_color[%d];\n"
		"\tvec4 dynamic_light_cone[%d];\n"
		"};\n"
		"layout(binding = %d) uniform highp usampler3D dynamic_light_cells;\n"
		"vec3 dynamic_light_sum(vec3 p, vec3 n, vec3 surface_normal, bool environment)\n"
		"{\n"
		"\tvec3 offset = p - dynamic_light_grid[0].xyz;\n"
		"\tvec3 cell = sign(offset) * log2(1.0 + abs(offset) * dynamic_light_grid[1].w) * dynamic_light_grid[1].xyz +\n"
		"\t\tvec3(%d.0, %d.0, %d.0);\n"
		"\tuvec4 mask = texelFetch(dynamic_light_cells, clamp(ivec3(floor(cell)), ivec3(0), ivec3(%d, %d, %d)), 0);\n"
		"\tvec3 sum = vec3(0.0);\n"
		"\tfor (int word = 0; word < 4; word++)\n"
		"\t{\n"
		"\t\tuint bits = mask[word];\n"
		"\t\twhile (bits != 0u)\n"
		"\t\t{\n"
		"\t\t\tint light = word * 32 + findLSB(bits);\n"
		"\t\t\tbits &= bits - 1u;\n"
		"\t\t\tvec4 position = dynamic_light_position[light];\n"
		"\t\t\tif (environment && position.w < 0.0)\n"
		"\t\t\t\tcontinue;\n"
		"\t\t\tvec3 to_light = position.xyz - p;\n"
		"\t\t\tfloat distance_squared = dot(to_light, to_light);\n"
		"\t\t\tfloat x2 = distance_squared * position.w * position.w;\n"
		"\t\t\tif (x2 >= 1.0)\n"
		"\t\t\t\tcontinue;\n"
		"\t\t\tvec3 l = to_light * inversesqrt(max(distance_squared, 1.0e-8));\n"
		"\t\t\tfloat window = 1.0 - x2 * x2;\n"
		"\t\t\tfloat core = max(dynamic_light_grid[2].y * position.w * position.w, %.4f);\n"
		"\t\t\tfloat falloff = window * window / (1.0 + distance_squared * core);\n"
		"\t\t\tfloat facing = environment ?\n"
		"\t\t\t\tmax(dot(n, l), 0.0) * clamp(dot(surface_normal, l) * 4.0 + 0.5, 0.0, 1.0) :\n"
		"\t\t\t\tclamp((dot(n, l) + dynamic_light_grid[2].w) / (1.0 + dynamic_light_grid[2].w), 0.0, 1.0);\n"
		"\t\t\tvec4 cone = dynamic_light_cone[light];\n"
		"\t\t\tvec4 color = dynamic_light_color[light];\n"
		"\t\t\tfloat spot = clamp(dot(l, -cone.xyz) * cone.w + color.w, 0.0, 1.0);\n"
		"\t\t\tsum += color.rgb * (falloff * facing * spot);\n"
		"\t\t}\n"
		"\t}\n"
		"\treturn sum;\n"
		"}\n"
		/* the light a pixel had (in the display's gamma) and the dynamic
		light added (linear), under the shoulder */
		"vec3 dynamic_light_add(vec3 base, vec3 light)\n"
		"{\n"
		"\tlight += vec3(max(max(light.r, max(light.g, light.b)) - 1.0, 0.0) * dynamic_light_grid[2].z);\n"
		"\tvec3 had = pow(clamp(base, 0.0, 1.0), vec3(2.2));\n"
		"\tvec3 room = max(1.0 - had, vec3(1.0e-3));\n"
		"\treturn pow(had + room * (1.0 - exp(-light / room)), vec3(1.0 / 2.2));\n"
		"}\n",
		XGPU_DYNAMIC_LIGHTS_BINDING, XGPU_DYNAMIC_LIGHTS_MAXIMUM, XGPU_DYNAMIC_LIGHTS_MAXIMUM, XGPU_DYNAMIC_LIGHTS_MAXIMUM,
		XGPU_DYNAMIC_LIGHTS_UNIT,
		XGPU_DYNAMIC_LIGHTS_GRID_X / 2, XGPU_DYNAMIC_LIGHTS_GRID_Y / 2, XGPU_DYNAMIC_LIGHTS_GRID_Z / 2,
		XGPU_DYNAMIC_LIGHTS_GRID_X - 1, XGPU_DYNAMIC_LIGHTS_GRID_Y - 1, XGPU_DYNAMIC_LIGHTS_GRID_Z - 1,
		1.0 / (XGPU_DYNAMIC_LIGHTS_CORE * XGPU_DYNAMIC_LIGHTS_CORE));
}

char *xgpu_glsl_pixel_shader(const struct xgpu_pixel_program *program, const struct xgpu_glsl_options *options)
{
	const struct xgpu_final_combiner *final = &program->final;
	struct xgpu_text text = { 0 };
	int stage;
	BOOL replaces_depth = xgpu_pixel_program_depth_stage(program) != 0;

	xgpu_text_append(&text, "#version %s\n", options->version);
	/* a z-sprite's depth, only ever nearer than its card's (conservative:
	the depth test before the shader keeps what it can); OpenGL ES has it
	with the extension */
	if (replaces_depth && options->es)
		xgpu_text_append(&text, "#extension GL_EXT_conservative_depth : enable\n");
	/* samples that pass the depth and stencil tests, as the NV2A's
	occlusion counter (ES's; the VR mode's) */
	if (options->es && program->count_samples)
	{
		xgpu_text_append(&text,
			"layout(early_fragment_tests) in;\n"
			"layout(binding = 0, offset = 0) uniform atomic_uint visible_samples;\n");
	}
	if (options->es)
	{
		xgpu_text_append(&text,
			"precision highp float;\n"
			"precision highp int;\n"
			"precision highp sampler2D;\n"
			"precision highp sampler3D;\n"
			"precision highp samplerCube;\n");
	}
	if (program->multiview == XGPU_MULTIVIEW_EYES || (program->multiview && options->foveated))
		xgpu_text_append(&text, "#extension GL_OVR_multiview2 : require\n");
	if (!options->es && program->count_samples)
	{
		xgpu_text_append(&text,
			"layout(early_fragment_tests) in;\n"
			"layout(binding = 0, offset = 0) uniform atomic_uint visible_samples;\n");
	}
	xgpu_text_append(&text,
		"in vec4 xD0;\n"
		"in vec4 xD1;\n"
		"in vec4 xB0;\n"
		"in vec4 xB1;\n"
		"in vec4 xT0;\n"
		"in vec4 xT1;\n"
		"in vec4 xT2;\n"
		"in vec4 xT3;\n"
		"in float xFog;\n"
		"layout(location = 0) out vec4 fragment_color;\n"
		/* the combiner registers that live in uniforms rather than in the
		program: C0/C1 of each stage and the final combiner, and texture
		constants */
		"uniform vec4 ps_c0[8];\n"
		"uniform vec4 ps_c1[8];\n"
		"uniform vec4 ps_final_c0;\n"
		"uniform vec4 ps_final_c1;\n"
		"uniform vec4 fog_color;\n"
		"uniform vec4 fog_parameters;\n"
		"uniform float alpha_reference;\n"
		"uniform vec4 bump_matrix[4];\n"
		"uniform vec4 bump_luminance[4];\n"
		"uniform vec4 texture_scale[4];\n");
	/* (ES samplers have no LOD bias of their own) */
	if (options->es)
		xgpu_text_append(&text, "uniform vec4 texture_lod_bias;\n");
	for (stage = 0; stage < 4; stage++)
		xgpu_text_append(&text, "uniform %s tex%d;\n", sampler_declaration(program->stages[stage].sampler), stage);
	if (options->foveated && (program->stages[0].sampler == _xgpu_sampler_2d_layered ||
		program->stages[1].sampler == _xgpu_sampler_2d_layered || program->stages[2].sampler == _xgpu_sampler_2d_layered ||
		program->stages[3].sampler == _xgpu_sampler_2d_layered))
	{
		/* (xgpu.h, xgpu_foveated) a point of the screen (0 to 1 from the top
		left of a layered target of this size) in this layer: in an inset's,
		where its view put it, or, where the inset does not reach, in the
		whole eye's layer (the edges of a blur, a distortion); the layer's
		rows upside down (xgpu_gl.c, vr_flip_rows) */
		xgpu_text_append(&text,
			"uniform vec4 fovea_sample[4];\n"
			"vec3 fovea_layer(vec2 uv, int layer, ivec3 size)\n"
			"{\n"
			"	vec2 inset = uv * fovea_sample[layer].xy + fovea_sample[layer].zw / vec2(size.xy);\n"
			"	if (all(greaterThanEqual(inset, vec2(0.0))) && all(lessThanEqual(inset, vec2(1.0))))\n"
			"		uv = inset;\n"
			"	else\n"
			"		layer &= ~1;\n"
			"	return vec3(uv.x, 1.0 - uv.y, float(layer));\n"
			"}\n");
	}
	if (program->source_blend)
		xgpu_text_append(&text, "uniform vec4 blend_constant;\n");
	if (replaces_depth && options->es)
		xgpu_text_append(&text, "#ifdef GL_EXT_conservative_depth\nlayout(depth_less) out highp float gl_FragDepth;\n#endif\n");
	else if (replaces_depth)
		xgpu_text_append(&text, "layout(depth_less) out float gl_FragDepth;\n");
	if (program->dynamic_lights)
		xgpu_dynamic_lights_glsl(&text);
	if (program->dynamic_lights == XGPU_DYNAMIC_LIGHTS_ENVIRONMENT)
		xgpu_text_append(&text, "in vec3 xWorldPosition;\nin vec4 xWorldNormal;\nin vec3 xWorldBinormal;\nin vec3 xWorldTangent;\n");
	if (program->per_pixel_lighting)
		model_lighting(&text, program->per_pixel_lighting == 2, program->dynamic_lights == XGPU_DYNAMIC_LIGHTS_MODEL);
	xgpu_text_append(&text,
		"float signed_byte(float x)\n"
		"{\n"
		"	float b = floor(x * 255.0 + 0.5);\n"
		"	return (b >= 128.0 ? b - 256.0 : b) / 127.0;\n"
		"}\n"
		"vec3 signed_bytes(vec3 x)\n"
		"{\n"
		"	return vec3(signed_byte(x.r), signed_byte(x.g), signed_byte(x.b));\n"
		"}\n"
		"void main()\n"
		"{\n"
		"\tvec4 v0 = xD0;\n"
		"\tvec4 v1 = xD1;\n"
		"\tvec4 t0 = vec4(0.0), t1 = vec4(0.0), t2 = vec4(0.0), t3 = vec4(0.0);\n"
		"\tfloat dot0 = 0.0, dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;\n");
	if (replaces_depth)
		xgpu_text_append(&text, "\tfloat replaced_depth = 1.0;\n");
	if (program->per_pixel_lighting && program->dynamic_lights == XGPU_DYNAMIC_LIGHTS_MODEL)
	{
		xgpu_text_append(&text,
			"\t{\n"
			"\t\tvec3 n = xWorldNormal.xyz * inversesqrt(max(dot(xWorldNormal.xyz, xWorldNormal.xyz), 1.0e-24));\n"
			"\t\tv0.rgb = dynamic_light_add(model_lighting(), dynamic_light_sum(xWorldPosition, n, n, false));\n"
			"\t}\n");
	}
	else if (program->per_pixel_lighting)
		xgpu_text_append(&text, "\tv0.rgb = model_lighting();\n");

	for (stage = 0; stage < 4; stage++)
		texture_stage(&text, program, stage, options);
	/* (the bump map's texel, which the combiners may overwrite: the enhanced
	dynamic lights read it after them) */
	if (program->dynamic_lights == XGPU_DYNAMIC_LIGHTS_ENVIRONMENT)
		xgpu_text_append(&text, "\tvec3 dynamic_light_bump = t0.rgb;\n");

	/* the fog register: rgb is the fog color, alpha the fog factor */
	if (program->fog)
	{
		switch (program->fog_mode)
		{
		case D3DFOG_EXP:
			xgpu_text_append(&text, "\tfloat fog_factor = exp(-fog_parameters.z * xFog);\n");
			break;
		case D3DFOG_EXP2:
			xgpu_text_append(&text, "\tfloat fog_factor = exp(-(fog_parameters.z * xFog) * (fog_parameters.z * xFog));\n");
			break;
		case D3DFOG_LINEAR:
			xgpu_text_append(&text, "\tfloat fog_factor = (fog_parameters.y - xFog) / max(fog_parameters.y - fog_parameters.x, 1.0e-6);\n");
			break;
		default:
			xgpu_text_append(&text, "\tfloat fog_factor = xFog;\n");
			break;
		}
	}
	else
	{
		xgpu_text_append(&text, "\tfloat fog_factor = 1.0;\n");
	}
	xgpu_text_append(&text,
		"\tvec4 fog = vec4(fog_color.rgb, clamp(fog_factor, 0.0, 1.0));\n"
		"\tvec4 r0 = vec4(0.0, 0.0, 0.0, t0.a);\n"
		"\tvec4 r1 = vec4(0.0);\n");

	for (stage = 0; stage < (int)program->combiner_count; stage++)
		combiner_stage(&text, program, stage);

	if (!final->present)
	{
		xgpu_text_append(&text, "\tvec4 result = r0;\n");
	}
	else
	{
		xgpu_text_append(&text, "\tvec4 ef_product = vec4(");
		final_input(&text, &final->inputs[4], FALSE);
		xgpu_text_append(&text, " * ");
		final_input(&text, &final->inputs[5], FALSE);
		xgpu_text_append(&text, ", 0.0);\n");
		xgpu_text_append(&text, "\tvec4 v1r0_sum = vec4(%s + %s, 0.0);\n",
			final->invert_v1 ? "(1.0 - clamp(v1.rgb, 0.0, 1.0))" : "clamp(v1.rgb, 0.0, 1.0)",
			final->invert_r0 ? "(1.0 - clamp(r0.rgb, 0.0, 1.0))" : "clamp(r0.rgb, 0.0, 1.0)");
		if (final->clamp_sum)
			xgpu_text_append(&text, "\tv1r0_sum = clamp(v1r0_sum, 0.0, 1.0);\n");
		xgpu_text_append(&text, "\tvec3 fA = ");
		final_input(&text, &final->inputs[0], FALSE);
		xgpu_text_append(&text, ";\n\tvec3 fB = ");
		final_input(&text, &final->inputs[1], FALSE);
		xgpu_text_append(&text, ";\n\tvec3 fC = ");
		final_input(&text, &final->inputs[2], FALSE);
		xgpu_text_append(&text, ";\n\tvec3 fD = ");
		final_input(&text, &final->inputs[3], FALSE);
		xgpu_text_append(&text, ";\n\tfloat fG = ");
		final_input(&text, &final->inputs[6], TRUE);
		xgpu_text_append(&text, ";\n\tvec4 result = vec4(fA * fB + (1.0 - fA) * fC + fD, fG);\n");
	}

	if (program->dynamic_lights == XGPU_DYNAMIC_LIGHTS_ENVIRONMENT)
	{
		/* the lightmap pass's light, before the textures multiply it: the
		dynamic lights added, by the bump map's normal (texture 0, unsigned:
		0.5 is level) in the vertex's tangent space, times the material's
		color (the final combiner's constant, as the game's own pass) */
		xgpu_text_append(&text,
			"\t{\n"
			"\t\tvec3 surface_normal = normalize(xWorldNormal.xyz);\n"
			"\t\tvec3 bump = %s;\n"
			"\t\tvec3 n = normalize(xWorldTangent * bump.x + xWorldBinormal * bump.y + surface_normal * max(bump.z, 0.05));\n"
			"\t\tresult.rgb = dynamic_light_add(result.rgb,\n"
			"\t\t\tdynamic_light_sum(xWorldPosition, n, surface_normal, true) * pow(ps_final_c0.rgb, vec3(2.2)));\n"
			"\t}\n",
			program->stages[0].sampler == _xgpu_sampler_2d ? "dynamic_light_bump * 2.0 - 1.0" : "vec3(0.0, 0.0, 1.0)");
	}
	if (program->coverage_alpha)
		xgpu_text_append(&text, "\tresult.a = mix(1.0, result.a, t0.g);\n");
	if (program->alpha_test)
	{
		const char *comparison = comparison_operator(program->alpha_test);

		if (!comparison)
		{
			xgpu_text_append(&text, "\tdiscard;\n");
		}
		else if (*comparison && program->alpha_test_samples && comparison[0] != '=' && comparison[0] != '!')
		{
			/* multisampled: the samples covered as alpha passes the reference
			over the pixel (its change across the pixel from fwidth), the rest
			left as they are; alpha itself is kept, the game keeping values of
			its own in destination alpha */
			xgpu_text_append(&text,
				"\tfloat test_alpha = clamp(result.a, 0.0, 1.0) * 255.0;\n"
				"\tfloat test_coverage = clamp(%s(test_alpha - alpha_reference) / max(fwidth(test_alpha), 1.0) + 0.5, 0.0, 1.0);\n"
				"\tint test_samples = int(test_coverage * %d.0 + 0.5);\n"
				"\tif (test_samples == 0) discard;\n"
				"\tgl_SampleMask[0] = (1 << test_samples) - 1;\n",
				comparison[0] == '<' ? "-" : "", (int)program->alpha_test_samples);
		}
		else if (*comparison)
		{
			xgpu_text_append(&text, "\tif (!(floor(clamp(result.a, 0.0, 1.0) * 255.0 + 0.5) %s alpha_reference)) discard;\n", comparison);
		}
	}
	if (options->debug_expression && *options->debug_expression)
		xgpu_text_append(&text, "\tresult = vec4(vec3(%s), 1.0);\n", options->debug_expression);
	if (options->debug_texture0)
		xgpu_text_append(&text, "\tresult = vec4(t0.rgb, 1.0);\n");
	if (options->debug_flat)
		xgpu_text_append(&text, "\tresult = xD0.a > 0.0 ? vec4(xD0.rgb, 1.0) : vec4(1.0, 0.0, 1.0, 1.0);\n");
	/* (no farther than the card: the conservative depth's promise) */
	if (replaces_depth)
		xgpu_text_append(&text, "\tgl_FragDepth = min(replaced_depth, gl_FragCoord.z);\n");
	if (program->count_samples && program->multiview == XGPU_MULTIVIEW_EYES && options->foveated)
	{
		/* (foveated: the whole eyes' layers alone, as unfoveated eyes) */
		xgpu_text_append(&text, "\tif ((gl_ViewID_OVR & 1u) == 0u)\n\t\tatomicCounterIncrement(visible_samples);\n");
	}
	else if (program->count_samples)
		xgpu_text_append(&text, "\tatomicCounterIncrement(visible_samples);\n");
	if (program->source_blend)
	{
		static const char *const factors[] =
		{
			"blend_constant", "(1.0 - blend_constant)", "vec4(blend_constant.a)", "vec4(1.0 - blend_constant.a)"
		};

		xgpu_text_append(&text, "\tfragment_color = clamp(result, 0.0, 1.0) * %s;\n}\n",
			factors[(program->source_blend - 1) & 3]);
		return text.buffer;
	}
	xgpu_text_append(&text, "\tfragment_color = clamp(result, 0.0, 1.0);\n}\n");
	return text.buffer;
}
