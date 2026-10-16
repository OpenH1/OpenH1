/*
XGPU_METAL_PREBUILD.H

The pipelines script's writer (xgpu_metal_prebuild_script.m), for
xgpu_metal_prebuild.c: Objective-C's, called from C.
*/

#ifndef __HALO_MACOS_XGPU_METAL_PREBUILD_H
#define __HALO_MACOS_XGPU_METAL_PREBUILD_H

struct xgpu_metal_clear_key;
struct xgpu_metal_pipeline_state_key;

/* a pipeline (its functions by name; fragment NULL for none) */
void prebuild_script_pipeline(const struct xgpu_metal_pipeline_state_key *key, const char *vertex, const char *fragment);
/* a clear's, the presentation's */
void prebuild_script_clear(const struct xgpu_metal_clear_key *key);
void prebuild_script_present(void);
/* writes the script, its functions the library's at library; returns the
pipelines (exits on failure) */
unsigned long prebuild_script_write(const char *path, const char *library);

/* writes text into a file (1 on success); the clears' and the
presentation's functions' source (exits on failure) */
int prebuild_write_text(const char *path, const char *text, unsigned long size);
void prebuild_write_fixed(const char *path);

#endif
