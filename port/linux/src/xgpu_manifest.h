/*
XGPU_MANIFEST.H

The renderer's manifest: every shader and pipeline the game draws with, as
the shared front end (d3d8_device.c) describes them to a backend, so that a
backend can build them ahead of time instead of at a draw. It is a property of
the game, not of a backend: it holds the shader IR's inputs (the NV2A
vertex programs' microcode, the pixel shader keys) and the pipeline keys
(xgpu_stream.h's fixed-width form of xgpu_backend.h's), with the vertex
layout and the texture stages' sampling that each draw of a key has. No
textures, geometry, names or other map content: a pixel shader key is the
combiner registers and render states the game's code set from its tags.

The front end records it (debug.render_manifest: a file each new draw
configuration is added to); tools/render_manifest.py merges recordings into
the committed one (port/assets/render/manifest.txt), whose README says how
it is made and checked. Any backend reads it with xgpu_manifest_read.

The file is text, a record a line ('#' starts a comment):

	manifest 1
	v <hash> <instructions> <microcode>
	p <hash> <field>=<value> ...
	d <field>=<value> ...
	s vp=<hash> pa=<mask> l=<layout> w=<strides>

v: a vertex program, named by its microcode's hash (xgpu_shader.h's,
sixteen hex digits), its instruction count and its words (eight hex digits
each, four an instruction, without spaces).
p: a pixel shader key (xgpu_stream_pixel_key), named by its hash
(xgpu_manifest_hash of its bytes): cs=<register>:<value>,... the combiner
states that are not 0, then tm (the texture modes), sa (the samplers'
types), ak (alpha kill), sg (colour sign), at (the alpha test's function),
fe, ft, cn, ca, pt, pl, as, mv, sb, dl (fog enabled, fog table mode, count
samples, coverage alpha, point threshold, per-pixel lighting, alpha test
samples, multiview, source blend, dynamic lights).
d: a draw's configuration: its pipeline key's fields (vp the vertex
program, pa packed attributes, im immediate, li lighting, vw views, bl
blend enabled/source/destination/operation, cm colour mask, dt depth
test/function/write, st stencil test/function/fail/depth fail/pass, sm
stencil read/write masks, cu cull/counter-clockwise/front, fi fill, db
depth bias, fm colour format/depth format/samples), px its pixel key's
hash; its vertex layout: l=<reg>.<stream>.<format>.<bytes>.<offset>/...
(xgpu_vertex_element) and w=<stream>:<stride>,... (the streams the
elements read), or i=<registers written> for immediate mode; and its
texture stages: t=<source>.<sampler type>.<address u>.<v>.<w>.<border
colour>/... (four; the border colour only where an axis is clamped to it,
else 0).

s: a vertex shader as the game creates it: its program and its
declaration's layout (fields as a draw's of the same names), whichever
draws use it. These come from the game's code, not from play:
tools/render_manifest.py vertex writes them from the decompilation's
table (rasterizer_xbox_vertex_shaders.c and _initialize.c).
*/

#ifndef __HALO_XGPU_MANIFEST_H
#define __HALO_XGPU_MANIFEST_H

#include "xgpu_stream.h"

#include <stddef.h>
#include <stdint.h>

#define XGPU_MANIFEST_VERSION 1
#define XGPU_MANIFEST_ATTRIBUTES 16
#define XGPU_MANIFEST_STREAMS 16
#define XGPU_MANIFEST_MICROCODE_WORDS (136 * 4)

struct xgpu_manifest_vertex_program
{
	uint64_t hash;
	uint32_t instruction_count;
	uint32_t words[XGPU_MANIFEST_MICROCODE_WORDS];
};

struct xgpu_manifest_element
{
	uint8_t reg, stream, format, bytes;
	uint16_t offset;
};

/* a texture stage's sampling: XGPU_SOURCE_*, _xgpu_sampler_*,
XGPU_ADDRESS_* for u, v and w, the border colour (ARGB) where an axis is
XGPU_ADDRESS_BORDER, else 0 */
struct xgpu_manifest_stage
{
	uint8_t source, sampler_type, address[3];
	uint32_t border_color;
};

struct xgpu_manifest_draw
{
	struct xgpu_stream_pipeline_key key;
	/* immediate mode: the registers written; else the layout */
	uint32_t immediate_written;
	uint32_t element_count;
	struct xgpu_manifest_element elements[XGPU_MANIFEST_ATTRIBUTES];
	uint16_t strides[XGPU_MANIFEST_STREAMS];
	struct xgpu_manifest_stage stages[4];
};

struct xgpu_manifest
{
	struct xgpu_manifest_vertex_program *vertex_programs;
	uint32_t vertex_program_count;
	struct xgpu_manifest_draw *draws;
	uint32_t draw_count;
	/* the vertex shaders as the game creates them (s records): each a
	draw's vertex program, packed attributes and layout alone */
	struct xgpu_manifest_draw *vertex_shaders;
	uint32_t vertex_shader_count;
};

/* FNV-1a, 64 bits: a pixel key's name, a draw's identity */
uint64_t xgpu_manifest_hash(const void *data, size_t size, uint64_t hash);
#define XGPU_MANIFEST_HASH_START 14695981039346656037ULL

/* reads a manifest; returns 1, or 0 with a message in error (the line) */
int xgpu_manifest_read(const char *path, struct xgpu_manifest *manifest, char *error, size_t error_size);
void xgpu_manifest_free(struct xgpu_manifest *manifest);
/* the vertex program a key names, or NULL */
const struct xgpu_manifest_vertex_program *xgpu_manifest_vertex_program(const struct xgpu_manifest *manifest,
	uint64_t hash);

/* ---------- the recorder (the front end's; debug.render_manifest) */

#ifndef XGPU_MANIFEST_READER_ONLY

struct xgpu_pipeline_key;
struct xgpu_texture_source;
struct xgpu_vertex_input;

/* whether draws are recorded (read once: debug.render_manifest) */
int xgpu_manifest_recording(void);
/* a vertex program's microcode as it is made (the words after its header) */
void xgpu_manifest_note_vertex_program(unsigned long long hash, const uint32_t *words, unsigned long instruction_count);
/* the next draw's textures, pipeline and vertices, then the draw */
/* (the four stages) */
void xgpu_manifest_note_textures(const struct xgpu_texture_source *stages);
void xgpu_manifest_note_pipeline(const struct xgpu_pipeline_key *key);
void xgpu_manifest_note_vertices(const struct xgpu_vertex_input *input);
void xgpu_manifest_record_draw(int immediate, unsigned long written);
/* a comment line in the recording (the draws after it are those recorded
since: a map's play's, after its first frame's) */
void xgpu_manifest_note(const char *text);

#endif

#endif
