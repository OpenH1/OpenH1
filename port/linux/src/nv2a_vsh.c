/*
NV2A_VSH.C

Decoding of NV2A vertex programs (Xbox vertex shader microcode) into the
shader intermediate form (xgpu_shader.h), and the analysis of the game's
model lighting programs on it; xgpu_spirv.c and xgpu_msl.c write the
shaders.

Each instruction is four little-endian words. Word 1 to 3 hold a MAC
(vector) operation and an ILU (scalar) operation that execute in parallel
on the same three operands A, B and C, and the destinations of both; the
bit positions of every field are spelled out in operand_decode() and in
nv2a_vertex_program_decode(). When both units run, the ILU result goes to
temporary r1, whatever the instruction's temporary register field says.

Xbox vertex programs finish by converting their clip-space position to
screen space with the viewport constants c[-38] and c[-37], which Direct3D
maintains. The generated shader inverts that transform to hand the GPU a
clip-space position again.
*/

#include "xgpu.h"
#include "xgpu_shader.h"

#include <stdlib.h>
#include <string.h>

/* ---------- decoding */

static unsigned long field(const DWORD *instruction, int word, int low_bit, int bit_count)
{
	return (instruction[word] >> low_bit) & ((1UL << bit_count) - 1);
}

/* the hardware's operand sources (the mux fields) */
enum
{
	_mux_unknown, _mux_temporary, _mux_input, _mux_constant,
};

/* output register addresses (o[]) */
static unsigned char output_register(unsigned long address)
{
	switch (address)
	{
	case 0: return XGPU_OUTPUT_POSITION;
	case 3: return XGPU_OUTPUT_DIFFUSE;
	case 4: return XGPU_OUTPUT_SPECULAR;
	case 5: return XGPU_OUTPUT_FOG;
	case 6: return XGPU_OUTPUT_POINT_SIZE;
	case 7: return XGPU_OUTPUT_BACK_DIFFUSE;
	case 8: return XGPU_OUTPUT_BACK_SPECULAR;
	case 9: return XGPU_OUTPUT_TEXTURE0;
	case 10: return XGPU_OUTPUT_TEXTURE1;
	case 11: return XGPU_OUTPUT_TEXTURE2;
	case 12: return XGPU_OUTPUT_TEXTURE3;
	default: return XGPU_OUTPUT_UNUSED;
	}
}

static void operand_decode(const DWORD *instruction, char which, struct xgpu_vertex_operand *operand)
{
	unsigned long index, mux;

	switch (which)
	{
	case 'A':
		operand->negate = (unsigned char)field(instruction, 1, 8, 1);
		operand->swizzle[0] = (unsigned char)field(instruction, 1, 6, 2);
		operand->swizzle[1] = (unsigned char)field(instruction, 1, 4, 2);
		operand->swizzle[2] = (unsigned char)field(instruction, 1, 2, 2);
		operand->swizzle[3] = (unsigned char)field(instruction, 1, 0, 2);
		index = field(instruction, 2, 28, 4);
		mux = field(instruction, 2, 26, 2);
		break;
	case 'B':
		operand->negate = (unsigned char)field(instruction, 2, 25, 1);
		operand->swizzle[0] = (unsigned char)field(instruction, 2, 23, 2);
		operand->swizzle[1] = (unsigned char)field(instruction, 2, 21, 2);
		operand->swizzle[2] = (unsigned char)field(instruction, 2, 19, 2);
		operand->swizzle[3] = (unsigned char)field(instruction, 2, 17, 2);
		index = field(instruction, 2, 13, 4);
		mux = field(instruction, 2, 11, 2);
		break;
	default:
		operand->negate = (unsigned char)field(instruction, 2, 10, 1);
		operand->swizzle[0] = (unsigned char)field(instruction, 2, 8, 2);
		operand->swizzle[1] = (unsigned char)field(instruction, 2, 6, 2);
		operand->swizzle[2] = (unsigned char)field(instruction, 2, 4, 2);
		operand->swizzle[3] = (unsigned char)field(instruction, 2, 2, 2);
		index = (field(instruction, 2, 0, 2) << 2) | field(instruction, 3, 30, 2);
		mux = field(instruction, 3, 28, 2);
		break;
	}
	switch (mux)
	{
	case _mux_temporary:
		operand->file = XGPU_VERTEX_TEMPORARY;
		operand->index = (unsigned char)index;
		break;
	case _mux_input:
		/* (one input an instruction, whichever operands read it) */
		operand->file = XGPU_VERTEX_INPUT;
		operand->index = (unsigned char)field(instruction, 1, 9, 4);
		break;
	case _mux_constant:
		operand->file = XGPU_VERTEX_CONSTANT;
		operand->index = (unsigned char)field(instruction, 1, 13, 8);
		break;
	default:
		operand->file = XGPU_VERTEX_NONE;
		operand->index = 0;
		break;
	}
}

