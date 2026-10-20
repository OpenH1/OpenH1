/*
MAIN.C

StageKit (README.md): writes a scenario's map, in code.

usage: stagekit --list
       stagekit <scenario> <map file> [<name>=<value>,...]

The values, each a number, change what the scenario builds where it asks for
them by name (stage_parameter): the scorecard's hidden scenarios vary them
(tools/scorecard/README.md).

Built as a guest of the game's host (tests/runner.py), it takes its arguments
from HALO_STAGEKIT, separated by |, and the host runs it with
HALO_GUEST_IMAGE naming it.
*/

/* ---------- headers */

#include "stagekit.h"
#include <stdlib.h>

/* ---------- constants */

enum
{
	MAXIMUM_PARAMETERS = 32,
};

/* ---------- globals */

extern struct stagekit_scenario const scenario_walk;
extern struct stagekit_scenario const scenario_obstacle;
extern struct stagekit_scenario const scenario_elevator;
extern struct stagekit_scenario const scenario_shoot;
extern struct stagekit_scenario const scenario_lob;
extern struct stagekit_scenario const scenario_patrol;
extern struct stagekit_scenario const scenario_drive;
extern struct stagekit_scenario const scenario_lift;

static struct stagekit_scenario const *const scenarios[] = { &scenario_walk, &scenario_obstacle, &scenario_elevator,
	&scenario_shoot, &scenario_lob, &scenario_patrol, &scenario_drive, &scenario_lift };

static struct
{
	char name[32];
	real value;
} parameters[MAXIMUM_PARAMETERS];
static int parameter_count;

/* ---------- private code */

/* name=value,name=value... */
static int parameters_read(
	char const *text)
{
	while (*text)
	{
		char const *end = strchr(text, ',');
		int length = end ? (int)(end - text) : (int)strlen(text);
		char const *equals = memchr(text, '=', length);
		char *number_end;

		if (!equals || equals == text || equals - text >= (int)sizeof(parameters[0].name) ||
			parameter_count >= MAXIMUM_PARAMETERS)
		{
			fprintf(stderr, "stagekit: \"%.*s\" is not <name>=<value>\n", length, text);
			return 0;
		}
		snprintf(parameters[parameter_count].name, sizeof(parameters[0].name), "%.*s", (int)(equals - text), text);
		parameters[parameter_count].value = (real)strtod(equals + 1, &number_end);
		if (number_end != text + length)
		{
			fprintf(stderr, "stagekit: \"%.*s\" is not a number\n", length, text);
			return 0;
		}
		parameter_count++;
		text += length + (end != NULL);
	}
	return 1;
}

/* ---------- public code */

real stage_parameter(
	char const *name,
	real value)
{
	int index;

	for (index = 0; index < parameter_count; index++)
	{
		if (!strcmp(parameters[index].name, name))
			return parameters[index].value;
	}
	return value;
}

int main(
	int argc,
	char **argv)
{
	struct stage stage = { 0 };
	struct stagekit_scenario const *scenario = NULL;
	long tag_count;
	unsigned long index;
	static char *words[5];
	static char arguments[1024];

	/* (a guest: its arguments in the environment, which is all the host
	passes it) */
	if (argc == 1 && getenv("HALO_STAGEKIT"))
	{
		char *word = arguments;

		snprintf(arguments, sizeof(arguments), "%s", getenv("HALO_STAGEKIT"));
		words[0] = argv[0];
		for (argc = 1; argc < 5 && word; argc++)
		{
			words[argc] = word;
			word = strchr(word, '|');
			if (word)
				*word++ = '\0';
		}
		argv = words;
	}

	if (argc == 2 && !strcmp(argv[1], "--list"))
	{
		for (index = 0; index < NUMBEROF(scenarios); index++)
			printf("%-12s %s\n", scenarios[index]->name, scenarios[index]->description);
		return 0;
	}
	for (index = 0; (argc == 3 || argc == 4) && index < NUMBEROF(scenarios); index++)
	{
		if (!strcmp(scenarios[index]->name, argv[1]))
			scenario = scenarios[index];
	}
	if (!scenario || (argc == 4 && !parameters_read(argv[3])))
	{
		fprintf(stderr, "usage: stagekit --list\n       stagekit <scenario> <map file> [<name>=<value>,...]\n");
		return 2;
	}
	stage_build(&stage, scenario);
	cache_write(argv[2], &tag_count);
	printf("stagekit: %s: %s, %ld tags\n", scenario->name, argv[2], tag_count);
	return 0;
}
