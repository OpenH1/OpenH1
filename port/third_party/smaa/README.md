# SMAA

Enhanced Subpixel Morphological Antialiasing, by Jorge Jimenez, Jose I.
Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez, MIT licensed
(see `LICENSE`).

Upstream: https://github.com/iryoku/smaa, commit
71c806a838bdd7d517df19192a20f0c61b3ca29d. `SMAA.hlsl` is copied
unchanged. `area_tex.zlib` and `search_tex.zlib` are the bytes of
`Textures/AreaTex.h` (160x560, two channels) and `Textures/SearchTex.h`
(64x16, one channel), compressed with zlib:

    python -c "import re, sys, zlib; t = open(sys.argv[1]).read(); \
    open(sys.argv[2], 'wb').write(zlib.compress(bytes(int(h, 16) for h in \
    re.findall(r'0x([0-9a-fA-F]{2})', t[t.index('{'):])), 9))" AreaTex.h area_tex.zlib

`display.anti_aliasing = "smaa"` antialiases the 3D view with it. The
Vulkan backend (the Linux builds' and Android's) has the passes as SPIR-V,
and the lookup textures inflated, in
`port/linux/vulkan/xgpu_vulkan_post_shaders.c`, which
`tools/vulkan_post_shaders.py` generates from these files. The Metal
backend has them as MSL, by way of the same SPIR-V and SPIRV-Cross, in
`port/macos/xgpu_metal_post_shaders.c`, which `tools/metal_post_shaders.py`
generates. The OpenGL backend (the Windows build's) compiles them as GLSL
(`port/linux/src/xgpu_post.c`), the files embedded in the game by
`tools/embed_assets.py`. OpenGL ES (the web) has FXAA in its place.
