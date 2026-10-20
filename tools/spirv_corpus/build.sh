#!/bin/sh
# build.sh <source tree> <build dir>: builds spirv_corpus (spirv_corpus.c)
# from a source tree's shader decoders and SPIR-V emitter, natively, with
# msl_corpus's stand-in platform header and made-up programs
# (tools/msl_corpus: stub/, synthetic.c; the files are copied away from the
# tree's own platform.h, which their quoted includes would find first)
set -e
here=$(cd "$(dirname "$0")" && pwd)
tree=$1
build=$2
src=$tree/port/linux/src
mkdir -p "$build/src"
for file in xgpu.h xgpu_backend.h xgpu_hdr.h xgpu_shader.h xgpu_spirv.h nv2a_vsh.c nv2a_psh.c xgpu_text.c xgpu_spirv.c; do
	cp "$src/$file" "$build/src/"
done
# the XDK's enumerations (D3DRS_*, D3DCMP_*, D3DFOG_*, PS_*)
sed -n '/^\/\* ---------- enumerations/,/^\/\* ---------- structures and unions/p' \
	"$tree/port/include/xdk/xdk_pdb.h" | sed '$d' > "$build/xdk_enums.h"
# (and anti-aliasing's passes, generated: tools/vulkan_post_shaders.py)
${CC:-cc} -std=gnu99 -O1 -Wall -I"$build" -I"$here/../msl_corpus/stub" -I"$here/../msl_corpus" -I"$build/src" \
	-I"$tree/port/linux/vulkan" -o "$build/spirv_corpus" "$here/spirv_corpus.c" \
	"$build/src/nv2a_vsh.c" "$build/src/nv2a_psh.c" "$build/src/xgpu_text.c" "$build/src/xgpu_spirv.c" \
	"$tree/port/linux/vulkan/xgpu_vulkan_post_shaders.c" -lm
