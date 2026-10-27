/*
CONFIG_CHECK.C

port/linux/src/port_config.c on its own, for tools/test_linux_port.py's
tests of config.toml: built with stand-ins for SDL, the platform layer
(the test writes them) and install_layout.c, the file in the folder given.

    config_check <folder> get <setting>          prints its value
    config_check <folder> write <setting> <value>
    config_check <folder> saving <setting>       prints the value the game reads
                                                 with battery saver on, then the
                                                 one Settings shows
*/

#include "install_layout.h"
#include "port_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char folder[1024];

/* (the platform layer's command line: none here, so no --config, --server
or --version; and a dedicated server's private file, which none is) */
int posix_command_line_argument(int index, char *buffer, unsigned long size)
{
	(void)index;
	(void)buffer;
	(void)size;
	return 0;
}

int posix_server_private_file(const char *path)
{
	(void)path;
	return 1;
}

/* (install_layout.c's: the folder of config.toml) */
const char *install_config_folder(void)
{
	return folder;
}

#ifdef HALO_MACOS
/* (the macOS app's: SDL's base path, its settings folder) */
const char *SDL_GetBasePath(void)
{
	return folder;
}
#endif

/* (port_config.c's look for a config.toml it does not read: none here) */
typedef struct SDL_PathInfo SDL_PathInfo;
_Bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info)
{
	(void)path;
	(void)info;
	return 0;
}

void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	fprintf(stderr, "log: ");
	vfprintf(stderr, format, arguments);
	fprintf(stderr, "\n");
	va_end(arguments);
}

int main(int argc, char **argv)
{
	char text[512];

	if (argc < 4)
		return 2;
	snprintf(folder, sizeof(folder), "%s/", argv[1]);
	if (!strcmp(argv[2], "get") && config_text(argv[3], text, sizeof(text)))
	{
		printf("%s\n", text);
		return 0;
	}
	if (!strcmp(argv[2], "write") && argc == 5)
		return config_write(argv[3], argv[4]) ? 0 : 1;
	/* (as the game reads it with battery saver on, by the type its text
	says, then config_text's, which Settings shows) */
	if (!strcmp(argv[2], "saving") && config_text(argv[3], text, sizeof(text)))
	{
		config_battery_saver_set(1);
		if (!strcmp(text, "true") || !strcmp(text, "false"))
			printf("%s", config_boolean(argv[3]) ? "true" : "false");
		else if (text[0] >= '0' && text[0] <= '9')
			printf("%ld", config_integer(argv[3]));
		else
			printf("%s", config_string(argv[3]));
		config_text(argv[3], text, sizeof(text));
		printf(" %s\n", text);
		return 0;
	}
	return 2;
}
