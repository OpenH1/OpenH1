/* POSIX <strings.h> for the 64-bit Windows host's shared files: the C
runtime's case-insensitive comparisons */

#ifndef __HALO_WINDOWS_HOST_STRINGS_H
#define __HALO_WINDOWS_HOST_STRINGS_H

#include <string.h>

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

#endif
