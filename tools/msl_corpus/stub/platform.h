/* msl_corpus's stand-in for the platform layer (port/linux/src/platform.h):
the types the shader decoders and emitters use, and the XDK's enumerations
(build.sh takes them from port/include/xdk/xdk_pdb.h) */
#ifndef MSL_CORPUS_PLATFORM_H
#define MSL_CORPUS_PLATFORM_H
#include <stdlib.h>
#include <string.h>
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef unsigned int D3DCOLOR;
#define TRUE 1
#define FALSE 0
#include "xdk_enums.h"
void platform_log(const char *format, ...);
#endif
