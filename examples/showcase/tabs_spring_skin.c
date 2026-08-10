/* tabs_spring_skin.c — RECIPE, not a library feature (ADR-0061).
 *
 * This is the spring-animated tab indicator that lived in lens's core
 * before ADR-0061 ("mechanism, neutral defaults, never flavor behind a
 * flag"). It is deliberately an EXAMPLE: copy this file into your own
 * tree and modify it — the physics constants, the colours, the geometry
 * are all yours. Bug fixes here do not propagate to copies; that is the
 * point of shipping flavor as source instead of as a flag.
 *
 * What it demonstrates:
 *   - a caller-owned LENS_WIDGET_TABS skin (lens_set_skin),
 *   - per-node animation state without any library allocation:
 *     lens_skin_scratch (4 retained floats = left/right + velocities),
 *   - the record payload a tabs skin needs (per-tab label, metrics,
 *     state, hover, last-frame geometry),
 *   - driving the animation from the host: iris_request_animation_frame()
 *     keeps the frame loop alive while the spring is unsettled.
 *
 * The window shows one tab strip; click tabs (or focus + Return) and the
 * indicator's edges spring across, stretching in the direction of travel.
 */

#include <iris/app.h>
#include <lens/lens.h>

#include <math.h>
#include <stdio.h>

/* ---- recipe parameters (yours to tune once copied) ------------------ */
static const float SPRING_STIFFNESS_LEAD = 480.0f; /* edge leading the travel */
static const float SPRING_STIFFNESS_TRAIL = 220.0f;
static const float SPRING_DAMPING_LEAD = 24.0f;
static const float SPRING_DAMPING_TRAIL = 18.0f;
static const float INDICATOR_THICKNESS = 3.0f;

static void spring_integrate(float *position, float *velocity, float target, float stiffness,
                             float damping, float dt) {
    *velocity += (target - *position) * stiffness * dt;
    *velocity *= expf(-damping * dt);
    *position += *velocity * dt;
}

/* ---- the skin ------------------------------------------------------- */

