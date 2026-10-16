/*
RENDER_ENUMERATE.H

A map's shaders and pipelines made as it starts (render_enumerate.c).
*/

#ifndef __HALO_PORT_RENDER_ENUMERATE_H
#define __HALO_PORT_RENDER_ENUMERATE_H

#include "cseries.h"

/* the window being drawn is one of a map's first frame's, unseen, drawn
through every shader */
extern boolean render_enumerating;

/* TRUE as a map's (or a new structure's) first frame is to be drawn so
first: its window drawn again each time render_enumerate_next says TRUE */
void render_enumerate_new_map(void);
boolean render_enumerate_begin(void);
boolean render_enumerate_next(void);
/* the window's parts: after the visibility's (every material's surfaces),
after the objects' (every model) */
void render_enumerate_structure(void);
void render_enumerate_models(void);
/* (before the transparent geometry's pass: every effect) */
void render_enumerate_effects(void);

/* the renderer's shaders and pipelines all made (port/linux/src/
d3d8_device.c): the seconds waited */
double halo_gpu_prepared(void);
void halo_gpu_prepare(void);

#endif
