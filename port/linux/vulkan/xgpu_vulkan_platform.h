/*
XGPU_VULKAN_PLATFORM.H

The host's stand-in for the platform header (port/linux/src/platform.h,
the guest's) where the renderer's shared parts are built into the Vulkan
build's host: the SPIR-V emitter and its headers (xgpu.h, xgpu_shader.h,
xgpu_spirv.h) and the Xbox's texture decoding. tools/linux_guest_build.py
copies them beside it as platform.h, with the XDK's enumerations
(port/include/xdk/xdk_pdb.h's) as xdk_enums.h, as the macOS build does with
its own (port/macos/xgpu_metal_platform.h). The types are the guest's sizes
(DWORD 4 bytes), which the host's code must not depend on: it converts the
stream's records field by field.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_PLATFORM_H
#define __HALO_LINUX_XGPU_VULKAN_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

typedef int BOOL;
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef DWORD D3DCOLOR;

#define TRUE 1
#define FALSE 0

#include "xdk_enums.h"

#endif
