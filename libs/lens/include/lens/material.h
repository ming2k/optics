/*
 * lens/material.h — semantic material tokens and recipe models for Lens.
 *
 * Materials in Lens represent physical and styled surfaces (Mica, Acrylic,
 * Frosted Glass, Liquid Glass) integrated into the design token system and
 * style cascade.
 *
 * Design Invariants:
 *   1. Domain Boundary: Prism owns physical shader permutations, optical
 *      refraction, and BRDF models. Lens owns theme tokens, semantic roles,
 *      and component style cascade resolution.
 *   2. Thematic Symmetry: Material recipes define dual-polarity recipes:
 *      Smoke plate (0.0) for dark theme contrast, Pearl plate (1.0) for light
 *      theme diffusion, along with theme-derived tints, highlights, and drop shadows.
 *   3. Headless Resilience: Material tokens are pure data; they compile cleanly
 *      without graphics backends or GPU dependencies.
 */

#ifndef LENS_MATERIAL_H
#define LENS_MATERIAL_H

#include <flux/core.h>
#include <lens/export.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Semantic material kinds recognized across the design system. */
typedef enum lens_material_kind : uint32_t {
    LENS_MATERIAL_NONE = 0,
    LENS_MATERIAL_MICA = 1,         /* Screen-anchored foundation (Windows 11 Mica) */
    LENS_MATERIAL_MICA_ALT = 2,     /* High-contrast commanding bars/tabs (Mica Alt) */
    LENS_MATERIAL_ACRYLIC = 3,      /* Transient backdrop-sampling composite (Acrylic) */
    LENS_MATERIAL_FROSTED = 4,      /* Dual-Kawase vibrancy blur (macOS Vibrancy) */
    LENS_MATERIAL_LIQUID_GLASS = 5, /* Optical IOR refraction & focus field */
} lens_material_kind;

/* Concrete physical recipe mapped to a material under a specific theme. */
typedef struct lens_material_recipe {
    float plate_polarity;   /* [0, 1]: 0.0 = Smoke (dark plate), 1.0 = Pearl (light plate) */
    float plate_opacity;    /* Luminosity plate weight [0, 1] */
    flux_color tint;        /* Theme wash/tint color multiplier */
    float tint_opacity;     /* Tint wash strength [0, 1] */
    float border_highlight; /* Specular / 1px SDF border highlight alpha [0, 1] */
    float shadow_alpha;     /* Spatial elevation drop-shadow opacity [0, 1] */
    float shadow_blur;      /* Spatial elevation drop-shadow blur radius in px */
    float noise_intensity;  /* Procedural grain / dithering intensity (eliminates banding) */
    flux_color fallback;    /* Solid fallback color for inactive / low-power state */
    float contact_ao;       /* Sub-pixel contact ambient occlusion [0, 1] (CAO) */
    float ambient_fresnel;  /* 360-degree isotropic environmental Fresnel sheen [0, 1] */
} lens_material_recipe;

/* Design token group for materials attached to lens_theme. */
typedef struct lens_theme_materials {
    lens_material_recipe foundation; /* Window foundation / desktop root (Mica Base) */
    lens_material_recipe command;    /* Commanding bars, headers, tab strips (Mica Alt) */
    lens_material_recipe floating;   /* Popups, dropdowns, modals (Acrylic / Frosted) */
    lens_material_recipe lens_body;  /* Refractive interactive chrome (Liquid Glass) */
} lens_theme_materials;

/* Standard recipe factory: returns reference neutral recipes for light/dark modes. */
LENS_API lens_material_recipe lens_material_recipe_default(lens_material_kind kind, bool dark);

#ifdef __cplusplus
}
#endif

#endif /* LENS_MATERIAL_H */
