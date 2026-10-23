# astc-encoder

Arm's ASTC texture compressor in C++, by Arm Limited, licensed under the
Apache License 2.0 (see `LICENSE.txt`).

Upstream: https://github.com/ARM-software/astc-encoder, release `5.7.0`,
`astc-encoder-5.7.0.tar.gz` (GitHub's archive of the tag), SHA-256
`7c1b28ece59c9c2737e297123a9910c1c548676c7aed87a09d5734e2fa7bdaf0`.

Only the core library's sources in `Source/` (`astcenc.h` and
`astcenc_*.cpp`, `astcenc_*.h`; not its command-line program's
`astcenccli_*`, its tests, utilities or build files) and `LICENSE.txt` are
kept, and nothing in them is changed. They are compiled as its own CMake
build compiles them for AArch64 (`ASTCENC_NEON=1`, the other instruction
sets off), without exceptions or RTTI, which it does not use
(`tools/build_common.py`, `ASTC_CXX_FLAGS`).

The hosts of the GPUs that cannot sample the Xbox's DXT textures (iOS's
Metal, the Android app's Vulkan) transcode them to ASTC 4x4
(`port/linux/astc`, `display.astc_textures`); this encodes the few blocks
that cannot be transcoded exactly, on worker threads, and decodes them to
check the result (`port/linux/astc/xgpu_astc_encode.cpp`). It is the hosts'
only C++, linked with the platform's C++ library (statically on Android).
It uses the system's maths functions: it encodes textures for the GPU on one
machine, never anything the game's simulation or another machine reads.
