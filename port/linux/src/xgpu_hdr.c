/*
XGPU_HDR.C

display.hdr's shared half (xgpu_hdr.h): which draws into the HDR back
buffer may go past paper white, and how far (xgpu_hdr_classify, the front
end's: d3d8_device.c), and the GLSL of the tone map and the encoding, which
tools/metal_post_shaders.py and tools/vulkan_post_shaders.py compile for the
backends, so that every backend runs the same curves.

Which draws. A float target clips nothing, so the classification is of the
draws whose blend could take a pixel out of [0, 1] from values in it (no
pixel shader's result is outside it: the NV2A's clamp), from the blend
alone: an add whose two factors are not one and its complement, nor either
zero (an add, an alpha multiply add, a double multiply, a meter's constant
blend), and a subtraction or a factor of one less the destination's colour
(which a pixel past white makes negative). Of those, what the game draws as
light keeps what it adds: its additive effects, plasma, lens flares, the
sun's glare, the dynamic lights and their glows, a sky's additive layers.
What is a surface's look (a double multiply anywhere; in Faithful the
structure's specular light and reflections and glass's and water's; the
fog screens) and everything drawn over the tone mapped view (the HUD, the
menus, screen flashes) stops at paper white, or where the pixel already
was, as the SDR frame has it. The deliberate glows the game's code names
(halo_hdr_draw_kind: the first-person weapon's displays, meters and
additive parts; the HUD's flashing meters and the motion tracker; the
additive surfaces that are light, which the game draws in [0, 1] though
they stand for light brighter than any surface: a transparent shader the
map gives radiosity power, as a teleporter's field and the light fixtures,
a plasma shader, a light's volume, as a plasma bolt's) are brightened
toward their ceilings and stop there. Destination alpha, which
the game reads as a blend factor (the specular passes, the fog screens), is
never let past 1.
*/

#include "xgpu_backend.h"
#include "xgpu_hdr.h"

/* ---------- the classification */

/* rasterizer.h's _rasterizer_profile_* (the categories halo_hdr_category
gives) */
enum
{
	_category_environment_specular_lightmaps = 12,
	_category_environment_reflection_lightmap_masks,
	_category_environment_reflection_mirrors,
	_category_environment_reflections,
	_category_environment_transparents,
	_category_environment_fog,
	_category_environment_fog_screen,
	_category_water,
	_category_screen_effect = 26,
	_category_hud,
	_category_screen_flash,
};

/* a factor as it acts on the alpha channel (the colour's factors take
alpha there) */
static int alpha_factor(int factor)
{
	switch (factor)
	{
	case XGPU_BLEND_SOURCE_COLOR: return XGPU_BLEND_SOURCE_ALPHA;
	case XGPU_BLEND_ONE_MINUS_SOURCE_COLOR: return XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA;
	case XGPU_BLEND_DESTINATION_COLOR: return XGPU_BLEND_DESTINATION_ALPHA;
	case XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR: return XGPU_BLEND_ONE_MINUS_DESTINATION_ALPHA;
	case XGPU_BLEND_CONSTANT_COLOR: return XGPU_BLEND_CONSTANT_ALPHA;
	case XGPU_BLEND_ONE_MINUS_CONSTANT_COLOR: return XGPU_BLEND_ONE_MINUS_CONSTANT_ALPHA;
	default: return factor;
	}
}

/* one less the factor (-1: none of the factors) */
static int complement(int factor)
{
	switch (factor)
	{
	case XGPU_BLEND_ZERO: return XGPU_BLEND_ONE;
	case XGPU_BLEND_ONE: return XGPU_BLEND_ZERO;
	case XGPU_BLEND_SOURCE_COLOR: return XGPU_BLEND_ONE_MINUS_SOURCE_COLOR;
	case XGPU_BLEND_ONE_MINUS_SOURCE_COLOR: return XGPU_BLEND_SOURCE_COLOR;
	case XGPU_BLEND_SOURCE_ALPHA: return XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA;
	case XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA: return XGPU_BLEND_SOURCE_ALPHA;
	case XGPU_BLEND_DESTINATION_ALPHA: return XGPU_BLEND_ONE_MINUS_DESTINATION_ALPHA;
	case XGPU_BLEND_ONE_MINUS_DESTINATION_ALPHA: return XGPU_BLEND_DESTINATION_ALPHA;
	case XGPU_BLEND_DESTINATION_COLOR: return XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR;
	case XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR: return XGPU_BLEND_DESTINATION_COLOR;
	case XGPU_BLEND_CONSTANT_COLOR: return XGPU_BLEND_ONE_MINUS_CONSTANT_COLOR;
	case XGPU_BLEND_ONE_MINUS_CONSTANT_COLOR: return XGPU_BLEND_CONSTANT_COLOR;
	case XGPU_BLEND_CONSTANT_ALPHA: return XGPU_BLEND_ONE_MINUS_CONSTANT_ALPHA;
	case XGPU_BLEND_ONE_MINUS_CONSTANT_ALPHA: return XGPU_BLEND_CONSTANT_ALPHA;
	default: return -1;
	}
}

