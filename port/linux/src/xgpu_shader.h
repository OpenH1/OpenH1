/*
XGPU_SHADER.H

The shader intermediate form: the NV2A's programs decoded once, every bit
field resolved, for the emitters to write in a shading language (GLSL and
ESSL: xgpu_glsl.c). nv2a_vsh.c decodes vertex programs.

The form is the hardware's own operations, typed: these programs have no
control flow, a few dozen kinds of operation, and each kind has a spelling
in every target language, so an emitter is a walk over the form and a table
of spellings.
*/

#ifndef __HALO_LINUX_XGPU_SHADER_H
#define __HALO_LINUX_XGPU_SHADER_H

/* ---------- vertex programs */

#define XGPU_VERTEX_PROGRAM_MAXIMUM 136

/* the MAC (vector) unit's operations, as the hardware numbers them */
enum
{
	XGPU_MAC_NOP, XGPU_MAC_MOV, XGPU_MAC_MUL, XGPU_MAC_ADD, XGPU_MAC_MAD, XGPU_MAC_DP3, XGPU_MAC_DPH, XGPU_MAC_DP4,
	XGPU_MAC_DST, XGPU_MAC_MIN, XGPU_MAC_MAX, XGPU_MAC_SLT, XGPU_MAC_SGE, XGPU_MAC_ARL,
};

/* the ILU (scalar) unit's */
enum
{
	XGPU_ILU_NOP, XGPU_ILU_MOV, XGPU_ILU_RCP, XGPU_ILU_RCC, XGPU_ILU_RSQ, XGPU_ILU_EXP, XGPU_ILU_LOG, XGPU_ILU_LIT,
};

/* where an operand is read from */
enum
{
	XGPU_VERTEX_NONE,
	/* r0 to r11, and r12, which reads back the position output */
	XGPU_VERTEX_TEMPORARY,
	XGPU_VERTEX_INPUT,
	XGPU_VERTEX_CONSTANT,
};

/* the temporary that is the position output */
#define XGPU_VERTEX_POSITION 12

/* the output registers (o[]) */
enum
{
	XGPU_OUTPUT_POSITION, XGPU_OUTPUT_DIFFUSE, XGPU_OUTPUT_SPECULAR, XGPU_OUTPUT_FOG, XGPU_OUTPUT_POINT_SIZE,
	XGPU_OUTPUT_BACK_DIFFUSE, XGPU_OUTPUT_BACK_SPECULAR,
	XGPU_OUTPUT_TEXTURE0, XGPU_OUTPUT_TEXTURE1, XGPU_OUTPUT_TEXTURE2, XGPU_OUTPUT_TEXTURE3,
	XGPU_OUTPUT_UNUSED,
};

struct xgpu_vertex_operand
{
	/* XGPU_VERTEX_* */
	unsigned char file;
	/* the temporary (to 15: past 12 the hardware has none), the input, or
	the constant (the instruction's: every constant operand of an
	instruction reads the same one) */
	unsigned char index;
	/* x, y, z, w: 0 to 3 */
	unsigned char swizzle[4];
	unsigned char negate;
};

struct xgpu_vertex_instruction
{
	/* XGPU_MAC_* and XGPU_ILU_* (the values past them do nothing but
	write 0) */
	unsigned char mac, ilu;
	struct xgpu_vertex_operand a, b, c;
	/* the constant operands are c[a0 + index] */
	unsigned char relative;
	/* the units' destinations: masks with bit 3 x ... bit 0 w, and the
	temporary each writes (XGPU_VERTEX_POSITION, the position): the ILU's
	is r1 when both units run */
	unsigned char mac_mask, mac_temporary;
	unsigned char ilu_mask, ilu_temporary;
	/* the output: its mask, whether it is an output register (else the
	constant memory, which Halo's programs do not write), the register
	(XGPU_OUTPUT_*; output_address as the hardware numbers it), and which
	unit feeds it */
	unsigned char output_mask, output_is_register, output, output_address, output_from_ilu;
	/* the output is written: a register, by a unit that runs */
	unsigned char writes_output;
	/* the screen conversion's reciprocal of the position's w (an rcc of
	r12.w): the clip position is what r12 holds before it */
	unsigned char captures_clip;
	/* the program's last */
	unsigned char last;
};

struct xgpu_vertex_program
{
	unsigned long instruction_count;
	struct xgpu_vertex_instruction instructions[XGPU_VERTEX_PROGRAM_MAXIMUM];
	/* of the microcode: the program's identity from run to run */
	unsigned long long hash;
	/* an instruction captures the clip position (the program projects
	through clip space) */
	unsigned char projects;
};

/* decodes NV2A vertex program microcode (the instruction words after the
program header, four each) */
void nv2a_vertex_program_decode(const DWORD *instructions, unsigned long instruction_count,
	struct xgpu_vertex_program *program);

#endif