static unsigned long long hash_words(const DWORD *words, unsigned long count)
{
	unsigned long long hash = 14695981039346656037ULL;
	unsigned long index;

	for (index = 0; index < count; index++)
		hash = (hash ^ words[index]) * 1099511628211ULL;
	return hash;
}

void nv2a_vertex_program_decode(const DWORD *instructions, unsigned long instruction_count,
	struct xgpu_vertex_program *program)
{
	unsigned long index;

	memset(program, 0, sizeof(*program));
	if (instruction_count > XGPU_VERTEX_PROGRAM_MAXIMUM)
		instruction_count = XGPU_VERTEX_PROGRAM_MAXIMUM;
	program->hash = hash_words(instructions, instruction_count * 4);
	for (index = 0; index < instruction_count; index++)
	{
		const DWORD *words = instructions + index * 4;
		struct xgpu_vertex_instruction *instruction = &program->instructions[index];

		instruction->mac = (unsigned char)field(words, 1, 21, 4);
		instruction->ilu = (unsigned char)field(words, 1, 25, 3);
		operand_decode(words, 'A', &instruction->a);
		operand_decode(words, 'B', &instruction->b);
		operand_decode(words, 'C', &instruction->c);
		instruction->relative = (unsigned char)field(words, 3, 1, 1);
		instruction->mac_mask = (unsigned char)field(words, 3, 24, 4);
		instruction->mac_temporary = (unsigned char)field(words, 3, 20, 4);
		instruction->ilu_mask = (unsigned char)field(words, 3, 16, 4);
		/* (the ILU writes r1 when both units run) */
		instruction->ilu_temporary = instruction->mac != XGPU_MAC_NOP ? 1 : instruction->mac_temporary;
		instruction->output_mask = (unsigned char)field(words, 3, 12, 4);
		instruction->output_is_register = (unsigned char)field(words, 3, 11, 1);
		instruction->output_address = (unsigned char)field(words, 3, 3, 8);
		instruction->output = output_register(instruction->output_address);
		instruction->output_from_ilu = (unsigned char)field(words, 3, 2, 1);
		/* (writes to constant memory are not used by Halo's programs) */
		instruction->writes_output = instruction->output_mask && instruction->output_is_register &&
			(instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0;
		instruction->captures_clip = instruction->ilu == XGPU_ILU_RCC &&
			instruction->c.file == XGPU_VERTEX_TEMPORARY && instruction->c.index == XGPU_VERTEX_POSITION;
		instruction->last = (unsigned char)field(words, 3, 0, 1);
		if (instruction->captures_clip)
			program->projects = TRUE;
		program->instruction_count = index + 1;
		if (instruction->last)
			break;
	}
}

/* ---------- model lighting

The game's model lighting programs (rasterizer_xbox_vertex_shaders_data.inc
9, 10, 17 and 27) sum into the diffuse color oD0.xyz the ambient light
c[-69], two distant lights (direction c[-73] and c[-71], color c[-72] and
c[-70]; the first also lights the back by the translucency c[-82].z) and, in
10 and 17, two point lights (c[-79] to c[-77] and c[-76] to c[-74]: position
and 1 / radius squared, the cone's axis and falloff scale, the color and
falloff offset). The normal and world position they light by are
temporaries that the programs reuse (10 and 17 overwrite both after the
sum). In 9, 10 and 17 the normal is the skinned normal turned by c[-84].w
for the back faces' pass, r0 (written by instruction 17) and the position
the skinned position, r10 (by 12); in 27, of a single node, the normal is
r3 (by 5). They are taken as the first instruction that lights by them
reads them: in 9 the normal before instruction 19, in 10 and 17 the normal
before 24 and the position before 19, in 27 the normal before 7. A program
is taken only where its lighting is the sum xgpu_spirv.c computes for each
pixel (the diffuse color written once, by the sum; both distant lights by
one normal; the translucency; each point light by one position, its cone
by its axis reversed) and the registers keep the same normal and position
through it. Nothing else of the programs goes into oD0.xyz; oD0.w is 0, or
in 17 the planar fog's. */

static BOOL xyz_unswizzled(const struct xgpu_vertex_operand *operand)
{
	return operand->swizzle[0] == 0 && operand->swizzle[1] == 1 && operand->swizzle[2] == 2;
}

/* the operand is c[reg] (as the game numbers them: -73), its xyz unswizzled */
static BOOL light_constant(const struct xgpu_vertex_instruction *instruction, const struct xgpu_vertex_operand *operand,
	int reg, BOOL negated)
{
	return operand->file == XGPU_VERTEX_CONSTANT && !instruction->relative &&
		operand->index == (unsigned long)(XGPU_VERTEX_CONSTANT_BIAS + reg) &&
		operand->negate == (negated ? 1 : 0) && xyz_unswizzled(operand);
}

/* the operand is a temporary (not the position output), its xyz unswizzled */
static BOOL light_temporary(const struct xgpu_vertex_operand *operand, BOOL negated)
{
	return operand->file == XGPU_VERTEX_TEMPORARY && operand->index < XGPU_VERTEX_POSITION &&
		operand->negate == (negated ? 1 : 0) && xyz_unswizzled(operand);
}

/* the constant the instruction reads (all its constant operands read the
same one), or -1 if it reads none */
static long instruction_constant(const struct xgpu_vertex_instruction *instruction)
{
	if (instruction->a.file == XGPU_VERTEX_CONSTANT)
		return instruction->a.index;
	if (instruction->b.file == XGPU_VERTEX_CONSTANT)
		return instruction->b.index;
	if (instruction->c.file == XGPU_VERTEX_CONSTANT)
		return instruction->c.index;
	return -1;
}

/* any of x, y and z of temporary reg written by the instructions in [first, last) */
static BOOL temporary_written(const struct xgpu_vertex_program *program, unsigned long first, unsigned long last,
	unsigned long reg)
{
	unsigned long index;

	for (index = first; index < last; index++)
	{
		const struct xgpu_vertex_instruction *instruction = &program->instructions[index];

		if (instruction->mac != XGPU_MAC_NOP && instruction->mac != XGPU_MAC_ARL &&
			instruction->mac_temporary == reg && (instruction->mac_mask & 0xe))
		{
			return TRUE;
		}
		if (instruction->ilu != XGPU_ILU_NOP && instruction->ilu_temporary == reg && (instruction->ilu_mask & 0xe))
			return TRUE;
	}
	return FALSE;
}

BOOL nv2a_vertex_shader_lighting(const struct xgpu_vertex_program *program, struct nv2a_vertex_lighting *lighting)
{
	unsigned long count = program->instruction_count, index, diffuse;
	unsigned long distant[2], distant_register[2] = { 0, 0 }, point[2], point_register[2] = { 0, 0 };
	BOOL cone[2] = { FALSE, FALSE }, translucency = FALSE, point_lights = FALSE;
	int light;

	diffuse = distant[0] = distant[1] = point[0] = point[1] = count;
	for (index = 0; index < count; index++)
	{
		const struct xgpu_vertex_instruction *instruction = &program->instructions[index];
		const struct xgpu_vertex_operand *a = &instruction->a, *b = &instruction->b, *c = &instruction->c;
		unsigned long mac = instruction->mac, output_mask = instruction->output_mask;
		long constant = instruction_constant(instruction);

		/* the diffuse color's x, y and z, written once and last by the sum:
		the second distant light's term times its color, plus the rest */
		if ((output_mask & 0xe) && instruction->output_is_register && instruction->output_address == 3)
		{
			if (diffuse != count || (output_mask & 0xe) != 0xe || instruction->output_from_ilu ||
				mac != XGPU_MAC_MAD || !light_constant(instruction, b, -70, FALSE) || c->file != XGPU_VERTEX_TEMPORARY)
			{
				return FALSE;
			}
			diffuse = index;
		}
		for (light = 0; light < 2; light++)
		{
			/* each distant light's facing: the normal's dot product with the
			light's direction reversed */
			if (mac == XGPU_MAC_DP3 && light_constant(instruction, b, light ? -71 : -73, TRUE))
			{
				if (distant[light] != count || !light_temporary(a, FALSE))
					return FALSE;
				distant[light] = index;
				distant_register[light] = a->index;
			}
			/* each point light's direction, from the world position */
			if (mac == XGPU_MAC_ADD && light_constant(instruction, a, light ? -76 : -79, FALSE) &&
				light_temporary(c, TRUE))
			{
				if (point[light] != count)
					return FALSE;
				point[light] = index;
				point_register[light] = c->index;
			}
			/* and its cone: the direction's dot product with the light's axis
			reversed */
			if (mac == XGPU_MAC_DP3 && b->file == XGPU_VERTEX_CONSTANT && !instruction->relative &&
				constant == XGPU_VERTEX_CONSTANT_BIAS + (light ? -75 : -78))
			{
				if (!light_constant(instruction, b, light ? -75 : -78, TRUE))
					return FALSE;
				cone[light] = TRUE;
			}
		}
		/* the back lit by the first distant light: its facing reversed, times
		the translucency */
		if (mac == XGPU_MAC_MUL && a->file == XGPU_VERTEX_TEMPORARY && a->negate && b->file == XGPU_VERTEX_CONSTANT &&
			!instruction->relative && constant == XGPU_VERTEX_CONSTANT_BIAS - 82 && b->swizzle[0] == 2)
		{
			translucency = TRUE;
		}
		if (constant >= 0 && !instruction->relative && constant >= XGPU_VERTEX_CONSTANT_BIAS - 79 &&
			constant <= XGPU_VERTEX_CONSTANT_BIAS - 74)
		{
			point_lights = TRUE;
		}
	}

	if (diffuse == count || distant[0] >= diffuse || distant[1] >= diffuse ||
		distant_register[0] != distant_register[1] || !translucency)
	{
		return FALSE;
	}
	/* the world position the clip position's x is computed from (with c[-96],
	the first row of the world's projection), for the enhanced dynamic
	lights: a temporary (the skinned position) */
	lighting->world_found = FALSE;
	for (index = 0; index < count; index++)
	{
		const struct xgpu_vertex_instruction *instruction = &program->instructions[index];
		unsigned long mac = instruction->mac;

		/* (written to the position's temporary, r12, or through the output
		register) */
		if ((mac == XGPU_MAC_DP4 || mac == XGPU_MAC_DPH) &&
			((instruction->mac_temporary == XGPU_VERTEX_POSITION && (instruction->mac_mask & 0x8)) ||
				((instruction->output_mask & 0x8) && instruction->output_is_register &&
					instruction->output_address == 0 && !instruction->output_from_ilu)) &&
			instruction->b.file == XGPU_VERTEX_CONSTANT && !instruction->relative &&
			instruction->b.index == XGPU_VERTEX_CONSTANT_BIAS - 96 && light_temporary(&instruction->a, FALSE))
		{
			lighting->world_found = TRUE;
			lighting->world_instruction = index;
			lighting->world_register = instruction->a.index;
			break;
		}
	}
	lighting->lights = 1;
	lighting->normal_register = distant_register[0];
	lighting->normal_instruction = distant[0] < distant[1] ? distant[0] : distant[1];
	if (temporary_written(program, lighting->normal_instruction, diffuse, lighting->normal_register))
		return FALSE;
	lighting->position_instruction = lighting->position_register = 0;
	if (point_lights)
	{
		unsigned long last = point[0] > point[1] ? point[0] : point[1];

		if (point[0] >= diffuse || point[1] >= diffuse || point_register[0] != point_register[1] ||
			!cone[0] || !cone[1])
		{
			return FALSE;
		}
		lighting->lights = 2;
		lighting->position_register = point_register[0];
		lighting->position_instruction = point[0] < point[1] ? point[0] : point[1];
		if (temporary_written(program, lighting->position_instruction, last, lighting->position_register))
			return FALSE;
	}
	return TRUE;
}
