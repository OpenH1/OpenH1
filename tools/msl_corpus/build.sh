#!/bin/sh
# build.sh <source tree> <build dir>: builds msl_corpus (msl_corpus.c) from
# a source tree's shader decoders and Metal emitter, natively, with the
# stand-in platform header in stub/ (the files are copied away from the
# tree's own platform.h, which their quoted includes would find first)
set -e
here=$(cd "$(dirname "$0")" && pwd)
tree=$1
build=$2
src=$tree/port/linux/src
mkdir -p "$build/src"
for file in xgpu.h xgpu_backend.h xgpu_hdr.h xgpu_shader.h xgpu_msl.h nv2a_vsh.c nv2a_psh.c xgpu_text.c xgpu_msl.c; do
	cp "$src/$file" "$build/src/"
done
# the XDK's enumerations (D3DRS_*, D3DCMP_*, D3DFOG_*, PS_*)
sed -n '/^\/\* ---------- enumerations/,/^\/\* ---------- structures and unions/p' \
	"$tree/port/include/xdk/xdk_pdb.h" | sed '$d' > "$build/xdk_enums.h"
${CC:-cc} -std=gnu99 -O1 -Wall -I"$build" -I"$here/stub" -I"$build/src" -o "$build/msl_corpus" "$here/msl_corpus.c" \
	"$build/src/nv2a_vsh.c" "$build/src/nv2a_psh.c" "$build/src/xgpu_text.c" "$build/src/xgpu_msl.c" -lm
