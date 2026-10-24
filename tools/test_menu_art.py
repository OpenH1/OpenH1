"""Menu texture lookup preserves replacement and forget semantics at capacity."""
from pathlib import Path
import subprocess

from test_load_trace import build

ROOT = Path(__file__).resolve().parents[1]


def test_menu_art_registration_replacement_and_forget(tmp_path):
    text = (ROOT / "port/linux/src/menu_files.c").read_text()
    code = text[text.index("/* ---------- art */"):]
    source = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#define MAXIMUM_ART 1024
static unsigned int created, deleted;
static void platform_log(const char *format, ...) { (void)format; }
static void xgpu_image_delete(unsigned int image) { assert(image); deleted++; }
static const unsigned char *file_data(const char *name, unsigned long *size)
{
    *size = 1;
    return !strcmp(name, "missing") ? NULL : (const unsigned char *)"x";
}
static unsigned int hud_hires_png_texture(const unsigned char *png,
    unsigned long size, unsigned long *levels)
{
    assert(png && size == 1);
    *levels = 3;
    return ++created;
}
''' + code + r'''
int main(void)
{
    unsigned long texture[2] = {0, 0}, levels = 0, i;
    unsigned int original;
    for (i = 0; i < MAXIMUM_ART; i++) {
        texture[1] = i * 4096;
        halo_menus_art_register(texture, "image");
    }
    for (i = 0; i < MAXIMUM_ART; i++) {
        assert(menu_art_texture(i * 4096, &levels) != 0 && levels == 3);
        assert(menu_art_texture(i * 4096 + 1, &levels) == 0);
    }
    assert(created == MAXIMUM_ART);
    texture[1] = MAXIMUM_ART * 4096;
    halo_menus_art_register(texture, "image");
    assert(menu_art_texture(texture[1], &levels) == 0);
    texture[1] = 0;
    original = menu_art_texture(0, &levels);
    halo_menus_art_register(texture, "image");
    assert(menu_art_texture(0, &levels) == original && !deleted);
    halo_menus_art_register(texture, "replacement");
    assert(deleted == 1 && menu_art_texture(0, &levels) != original);
    halo_menus_art_register(texture, "missing");
    assert(menu_art_texture(0, &levels) == 0);
    halo_menus_art_forget();
    assert(deleted == created);
    for (i = 0; i < MAXIMUM_ART; i++) assert(menu_art_texture(i * 4096, &levels) == 0);
    halo_menus_art_register(texture, "image");
    assert(menu_art_texture(0, &levels));
    halo_menus_art_forget();
    assert(deleted == created);
    return 0;
}
'''
    subprocess.run([str(build(tmp_path, source))], check=True, timeout=15)
