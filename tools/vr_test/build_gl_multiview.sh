#!/bin/sh
# Build the asset-free GPU check beside a configured SDL3 host dependency.
# Usage: build_gl_multiview.sh <build directory> <SDL3 host build directory>
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tree=$(CDPATH= cd -- "$here/../.." && pwd)
build=$1
sdl=$2
mkdir -p "$build/src"
# Copy away from the guest's platform.h, as the emitter corpus tools do.
for file in xgpu.h xgpu_shader.h xgpu_gl.h xgpu_glsl.c xgpu_text.c; do
	cp "$tree/port/linux/src/$file" "$build/src/"
done
sed -n '/^\/\* ---------- enumerations/,/^\/\* ---------- structures and unions/p' \
	"$tree/port/include/xdk/xdk_pdb.h" | sed '$d' > "$build/xdk_enums.h"
# The standalone check links standard GL entry points directly.
printf '#define GL_GLEXT_PROTOTYPES\n#include <GL/glcorearb.h>\n' > "$build/src/gl.h"
${CC:-cc} -std=gnu11 -O1 -Wall -Wextra -Werror \
	-I"$build" -I"$tree/tools/msl_corpus/stub" -I"$build/src" \
	-I"$sdl/third_party/SDL3/include" "$here/gl_multiview.c" \
	"$build/src/xgpu_glsl.c" "$build/src/xgpu_text.c" \
	-L"$sdl" -l:libSDL3.so.0 -lGL -lm -o "$build/gl_multiview"
