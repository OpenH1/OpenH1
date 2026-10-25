/*
TEXTURE_CACHE_SIZE.C (test)

The real texture_cache_map_page_count (xbox_texture_cache.c): the pages a
map's textures may take, by the kind of map, whether mods' bitmaps are
played over it, and the computer's memory, with the capacities of
halo_port_capacity.h as the AArch64 guests or the desktop builds have them.
test_texture_cache_size.py takes the capacities (config.inc, AARCH64_GUEST
saying which builds' they are), the cache's constants and the code
(under_test.inc).
*/

#include "harness.h"
#include "config.inc"

#define GB (1ULL << 30)
#define MB_PAGES(megabytes) ((megabytes) * 64L) /* (16 KB pages) */

#include "under_test.inc"

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	/* an Xbox map: the Xbox's 22 MB on the AArch64 guests, whatever the
	memory; the whole cache on the desktop builds */
	CASE("xbox-map")
	{
		long expected = AARCH64_GUEST ? 0x580 : MB_PAGES(256);

		CHECK(texture_cache_map_page_count(FALSE, FALSE, 16 * GB) == expected, "%ld pages",
			texture_cache_map_page_count(FALSE, FALSE, 16 * GB));
		CHECK(texture_cache_map_page_count(FALSE, FALSE, 0) == expected, "%ld pages with no memory known",
			texture_cache_map_page_count(FALSE, FALSE, 0));
		return 0;
	}
	/* a Custom Edition map: a 32nd of the memory, from 64 MB to 256 MB, on
	the AArch64 guests; the whole cache on the desktop builds */
	CASE("custom-edition-map")
	{
		static const struct { unsigned long long memory; long guest, desktop; } sizes[] =
		{
			{ 0, MB_PAGES(64), MB_PAGES(256) }, /* (unknown: the least) */
			{ 1 * GB, MB_PAGES(64), MB_PAGES(256) },
			{ 2 * GB, MB_PAGES(64), MB_PAGES(256) },
			{ 3 * GB, MB_PAGES(96), MB_PAGES(256) },
			{ 4 * GB, MB_PAGES(128), MB_PAGES(256) },
			{ 6 * GB, MB_PAGES(192), MB_PAGES(256) },
			{ 8 * GB, MB_PAGES(256), MB_PAGES(256) },
			{ 16 * GB, MB_PAGES(256), MB_PAGES(256) },
			{ 64 * GB, MB_PAGES(256), MB_PAGES(256) },
		};
		int index;

		for (index = 0; index < (int)(sizeof(sizes) / sizeof(sizes[0])); index++)
		{
			long expected = AARCH64_GUEST ? sizes[index].guest : sizes[index].desktop;
			long found = texture_cache_map_page_count(TRUE, FALSE, sizes[index].memory);

			CHECK(found == expected, "%llu MB of memory: %ld pages, not %ld", sizes[index].memory >> 20, found, expected);
		}
		return 0;
	}
	/* an Xbox map with mods' bitmaps takes a Custom Edition map's size */
	CASE("modded-xbox-map")
	{
		CHECK(texture_cache_map_page_count(FALSE, TRUE, 4 * GB) == texture_cache_map_page_count(TRUE, FALSE, 4 * GB),
			"%ld pages", texture_cache_map_page_count(FALSE, TRUE, 4 * GB));
		CHECK(texture_cache_map_page_count(TRUE, TRUE, 4 * GB) == texture_cache_map_page_count(TRUE, FALSE, 4 * GB),
			"%ld pages", texture_cache_map_page_count(TRUE, TRUE, 4 * GB));
		return 0;
	}
	fprintf(stderr, "unknown case '%s'\n", case_name);
	return 2;
}
