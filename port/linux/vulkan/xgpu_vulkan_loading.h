/* The loading scene is independent of the game's targets and pipelines. */
#ifndef XGPU_VULKAN_LOADING_H
#define XGPU_VULKAN_LOADING_H

#include <stdint.h>
struct loading_presentation_frame;
uint64_t xgpu_vulkan_loading(void *context, const struct loading_presentation_frame *frame);
uint64_t host_vulkan_display_period(void);

#endif
