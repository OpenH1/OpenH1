# The SPIR-V shaders' corpus

The SPIR-V emitter (`port/linux/src/xgpu_spirv.c`) writes the NV2A's programs
as SPIR-V modules for the Vulkan backend. `spirv_corpus` writes the same
variants as the Metal shaders' corpus (`tools/msl_corpus`), one `.spv` file
each:

- every captured vertex program, from immediate mode (all floats) and from
  streams (its NORMPACKED3 inputs), unlit, lit for the enhanced dynamic
  lights and lit for each pixel; and from streams with its other inputs
  unnormalized shorts;
- every captured pixel shader key, flat (the VR mode's views drawn as none),
  and again counting its samples in the shader (where it is a visibility
  test's) and drawing its border colors in the shader (where it samples a 2D
  or 3D texture);
- `msl_corpus`'s programs made up to use every part of the intermediate form
  (`tools/msl_corpus/synthetic.c`).

Identical modules are written once; `index.txt` names each variant's file.
The captures are what the renderer's test capture writes
(`HALO_TEST_CAPTURE`): `vsNNN.bin` and `ps_XXXXXXXX.key`, as for `msl_corpus`.

On Linux, with SPIRV-Tools' `spirv-val`:

    tools/spirv_corpus/build.sh . build/spirv_corpus
    build/spirv_corpus/spirv_corpus <capture dir> <corpus dir>
    sh tools/spirv_corpus/check.sh <corpus dir> <log>

`check.sh` validates each module for Vulkan 1.1, logs every message and
exits nonzero if any module fails.