/* whether a channel (alpha's, or the colour's) can be blended past 1 from
a source and destination in [0, 1]: an add, unless a factor is zero (the
other term is at most 1), the factors are one and its complement (a mix),
or, for alpha, the source is whole and the destination kept by one less
its alpha (the "over" operator: a + d (1 - a)) */
static BOOL overflows(int source, int destination, int operation, BOOL alpha)
{
	if (operation != XGPU_BLEND_ADD || source == XGPU_BLEND_ZERO || destination == XGPU_BLEND_ZERO)
		return FALSE;
	if (alpha)
	{
		source = alpha_factor(source);
		destination = alpha_factor(destination);
		if (source == XGPU_BLEND_ONE && destination == XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA)
			return FALSE;
	}
	return complement(source) != destination;
}

/* whether the colour can be blended below 0: a subtraction, or one less
the destination's colour past white */
static BOOL underflows(int source, int destination, int operation)
{
	return operation == XGPU_BLEND_SUBTRACT || operation == XGPU_BLEND_REVERSE_SUBTRACT ||
		source == XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR || destination == XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR;
}

/* a surface's look rather than light: kept under paper white */
static BOOL surface(const struct xgpu_hdr_draw *draw)
{
	BOOL multiply = (draw->blend_source == XGPU_BLEND_DESTINATION_COLOR &&
		draw->blend_destination == XGPU_BLEND_SOURCE_COLOR) ||
		(draw->blend_source == XGPU_BLEND_SOURCE_COLOR && draw->blend_destination == XGPU_BLEND_DESTINATION_COLOR);

	if (multiply || draw->interface)
		return TRUE;
	switch (draw->category)
	{
	case _category_environment_fog:
	case _category_environment_fog_screen:
	case _category_screen_effect:
	case _category_hud:
	case _category_screen_flash:
		return TRUE;
	case _category_environment_specular_lightmaps:
	case _category_environment_reflection_lightmap_masks:
	case _category_environment_reflection_mirrors:
	case _category_environment_reflections:
	case _category_water:
		return draw->mode == XGPU_HDR_MODE_FAITHFUL;
	default:
		return (draw->kind & HALO_HDR_KIND_SPECULAR) && draw->mode == XGPU_HDR_MODE_FAITHFUL;
	}
}

unsigned short xgpu_hdr_classify(const struct xgpu_hdr_draw *draw)
{
	int source = draw->blend ? draw->blend_source : XGPU_BLEND_ONE;
	int destination = draw->blend ? draw->blend_destination : XGPU_BLEND_ZERO;
	int operation = draw->blend ? draw->blend_operation : XGPU_BLEND_ADD;
	BOOL colour = (draw->color_mask & (XGPU_WRITE_RED | XGPU_WRITE_GREEN | XGPU_WRITE_BLUE)) != 0;
	BOOL additive = operation == XGPU_BLEND_ADD && (destination == XGPU_BLEND_ONE ||
		(source == XGPU_BLEND_ONE && destination == XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA));
	BOOL out_of_range = colour && (overflows(source, destination, operation, FALSE) ||
		underflows(source, destination, operation));
	unsigned short hdr = 0;
	int glow = XGPU_HDR_CEILING_NONE;

	if (draw->kind & HALO_HDR_KIND_HUD_GLOW)
		glow = XGPU_HDR_CEILING_HUD;
	else if ((draw->kind & HALO_HDR_KIND_FIRST_PERSON) &&
		((draw->kind & (HALO_HDR_KIND_NUMERIC | HALO_HDR_KIND_METER)) || additive))
		glow = XGPU_HDR_CEILING_WEAPON;
	else if ((draw->kind & HALO_HDR_KIND_LIGHT) && additive && !draw->interface)
		glow = XGPU_HDR_CEILING_LIGHT;
	if (glow != XGPU_HDR_CEILING_NONE && colour)
		hdr = XGPU_HDR_CEILING(glow);
	else if (out_of_range && (surface(draw) || underflows(source, destination, operation)))
		hdr = XGPU_HDR_CEILING(XGPU_HDR_CEILING_PAPER_WHITE);
	if ((draw->color_mask & XGPU_WRITE_ALPHA) && overflows(source, destination, operation, TRUE))
		hdr |= XGPU_HDR_ALPHA_LIMIT;
	if (XGPU_HDR_SHADER_BLENDS(hdr))
		hdr |= XGPU_HDR_BLEND(source, destination, operation);
	if (draw->dynamic_lights)
		hdr |= XGPU_HDR_LIGHTS;
	return hdr;
}

