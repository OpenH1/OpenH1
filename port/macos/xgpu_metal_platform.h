/*
XGPU_METAL_PLATFORM.H

The host's stand-in for the platform header (port/linux/src/platform.h,
the guest's) where the renderer's shared parts are built into the macOS
host: the MSL emitter and its headers (xgpu.h, xgpu_backend.h,
xgpu_shader.h, xgpu_msl.h) and the Xbox's texture decoding; in the iOS
host, the disc image's extractor too (xiso.c). tools/
macos_build.py copies them beside it as platform.h, with the XDK's
enumerations (port/include/xdk/xdk_pdb.h's) as xdk_enums.h. The types are
the guest's sizes (DWORD 4 bytes), which the host's code must not depend
on: it converts the stream's records field by field.
*/

#ifndef __HALO_MACOS_XGPU_METAL_PLATFORM_H
#define __HALO_MACOS_XGPU_METAL_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

typedef int BOOL;
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef DWORD D3DCOLOR;

#define TRUE 1
#define FALSE 0

/* (the iOS host's: port/ios/data_import.m, for xiso.c) */
void platform_log(const char *format, ...);

#include "xdk_enums.h"

#endif
