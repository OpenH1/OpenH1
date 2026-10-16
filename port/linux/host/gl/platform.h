/*
PLATFORM.H (the host's)

The host's stand-in for the platform header (port/linux/src/platform.h,
the guest's) where the OpenGL backend is built into the host, behind the
renderer's command stream (XGPU_GL_HOST): the backend (xgpu_gl.c, its GLSL
emitter, anti-aliasing passes and entry points) and its headers, and the
Xbox's texture decoding. tools/linux_guest_build.py copies them beside this
folder's headers, which their quoted includes then find in place of the
guest's (as tools/macos_build.py does for Metal), with the XDK's
enumerations (port/include/xdk/xdk_pdb.h's) as xdk_enums.h.

The types are the guest's sizes (DWORD 4 bytes); the interface's own
structures (xgpu_backend.h) keep their unsigned longs, 8 bytes here, which
the host fills from the stream's records field by field
(port/linux/host/host_gl_stream.c).
*/

#ifndef __HALO_HOST_GL_PLATFORM_H
#define __HALO_HOST_GL_PLATFORM_H

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
#include "halo_android_abi.h"

/* the Xbox's contiguous memory, the guest's window (the mirror's segments
cover it) */
#define PLATFORM_CONTIGUOUS_BASE HALO_GUEST_WINDOW_BASE
#define PLATFORM_CONTIGUOUS_SIZE HALO_GUEST_WINDOW_SIZE

/* the host's log (host_logf's information) */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