/* ---------- the shaders' GLSL

The functions both shaders share: the frame's values made linear and
encoded again (sRGB's curve, extended past 1), the shoulder, the tone map
(hdr: the mode, the peak) and the encoding (hdr: paper white, the peak, the
output: xgpu_hdr.h's XGPU_HDR_OUTPUT_*), as xgpu_hdr.h's reference has them. */
const char xgpu_hdr_functions_source[] =
	"const vec3 HDR_LUMA = vec3(0.2126, 0.7152, 0.0722);\n"
	"\n"
	"vec3 hdr_linear(vec3 value)\n"
	"{\n"
	"\tvec3 v = max(value, vec3(0.0));\n"
	"\treturn mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92, lessThanEqual(v, vec3(0.04045)));\n"
	"}\n"
	"\n"
	"vec3 hdr_encoded(vec3 light)\n"
	"{\n"
	"\tvec3 l = max(light, vec3(0.0));\n"
	"\treturn mix(1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, l * 12.92, lessThanEqual(l, vec3(0.0031308)));\n"
	"}\n"
	"\n"
	"float hdr_shoulder(float x, float knee, float peak)\n"
	"{\n"
	"\tfloat range = peak - knee;\n"
	"\tif (x <= knee)\n"
	"\t\treturn x;\n"
	"\tif (range <= 0.0)\n"
	"\t\treturn peak;\n"
	"\tfloat t = (x - knee) / range;\n"
	"\treturn knee + range * t / (1.0 + t);\n"
	"}\n"
	"\n"
	"vec3 hdr_tone_map(vec3 value, float mode, float peak)\n"
	"{\n"
	"\tvec3 v = max(value, vec3(0.0));\n"
	"\tvec3 sdr = hdr_linear(min(v, vec3(1.0)));\n"
	"\tvec3 light = hdr_linear(v);\n"
	"\tfloat sdr_luminance = dot(sdr, HDR_LUMA);\n"
	"\tfloat luminance = dot(light, HDR_LUMA);\n"
	"\tfloat gain = 1.0, knee = 1.0;\n"
	"\tif (mode > 0.5)\n"
	"\t{\n"
	"\t\tgain = 1.2 * (1.0 + 0.6 * smoothstep(0.5, 1.0, sdr_luminance));\n"
	"\t\tknee = min(1.0, 0.75 * peak);\n"
	"\t}\n"
	"\telse if (luminance <= sdr_luminance)\n"
	"\t\treturn v;\n"
	"\tfloat clipped = smoothstep(1.0, 2.0, max(v.r, max(v.g, v.b)));\n"
	"\tfloat most = max(light.r, max(light.g, light.b)) * gain;\n"
	"\tfloat hue = most > 0.0 ? gain * hdr_shoulder(most, knee, peak) / most : 0.0;\n"
	"\tfloat brightened = sdr_luminance > 0.0 ? hdr_shoulder(gain * luminance / sdr_luminance, knee, peak) : 0.0;\n"
	"\treturn hdr_encoded(mix(light * hue, sdr * brightened, clipped));\n"
	"}\n"
	"\n"
	"vec3 hdr_encode(vec3 value, vec4 hdr)\n"
	"{\n"
	"\tif (hdr.z < 0.5)\n"
	"\t\treturn clamp(value, 0.0, 1.0);\n"
	"\tvec3 light = min(hdr_linear(value), vec3(hdr.y)) * hdr.x;\n"
	"\tif (hdr.z < 2.5)\n"
	"\t\treturn light;\n"
	"\tvec3 wide = mat3(0.627404, 0.069097, 0.016391, 0.329283, 0.919540, 0.088013, 0.043313, 0.011362, 0.895595) * light;\n"
	"\tvec3 p = pow(max(wide, vec3(0.0)), vec3(0.1593017578125));\n"
	"\treturn pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), vec3(78.84375));\n"
	"}\n";

/* the tone map over a window's 3D view, in place: the pixel it draws over
read as the pass's input (Metal's programmable blending), its colour
written (the game's destination alpha kept) */
static const char tone_map_source[] =
	"layout(input_attachment_index = 0, set = 2, binding = 0) uniform subpassInput frame_input;\n"
	"/* the mode, the peak */\n"
	"uniform vec4 hdr;\n"
	"out vec4 result;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tresult = vec4(hdr_tone_map(subpassLoad(frame_input).rgb, hdr.x, hdr.y), 1.0);\n"
	"}\n";

/* the encoding of the frame for the display, as it is drawn into the
display's image: metrics the viewport's rectangle (x, y, 1 / width, 1 /
height), the frame scaled over it */
static const char encode_source[] =
	"uniform sampler2D color_texture;\n"
	"/* the viewport: x, y, 1 / width, 1 / height */\n"
	"uniform vec4 metrics;\n"
	"/* paper white, the peak, the output */\n"
	"uniform vec4 hdr;\n"
	"out vec4 result;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec2 position = (gl_FragCoord.xy - metrics.xy) * metrics.zw;\n"
	"\tresult = vec4(hdr_encode(textureLod(color_texture, position, 0.0).rgb, hdr), 1.0);\n"
	"}\n";

/* (the generators read them out of this file's text; nothing here draws
with them) */
const char *const xgpu_hdr_programs[2] = { tone_map_source, encode_source };
