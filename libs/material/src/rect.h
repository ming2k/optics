#ifndef OPTICS_MATERIAL_RECT_H
#define OPTICS_MATERIAL_RECT_H

#include <stdint.h>

/* An integer pixel rectangle inside a material image or capture: dispatch
 * regions, clear regions and slab footprints. */
typedef struct optics_material__rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} optics_material__rect;

#endif /* OPTICS_MATERIAL_RECT_H */
