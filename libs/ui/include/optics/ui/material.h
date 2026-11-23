/*
 * ui/material.h — semantic material tokens and recipe models for ui.
 *
 * Materials in ui represent physical and styled surfaces (Mica, Acrylic,
 * Frosted Glass, Liquid Glass) integrated into the design token system and
 * style cascade.
 *
 * Design Invariants:
 *   1. Domain Boundary: material owns physical shader permutations, optical
 *      refraction, and BRDF models. ui owns theme tokens, semantic roles,
 *      and component style cascade resolution.
 *   2. Thematic Symmetry: Material recipes define dual-polarity recipes:
 *      Smoke plate (0.0) for dark theme contrast, Pearl plate (1.0) for light
 *      theme diffusion, along with theme-derived tints, highlights, and drop shadows.
 *   3. Headless Resilience: Material tokens are pure data; they compile cleanly
 *      without graphics backends or GPU dependencies.
 */

#ifndef OPTICS_UI_MATERIAL_H
#define OPTICS_UI_MATERIAL_H

#include <optics/gfx/core.h>
#include <optics/gfx/math.h> /* optics_gfx_color */
#include <optics/ui/export.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Semantic material kinds recognized across the design system. */
typedef enum optics_ui_material_kind : uint32_t {
    OPTICS_UI_MATERIAL_NONE = 0,
    OPTICS_UI_MATERIAL_MICA = 1,         /* Screen-anchored foundation (Windows 11 Mica) */
    OPTICS_UI_MATERIAL_MICA_ALT = 2,     /* High-contrast commanding bars/tabs (Mica Alt) */
    OPTICS_UI_MATERIAL_ACRYLIC = 3,      /* Transient backdrop-sampling composite (Acrylic) */
    OPTICS_UI_MATERIAL_FROSTED = 4,      /* Dual-Kawase vibrancy blur (macOS Vibrancy) */
    OPTICS_UI_MATERIAL_LIQUID_GLASS = 5, /* Optical IOR refraction & focus field */
} optics_ui_material_kind;

/* Concrete physical recipe mapped to a material under a specific theme. */
typedef struct optics_ui_material_recipe {
    float plate_polarity;      /* [0, 1]: 0.0 = Smoke (dark plate), 1.0 = Pearl (light plate) */
    float plate_opacity;       /* Luminosity plate weight [0, 1] */
    optics_gfx_color tint;     /* Theme wash/tint color multiplier */
    float tint_opacity;        /* Tint wash strength [0, 1] */
    float border_highlight;    /* Specular / 1px SDF border highlight alpha [0, 1] */
    float shadow_alpha;        /* Spatial elevation drop-shadow opacity [0, 1] */
    float shadow_blur;         /* Spatial elevation drop-shadow blur radius in px */
    float noise_intensity;     /* Procedural grain / dithering intensity (eliminates banding) */
    optics_gfx_color fallback; /* Solid fallback color for inactive / low-power state */
    float contact_ao;          /* Sub-pixel contact ambient occlusion [0, 1] (CAO) */
    float ambient_fresnel;     /* 360-degree isotropic environmental Fresnel sheen [0, 1] */
} optics_ui_material_recipe;

/* Design token group for materials attached to optics_ui_theme. */
typedef struct optics_ui_theme_materials {
    optics_ui_material_recipe foundation;     /* Window foundation / desktop root (Mica Base) */
    optics_ui_material_recipe command;        /* Commanding bars, headers, tab strips (Mica Alt) */
    optics_ui_material_recipe floating;       /* Popups, dropdowns, modals (Acrylic / Frosted) */
    optics_ui_material_recipe optics_ui_body; /* Refractive interactive chrome (Liquid Glass) */
} optics_ui_theme_materials;

/* Standard recipe factory: returns reference neutral recipes for light/dark modes. */
OPTICS_UI_API optics_ui_material_recipe
optics_ui_material_recipe_default(optics_ui_material_kind kind, bool dark);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_UI_MATERIAL_H */
