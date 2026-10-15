/*
PLATFORM.H (the render stream's test)

The few of the guest's types the renderer's interface headers
(xgpu.h, xgpu_backend.h, xgpu_shader.h) use, for building the stream's
encoder on the build machine (tools/test_render_stream.py). As the XDK
has them: DWORD is an unsigned long, 8 bytes here and 4 in the guest,
which the encoder must not depend on.
*/

#ifndef __HALO_RENDER_STREAM_TEST_PLATFORM_H
#define __HALO_RENDER_STREAM_TEST_PLATFORM_H

#include <stddef.h>

typedef int BOOL;
typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef DWORD D3DCOLOR;

#define TRUE 1
#define FALSE 0

#define D3DRS_PS_MAX 57

typedef struct D3DBaseTexture
{
	unsigned long Common;
	unsigned long Data;
	unsigned long Lock;
	unsigned long Format;
	unsigned long Size;
} D3DBaseTexture;

#endif
