/*
XGPU_METAL_COMBINER.H

The NV2A's pixel stage as one Metal fragment function made ahead of time
(xgpu_metal_combiner.c): the texture stages and the register combiners (up
to eight general stages and the final combiner) written once, whose
configuration (what the MSL emitter's shaders are specialised on: struct
xgpu_pixel_program) is the function's constants. Specialised for a pixel
program (newFunctionWithName:constantValues:), it draws that program with no
MSL compiled: the backend's way to draw a program the app's library lacks
at once, while that program's own shader compiles in the background (its
own draws faster: Metal's specialisation does not optimise the function
again as a compile of the program would).

Texture stage n's texture is at its type's place: a 2D texture at
XGPU_METAL_COMBINER_TEXTURE_2D + n (the emitter's shaders' place too), a
3D one, a cube and a layered one past it. Its sampler is sampler n.
*/

#ifndef __HALO_MACOS_XGPU_METAL_COMBINER_H
#define __HALO_MACOS_XGPU_METAL_COMBINER_H

#include <stdint.h>

struct xgpu_pixel_program;

#define XGPU_METAL_COMBINER_TEXTURE_2D 0
#define XGPU_METAL_COMBINER_TEXTURE_3D 8
#define XGPU_METAL_COMBINER_TEXTURE_CUBE 12
#define XGPU_METAL_COMBINER_TEXTURE_2D_ARRAY 16
/* the configuration, packed (160 bytes): its words are the function's
constants (xgpu_metal_combiner_constants) */
struct xgpu_metal_combiner
{
	/* each texture stage: mode (5 bits), sampler type (3), input (2),
	previous + 1 (2), dot mapping (3), compare (4), colour sign (4), alpha
	kill (1), the axes whose border the shader draws (3) */
	uint32_t stages[4];
	/* the general combiners' count (4 bits), the mux's bit (1), fog (1),
	fog mode (3), the alpha test's function (4), coverage alpha (1), point
	threshold (1), source blend (3), the final combiner present (1), its
	sum clamped (1), v1 inverted (1), r0 inverted (1) */
	uint32_t flags;
	/* the final combiner's inputs A to G, 8 bits each: register (4), alpha
	(1), mapping (3) */
	uint32_t final_inputs[2];
	uint32_t pad;
	struct
	{
		/* [0] rgb, [1] alpha: the inputs A to D, 8 bits each as above */
		uint32_t inputs[2];
		/* [n]: A.B a dot product (1), C.D one (1), the sum a mux (1), the
		output mapping (3), the registers written by A.B (4), C.D (4) and
		the sum (4), A.B's blue to alpha (1), C.D's (1); [1] also c0's
		constant (3) and c1's (3) from bit 24 */
		uint32_t control[2];
	} combiners[8];
};

/* whether a program is the combiner function's to draw (not one lit for
each pixel, nor with the enhanced dynamic lights, nor whose alpha test
covers samples in proportion, nor display.hdr's tagged draws: those have
inputs or outputs of their own) */
int xgpu_metal_combiner_draws(const struct xgpu_pixel_program *program);
/* a program's configuration, with the axes whose border the shader draws
(xgpu_metal_stream_variant's) */
void xgpu_metal_combiner_configure(const struct xgpu_pixel_program *program, const uint8_t border[4],
	struct xgpu_metal_combiner *combiner);

/* the configuration as the function's constants, in their order: 0-3 the
texture stages, 4 the flags, 5-6 the final combiner's inputs, 7 + 4n the
general combiner n's inputs (rgb, alpha) and controls (rgb, alpha) */
#define XGPU_METAL_COMBINER_CONSTANTS 39
void xgpu_metal_combiner_constants(const struct xgpu_metal_combiner *combiner,
	uint32_t constants[XGPU_METAL_COMBINER_CONSTANTS]);

/* the function's source (its entry: xgpu_combiner_fragment) */
extern const char xgpu_metal_combiner_source[];

#endif
