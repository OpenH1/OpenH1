# The Metal shaders' corpus

The Metal emitter (`port/linux/src/xgpu_msl.c`) writes the NV2A's programs as
Metal Shading Language; Apple's compiler runs only on macOS, so the shaders
are written here, on any machine, and compiled there.

`msl_corpus` writes, one `.metal` file each:

- every captured vertex program, from immediate mode (all floats) and from
  streams (its NORMPACKED3 inputs), unlit, lit for the enhanced dynamic
  lights and lit for each pixel; and from streams with its other inputs
  unnormalized shorts;
- every captured pixel shader key, flat (the VR mode's views drawn as none),
  and again counting its samples in the shader (where it is a visibility
  test's) and drawing its border colors in the shader (where it samples a 2D
  or 3D texture);
- programs made up to use every part of the intermediate form the captures
  may not (`synthetic.c`): each texture mode and dot mapping, combiner
  input, mapping and destination, the final combiner's inputs, each fog mode,
  alpha test function and feature, each vertex program operation.

Identical shaders are written once; `index.txt` names each variant's file.

The captures are what the renderer's test capture writes (`HALO_TEST_CAPTURE`,
not part of the game): `vsNNN.bin` (the program's NORMPACKED3 inputs' mask, 4
bytes, then its microcode) and `ps_XXXXXXXX.key` (a pixel shader key as the
32-bit game lays it out, 264 bytes).

On Linux:

    tools/msl_corpus/build.sh . build/msl_corpus
    build/msl_corpus/msl_corpus <capture dir> <corpus dir>

On the Mac (Xcode's command line tools):

    sh tools/msl_corpus/compile_mac.sh <corpus dir> <log>

which compiles each file as the Metal backend would (MSL 3.0, the position
invariant) and logs every error and warning, then a count of each.