static void spring_tabs_skin(lens *ui, lens_node *strip, const lens_widget_record *rec) {
    const lens_style_resolved *rs = &rec->style;
    const lens_widget_content *c = &rec->content;
    if (c->tab_count <= 0)
        return;

    /* Per-tab chrome + label, onto each tab node (node-local rects resolve
     * against the tab's final box at render time). */
    lens_node *child = lens_node_first_child(strip);
    for (int i = 0; i < c->tab_count && child; i++, child = lens_node_next_sibling(child)) {
        const lens_tab_item *tab = &c->tabs[i];
        bool disabled = (tab->state & LENS_STATE_DISABLED) != 0;
        if (!disabled && tab->hover_t > 0.0f) {
            /* premultiplied translucent hover wash */
            lens_skin_rect(ui, child, (flux_rect){2.0f, 1.0f, 0, 0},
                           flux_color_rgba(0xFF, 0xFF, 0xFF, (uint8_t)(24.0f * tab->hover_t)),
                           rs->corner_radius);
        }
        float text_y = (tab->last_bounds.h - tab->text.height) * 0.5f;
        if (text_y < 0.0f)
            text_y = 0.0f;
        lens_skin_text(ui, child, (flux_rect){rs->padding, text_y, -1.0f, 0},
                       disabled ? rs->disabled : rs->fg, tab->label, rs->font_size, 0.0f);
    }

    if (c->active_index < 0 || c->active_index >= c->tab_count)
        return;
    const lens_tab_item *first = &c->tabs[0];
    const lens_tab_item *sel = &c->tabs[c->active_index];
    if (first->last_bounds.w <= 0.0f || sel->last_bounds.w <= 0.0f)
        return; /* first frame: no geometry yet, draw nothing */

    /* Indicator target: centred under the active tab's label, coordinates
     * relative to the first tab (stable under ancestor motion). */
    float indicator_padding = fmaxf(8.0f, rs->padding * 0.75f);
    float tab_left = sel->last_bounds.x - first->last_bounds.x;
    float tab_width = sel->last_bounds.w;
    float target_w = fminf(tab_width, sel->text.width + 2.0f * indicator_padding);
    if (target_w < INDICATOR_THICKNESS)
        target_w = INDICATOR_THICKNESS;
    float target_left = tab_left + (tab_width - target_w) * 0.5f;
    float target_right = target_left + target_w;

    /* Spring state: four retained floats on the strip node, zeroed on first
     * touch, GC'd with the node. Storage is the library's job; the
     * animation is this recipe's. scratch layout: [0]=left [1]=right
     * [2]=left velocity [3]=right velocity. A zero right edge means the
     * spring has never been seeded (any real tab has right > 0). */
    float *s = lens_skin_scratch(ui, strip);
    if (!s)
        return;
    if (s[1] <= 0.0f) {
        s[0] = target_left;
        s[1] = target_right;
        s[2] = s[3] = 0.0f;
    } else {
        float dt = lens_dt(ui);
        if (dt <= 0.0f)
            dt = 1.0f / 60.0f;
        if (dt > 1.0f / 30.0f)
            dt = 1.0f / 30.0f;
        bool moving_right = target_left >= s[0];
        spring_integrate(&s[0], &s[2], target_left,
                         moving_right ? SPRING_STIFFNESS_LEAD : SPRING_STIFFNESS_TRAIL,
                         moving_right ? SPRING_DAMPING_LEAD : SPRING_DAMPING_TRAIL, dt);
        spring_integrate(&s[1], &s[3], target_right,
                         moving_right ? SPRING_STIFFNESS_TRAIL : SPRING_STIFFNESS_LEAD,
                         moving_right ? SPRING_DAMPING_TRAIL : SPRING_DAMPING_LEAD, dt);
        if (fabsf(s[0] - target_left) < 0.05f && fabsf(s[1] - target_right) < 0.05f &&
            fabsf(s[2]) < 0.05f && fabsf(s[3]) < 0.05f) {
            s[0] = target_left;
            s[1] = target_right;
            s[2] = s[3] = 0.0f;
        }
    }

    if (s[0] != target_left || s[1] != target_right)
        iris_request_animation_frame(); /* unsettled: keep the host ticking */

    /* Draw at the strip's bottom edge (last-frame strip height; the strip
     * never reshapes here and the one-frame latency is invisible). */
    float strip_h = rec->last_bounds.h;
    if (strip_h <= 0.0f)
        return;
    lens_skin_rect(ui, strip,
                   (flux_rect){s[0], strip_h - INDICATOR_THICKNESS,
                               fmaxf(INDICATOR_THICKNESS, s[1] - s[0]), INDICATOR_THICKNESS},
                   rs->accent, INDICATOR_THICKNESS * 0.5f);
}

/* ---- the demo app ---------------------------------------------------- */

static void build(lens *ui, const lens_input *in, void *user) {
    (void)in;
    int *active = user;

    /* Install the recipe skin. lens_set_skin is a pointer store on the
     * context, so re-applying it every frame is both harmless and the
     * simplest correct thing. */
    lens_set_skin(ui, LENS_WIDGET_TABS, spring_tabs_skin);

    lens_column_ex(ui, (lens_layout_opts){.pad = 24, .gap = 14, .cross = LENS_START});
    lens_label(ui, "Spring indicator — a caller-owned LENS_WIDGET_TABS skin.");
    lens_label(ui, "This file is a recipe: copy it, then tune the constants at the top.");
    if (lens_tabs_begin(ui, "demo-tabs", active)) {
        lens_tab(ui, "Composition");
        lens_tab(ui, "Atmosphere");
        lens_tab(ui, "Lighting");
        lens_tabs_end(ui);
    }
    lens_separator(ui);
    lens_label(ui, "The library default is the static indicator; this spring ships as source.");
    lens_close(ui);
}

int main(void) {
    static int active = 0;
    printf("iris — tabs spring skin recipe (ADR-0061). Click the tabs; Esc quits.\n"
           "This is a recipe, not a library feature: copy it and make it yours.\n\n");
    return iris_app_run(&(iris_app_config){
        .title = "iris — tabs spring skin (recipe)",
        .width = 560,
        .height = 220,
        .dark = true,
        .build = build,
        .user = &active,
    });
}
