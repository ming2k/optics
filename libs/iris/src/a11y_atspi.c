/* a11y_atspi.c — minimum-viable AT-SPI bridge via sd-bus.
 *
 * Connects to the AT-SPI bus (discovered via org.a11y.Bus on the session
 * bus), registers the application as org.a11y.atspi.Application at
 * /org/a11y/atspi/accessible/root, and exposes a fallback vtable covering
 * /org/a11y/atspi/accessible/<lens_id> for every widget that lens's
 * lens_accessibility_walk reports.
 *
 * Reconciliation (iris_a11y_update):
 *   - Walk lens semantics, build a snapshot for this frame.
 *   - Diff against last frame: emit ChildrenChanged for added/removed IDs.
 *   - Track the focused id; emit StateChanged:focused when it changes.
 *
 * What AT-SPI clients get today (e.g. orca):
 *   - A read-only widget tree with Name, Role, RoleName, State, Parent,
 *     ChildCount, ChildAtIndex. Enough for orca to read the UI aloud and
 *     announce focus changes.
 *
 * What they can't do yet: invoke actions (button clicks), read text-field
 * contents via the Text interface, or query slider values via Value.
 * Those land in subsequent revisions.
 *
 * Build gate: IRIS_HAVE_ATSPI (defined when libsystemd is available).
 */

#include "a11y_internal.h"

#ifdef IRIS_HAVE_ATSPI

/* _GNU_SOURCE is provided by the build system (add_project_arguments). */
#include "a11y_util.h"
#include <iris/a11y.h>
#include <lens/lens.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

/* ------------------------------------------------------------------ */
/*  AT-SPI role constants (subset of atspi/atspi-constants.h)         */
/* ------------------------------------------------------------------ */

enum atspi_role {
    ATSPI_ROLE_UNKNOWN = 0,
    ATSPI_ROLE_PUSH_BUTTON = 41,
    ATSPI_ROLE_CHECK_BOX = 5,
    ATSPI_ROLE_RADIO_BUTTON = 42,
    ATSPI_ROLE_SLIDER = 76,
    ATSPI_ROLE_LABEL = 29,
    ATSPI_ROLE_PANEL = 28,
    ATSPI_ROLE_SCROLL_PANE = 64,
    ATSPI_ROLE_ENTRY = 16,
    ATSPI_ROLE_TEXT = 89,
    ATSPI_ROLE_TOGGLE_BUTTON = 86,
    ATSPI_ROLE_MENU = 24,
    ATSPI_ROLE_FRAME = 73, /* the application root         */
};

/* AT-SPI State enum bit positions (subset of atspi/atspi-constants.h).
 * State is two uint32 bitfields (64 bits); bits 0-31 live in `lo`,
 * bits 32-63 in `hi`. */
enum atspi_state {
    ATSPI_STATE_CHECKED = 3,
    ATSPI_STATE_FOCUSABLE = 5,
    ATSPI_STATE_FOCUSED = 8,
    ATSPI_STATE_ENABLED = 9,
    ATSPI_STATE_SENSITIVE = 23,
    ATSPI_STATE_EXPANDED = 41, /* -> hi bit 9  */
    ATSPI_STATE_SHOWING = 48,  /* -> hi bit 16 */
};

/* Pack a state bit into the (lo, hi) pair. */
static inline void state_set(uint32_t *lo, uint32_t *hi, int bit) {
    if (bit < 32)
        *lo |= (1u << bit);
    else
        *hi |= (1u << (bit - 32));
}

/* Map lens role → AT-SPI role. */
static enum atspi_role map_role(lens_role r) {
    switch (r) {
    case LENS_ROLE_LABEL:
        return ATSPI_ROLE_LABEL;
    case LENS_ROLE_BUTTON:
        return ATSPI_ROLE_PUSH_BUTTON;
    case LENS_ROLE_CHECKBOX:
        return ATSPI_ROLE_CHECK_BOX;
    case LENS_ROLE_SLIDER:
        return ATSPI_ROLE_SLIDER;
    case LENS_ROLE_DISCLOSURE:
        return ATSPI_ROLE_TOGGLE_BUTTON;
    case LENS_ROLE_SCROLLAREA:
        return ATSPI_ROLE_SCROLL_PANE;
    case LENS_ROLE_TEXTFIELD:
        return ATSPI_ROLE_ENTRY;
    case LENS_ROLE_TEXTAREA:
        return ATSPI_ROLE_TEXT;
    case LENS_ROLE_MENU:
        return ATSPI_ROLE_MENU;
    case LENS_ROLE_RADIO:
        return ATSPI_ROLE_RADIO_BUTTON;
    case LENS_ROLE_GROUP:
        return ATSPI_ROLE_PANEL;
    case LENS_ROLE_NONE:
    default:
        return ATSPI_ROLE_UNKNOWN;
    }
}

static const char *role_name(enum atspi_role r) {
    switch (r) {
    case ATSPI_ROLE_PUSH_BUTTON:
        return "push button";
    case ATSPI_ROLE_CHECK_BOX:
        return "check box";
    case ATSPI_ROLE_RADIO_BUTTON:
        return "radio button";
    case ATSPI_ROLE_SLIDER:
        return "slider";
    case ATSPI_ROLE_LABEL:
        return "label";
    case ATSPI_ROLE_PANEL:
        return "panel";
    case ATSPI_ROLE_SCROLL_PANE:
        return "scroll pane";
    case ATSPI_ROLE_ENTRY:
        return "entry";
    case ATSPI_ROLE_TEXT:
        return "text";
    case ATSPI_ROLE_TOGGLE_BUTTON:
        return "toggle button";
    case ATSPI_ROLE_MENU:
        return "menu";
    case ATSPI_ROLE_FRAME:
        return "frame";
    default:
        return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/*  Per-frame snapshot of the semantic tree                            */
/* ------------------------------------------------------------------ */

#define IRIS_A11Y_MAX_NODES 256

typedef struct {
    lens_id id;
    lens_id parent;
    lens_role role;
    const char *name;
    const char *value;
    uint32_t flags;
    flux_rect bounds; /* last frame's solved rect — used for DoAction */
} a11y_node;

typedef struct {
    a11y_node nodes[IRIS_A11Y_MAX_NODES];
    size_t n;

    /* Index from lens_id → position in `nodes`. Built per update. */
    struct {
        lens_id id;
        size_t idx;
    } lookup[IRIS_A11Y_MAX_NODES];
    size_t n_lookup;

    /* Tracks the previous frame's ids so we can diff. */
    lens_id prev_ids[IRIS_A11Y_MAX_NODES];
    size_t n_prev;

    /* Tracks the currently focused id (0 = none). */
    lens_id focused;
} a11y_state;

static a11y_state g_state;

static const char *node_string(a11y_node *n) {
    if (!n)
        return "";
    return n->name ? n->name : "";
}

/* find_node: returns ptr into g_state.nodes or NULL */
static a11y_node *find_node(lens_id id) {
    if (id == 0)
        return NULL; /* root */
    for (size_t i = 0; i < g_state.n; i++) {
        if (g_state.nodes[i].id == id)
            return &g_state.nodes[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  D-Bus connection + registration                                    */
/* ------------------------------------------------------------------ */

static sd_bus *g_a11y_bus = NULL;
static char g_unique[128] = {0}; /* our unique bus name on a11y bus  */

/* Path helpers. Root is /org/a11y/atspi/accessible/root; widgets use
 * /org/a11y/atspi/accessible/<lens_id>. */
static void format_path(char *buf, size_t cap, lens_id id) {
    if (id == 0)
        snprintf(buf, cap, "/org/a11y/atspi/accessible/root");
    else
        snprintf(buf, cap, "/org/a11y/atspi/accessible/%llu", (unsigned long long)id);
}

static int parse_path(const char *path, lens_id *out) {
    const char *prefix = "/org/a11y/atspi/accessible/";
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0)
        return -1;
    const char *rest = path + plen;
    if (strcmp(rest, "root") == 0) {
        *out = 0;
        return 0;
    }
    char *end = NULL;
    unsigned long long v = strtoull(rest, &end, 10);
    if (end == rest || *end != '\0')
        return -1;
    *out = (lens_id)v;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Action.DoAction click-synthesis seam                              */
/* ------------------------------------------------------------------ */

/* A pending click requested by org.a11y.atspi.Action.DoAction. Single slot:
 * AT-SPI actions are synchronous/sequential, so one outstanding click is
 * enough. The app loop drains it via iris_a11y__take_pending_click() and
 * synthesizes a lens_input press+release at the widget's centre next frame.
 * (lens is input-driven — it has no programmatic "activate" API, so we must
 * route the action back through the input queue.) */
static flux_point g_pending_click_pt;
static bool g_pending_click_active;

/* Internal seam consumed by the platform backend (app_wayland.c). Not part
 * of the public <iris/a11y.h> surface. */
bool iris_a11y__take_pending_click(flux_point *out) {
    if (!g_pending_click_active)
        return false;
    g_pending_click_active = false;
    if (out)
        *out = g_pending_click_pt;
    return true;
}

/* Queue a click at widget `id`'s centre. Called from the Action vtable's
 * DoAction handler. */
static void queue_click(lens_id id) {
    a11y_node *n = find_node(id);
    if (!n || n->bounds.w <= 0.0f || n->bounds.h <= 0.0f)
        return;
    g_pending_click_pt.x = n->bounds.x + n->bounds.w * 0.5f;
    g_pending_click_pt.y = n->bounds.y + n->bounds.h * 0.5f;
    g_pending_click_active = true;
}

/* ------------------------------------------------------------------ */
/*  org.a11y.atspi.Accessible method handlers                          */
/* ------------------------------------------------------------------ */

/* GetName / GetDescription / GetRole / GetRoleName / GetParent /
 * GetChildCount / GetChildAtIndex / GetState / GetIndexInParent /
 * GetApplication / GetAttributes */
static int m_get_name(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    const char *path = sd_bus_message_get_path(m);
    lens_id id = 0;
    if (parse_path(path, &id) != 0)
        return sd_bus_reply_method_return(m, "s", "");
    if (id == 0)
        return sd_bus_reply_method_return(m, "s", "iris application");
    a11y_node *n = find_node(id);
    return sd_bus_reply_method_return(m, "s", n ? node_string(n) : "");
}

static int m_get_role(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    const char *path = sd_bus_message_get_path(m);
    lens_id id = 0;
    if (parse_path(path, &id) != 0)
        return sd_bus_reply_method_return(m, "u", (uint32_t)ATSPI_ROLE_UNKNOWN);
    enum atspi_role r;
    if (id == 0) {
        r = ATSPI_ROLE_FRAME;
    } else {
        a11y_node *n = find_node(id);
        r = n ? map_role(n->role) : ATSPI_ROLE_UNKNOWN;
    }
    return sd_bus_reply_method_return(m, "u", (uint32_t)r);
}

static int m_get_role_name(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    const char *path = sd_bus_message_get_path(m);
    lens_id id = 0;
    enum atspi_role r = ATSPI_ROLE_UNKNOWN;
    if (parse_path(path, &id) == 0) {
        if (id == 0)
            r = ATSPI_ROLE_FRAME;
        else {
            a11y_node *n = find_node(id);
            if (n)
                r = map_role(n->role);
        }
    }
    return sd_bus_reply_method_return(m, "s", role_name(r));
}

static int m_get_description(sd_bus_message *m, void *u, sd_bus_error *e) {
    /* Description isn't exposed by lens semantics yet; return empty. */
    (void)u;
    (void)e;
    return sd_bus_reply_method_return(m, "s", "");
}

static int m_get_parent(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    const char *path = sd_bus_message_get_path(m);
    lens_id id = 0;
    if (parse_path(path, &id) != 0 || id == 0) {
        /* Root's parent is the registry (we return a null reference). */
        return sd_bus_reply_method_return(m, "so", "", "/org/a11y/atspi/null");
    }
    a11y_node *n = find_node(id);
    lens_id pid = n ? n->parent : 0;
    char ppath[64];
    format_path(ppath, sizeof ppath, pid);
    return sd_bus_reply_method_return(m, "so", g_unique, ppath);
}

static int m_get_child_count(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    const char *path = sd_bus_message_get_path(m);
    lens_id id = 0;
    if (parse_path(path, &id) != 0)
        return sd_bus_reply_method_return(m, "i", (int32_t)0);
    /* Count nodes whose parent == this id (or root for parent==0). */
    int32_t count = 0;
    for (size_t i = 0; i < g_state.n; i++) {
        if (g_state.nodes[i].parent == id)
            count++;
    }
    return sd_bus_reply_method_return(m, "i", count);
}

static int m_get_child_at_index(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    int32_t want = 0;
    int rc = sd_bus_message_read(m, "i", &want);
    if (rc < 0)
        return rc;

    const char *path = sd_bus_message_get_path(m);
    lens_id id = 0;
    if (parse_path(path, &id) != 0)
        return sd_bus_reply_method_return(m, "so", "", "/org/a11y/atspi/null");

    int32_t idx = 0;
    for (size_t i = 0; i < g_state.n; i++) {
        if (g_state.nodes[i].parent != id)
            continue;
        if (idx == want) {
            char cpath[64];
            format_path(cpath, sizeof cpath, g_state.nodes[i].id);
            return sd_bus_reply_method_return(m, "so", g_unique, cpath);
        }
        idx++;
    }
    return sd_bus_reply_method_return(m, "so", "", "/org/a11y/atspi/null");
}

static int m_get_state(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    /* AT-SPI state is two uint32 bitfields (64 bits). Build it from
     * lens's lens_a11y flags + focus tracking. */
    uint32_t lo = 0, hi = 0;
    const char *path = sd_bus_message_get_path(m);
    lens_id id = 0;
    if (parse_path(path, &id) == 0 && id != 0) {
        a11y_node *n = find_node(id);
        if (n) {
            /* Every widget is focusable+enabled+sensitive+showing unless
             * the lens DISABLED flag is set. */
            if (!(n->flags & LENS_A11Y_DISABLED)) {
                state_set(&lo, &hi, ATSPI_STATE_FOCUSABLE);
                state_set(&lo, &hi, ATSPI_STATE_ENABLED);
                state_set(&lo, &hi, ATSPI_STATE_SENSITIVE);
            }
            state_set(&lo, &hi, ATSPI_STATE_SHOWING);
            if (n->flags & LENS_A11Y_FOCUSED)
                state_set(&lo, &hi, ATSPI_STATE_FOCUSED);
            if (n->flags & LENS_A11Y_CHECKED)
                state_set(&lo, &hi, ATSPI_STATE_CHECKED);
            if (n->flags & LENS_A11Y_EXPANDED)
                state_set(&lo, &hi, ATSPI_STATE_EXPANDED);
        }
    } else if (id == 0) {
        state_set(&lo, &hi, ATSPI_STATE_SHOWING);
        state_set(&lo, &hi, ATSPI_STATE_ENABLED);
    }
    return sd_bus_reply_method_return(m, "uu", lo, hi);
}

static int m_get_application(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    return sd_bus_reply_method_return(m, "so", g_unique, "/org/a11y/atspi/accessible/root");
}

static int m_get_index_in_parent(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    const char *path = sd_bus_message_get_path(m);
    lens_id id = 0;
    if (parse_path(path, &id) != 0 || id == 0)
        return sd_bus_reply_method_return(m, "i", (int32_t)0);
    a11y_node *n = find_node(id);
    if (!n)
        return sd_bus_reply_method_return(m, "i", (int32_t)-1);
    int32_t idx = 0;
    for (size_t i = 0; i < g_state.n; i++) {
        if (g_state.nodes[i].parent != n->parent)
            continue;
        if (g_state.nodes[i].id == id)
            return sd_bus_reply_method_return(m, "i", idx);
        idx++;
    }
    return sd_bus_reply_method_return(m, "i", (int32_t)-1);
}

static int m_get_attributes(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    /* Return an empty a{ss} dict. */
    sd_bus_message *reply = NULL;
    int rc = sd_bus_message_new_method_return(m, &reply);
    if (rc < 0)
        return rc;
    rc = sd_bus_message_open_container(reply, 'a', "{ss}");
    if (rc < 0) {
        sd_bus_message_unref(reply);
        return rc;
    }
    rc = sd_bus_message_close_container(reply);
    if (rc < 0) {
        sd_bus_message_unref(reply);
        return rc;
    }
    return sd_bus_send(NULL, reply, NULL);
}

static int m_get_interfaces(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    /* The interface set depends on the widget's role: every object exposes
     * Accessible; actionable controls add Action; text inputs add Text;
     * sliders add Value. The root is always Accessible-only. */
    lens_role role = LENS_ROLE_NONE;
    lens_id id = 0;
    const char *path = sd_bus_message_get_path(m);
    if (parse_path(path, &id) == 0 && id != 0) {
        a11y_node *n = find_node(id);
        if (n)
            role = n->role;
    }

    sd_bus_message *reply = NULL;
    int rc = sd_bus_message_new_method_return(m, &reply);
    if (rc < 0)
        return rc;
    rc = sd_bus_message_open_container(reply, 'a', "s");
    if (rc < 0) {
        sd_bus_message_unref(reply);
        return rc;
    }

    rc = sd_bus_message_append(reply, "s", "org.a11y.atspi.Accessible");
    if (rc < 0) {
        sd_bus_message_unref(reply);
        return rc;
    }
    if (iris_a11y__supports_action(role)) {
        rc = sd_bus_message_append(reply, "s", "org.a11y.atspi.Action");
        if (rc < 0) {
            sd_bus_message_unref(reply);
            return rc;
        }
    }
    if (iris_a11y__supports_text(role)) {
        rc = sd_bus_message_append(reply, "s", "org.a11y.atspi.Text");
        if (rc < 0) {
            sd_bus_message_unref(reply);
            return rc;
        }
    }
    if (iris_a11y__supports_value(role)) {
        rc = sd_bus_message_append(reply, "s", "org.a11y.atspi.Value");
        if (rc < 0) {
            sd_bus_message_unref(reply);
            return rc;
        }
    }

    rc = sd_bus_message_close_container(reply);
    if (rc < 0) {
        sd_bus_message_unref(reply);
        return rc;
    }
    return sd_bus_send(NULL, reply, NULL);
}

/* org.a11y.atspi.Application: GetLocale, GetToolkitName, GetVersion */
static int m_get_toolkit_name(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    return sd_bus_reply_method_return(m, "s", "iris");
}
static int m_get_version(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    return sd_bus_reply_method_return(m, "s", "0.0.11");
}
static int m_get_locale(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    /* lctype = 0 (messages), return a reasonable default. */
    int32_t lctype = 0;
    (void)sd_bus_message_read(m, "i", &lctype);
    (void)lctype;
    const char *loc = getenv("LANG");
    if (!loc)
        loc = "C";
    return sd_bus_reply_method_return(m, "s", loc);
}

/* org.a11y.atspi.Application: Id property — AT clients use this to identify
 * the application. We return the toolkit name; the bus unique name is also
 * commonly used. The data pointer is unused (the value is constant). */
static int p_get_app_id(sd_bus *bus, const char *path, const char *interface,
                        const char *property, sd_bus_message *reply, void *u,
                        sd_bus_error *e) {
    (void)bus;
    (void)path;
    (void)interface;
    (void)property;
    (void)u;
    (void)e;
    return sd_bus_message_append(reply, "s", "iris");
}

/* ------------------------------------------------------------------ */
/*  Vtables                                                            */
/* ------------------------------------------------------------------ */

#define HANDLER(name) ((sd_bus_message_handler_t)(name))

static const sd_bus_vtable g_accessible_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetName", NULL, "s", HANDLER(m_get_name), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetDescription", NULL, "s", HANDLER(m_get_description),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetRole", NULL, "u", HANDLER(m_get_role), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetRoleName", NULL, "s", HANDLER(m_get_role_name), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetParent", NULL, "so", HANDLER(m_get_parent), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetChildCount", NULL, "i", HANDLER(m_get_child_count),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetChildAtIndex", "i", "so", HANDLER(m_get_child_at_index),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetState", NULL, "au", HANDLER(m_get_state), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetApplication", NULL, "so", HANDLER(m_get_application),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetIndexInParent", NULL, "i", HANDLER(m_get_index_in_parent),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetAttributes", NULL, "a{ss}", HANDLER(m_get_attributes),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetInterfaces", NULL, "as", HANDLER(m_get_interfaces),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("ChildrenChanged", "si", 0),
    SD_BUS_SIGNAL("StateChanged", "siu", 0),
    SD_BUS_VTABLE_END,
};

/* ------------------------------------------------------------------ */
/*  org.a11y.atspi.Action                                              */
/* ------------------------------------------------------------------ */

static int action_get_n(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    lens_id id = 0;
    a11y_node *n =
        (parse_path(sd_bus_message_get_path(m), &id) == 0 && id != 0) ? find_node(id) : NULL;
    int32_t count = (n && iris_a11y__supports_action(n->role)) ? 1 : 0;
    return sd_bus_reply_method_return(m, "i", count);
}

static int action_named(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    int32_t idx = 0;
    (void)sd_bus_message_read(m, "i", &idx);
    lens_id id = 0;
    a11y_node *n =
        (parse_path(sd_bus_message_get_path(m), &id) == 0 && id != 0) ? find_node(id) : NULL;
    const char *nm = (n && idx == 0) ? iris_a11y__action_name(n->role) : NULL;
    return sd_bus_reply_method_return(m, "s", nm ? nm : "");
}

static int action_description(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    int32_t idx = 0;
    (void)sd_bus_message_read(m, "i", &idx);
    lens_id id = 0;
    a11y_node *n =
        (parse_path(sd_bus_message_get_path(m), &id) == 0 && id != 0) ? find_node(id) : NULL;
    const char *desc =
        (n && idx == 0 && iris_a11y__supports_action(n->role)) ? "activate the control" : "";
    return sd_bus_reply_method_return(m, "s", desc);
}

static int action_keybinding(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    int32_t idx = 0;
    (void)sd_bus_message_read(m, "i", &idx);
    /* lens does not surface keybindings per widget; report none. */
    return sd_bus_reply_method_return(m, "s", "");
}

static int action_get_actions(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    lens_id id = 0;
    a11y_node *n =
        (parse_path(sd_bus_message_get_path(m), &id) == 0 && id != 0) ? find_node(id) : NULL;
    sd_bus_message *reply = NULL;
    int rc = sd_bus_message_new_method_return(m, &reply);
    if (rc < 0)
        return rc;
    rc = sd_bus_message_open_container(reply, 'a', "(is)");
    if (rc < 0) {
        sd_bus_message_unref(reply);
        return rc;
    }
    if (n && iris_a11y__supports_action(n->role)) {
        const char *nm = iris_a11y__action_name(n->role);
        rc = sd_bus_message_open_container(reply, 'r', "is");
        if (rc >= 0)
            rc = sd_bus_message_append(reply, "is", 0, nm ? nm : "click");
        if (rc >= 0)
            rc = sd_bus_message_close_container(reply);
        if (rc < 0) {
            sd_bus_message_unref(reply);
            return rc;
        }
    }
    rc = sd_bus_message_close_container(reply);
    if (rc < 0) {
        sd_bus_message_unref(reply);
        return rc;
    }
    return sd_bus_send(NULL, reply, NULL);
}

static int action_do(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    int32_t idx = 0;
    int rc = sd_bus_message_read(m, "i", &idx);
    if (rc < 0)
        return rc;
    lens_id id = 0;
    a11y_node *n =
        (parse_path(sd_bus_message_get_path(m), &id) == 0 && id != 0) ? find_node(id) : NULL;
    int ok = 0;
    if (n && idx == 0 && iris_a11y__supports_action(n->role)) {
        queue_click(id); /* drained by the platform loop next frame */
        ok = 1;
    }
    return sd_bus_reply_method_return(m, "b", ok);
}

static const sd_bus_vtable g_action_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetNActions", NULL, "i", HANDLER(action_get_n), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetName", "i", "s", HANDLER(action_named), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetDescription", "i", "s", HANDLER(action_description),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetKeyBinding", "i", "s", HANDLER(action_keybinding),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetActions", NULL, "a(is)", HANDLER(action_get_actions),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DoAction", "i", "b", HANDLER(action_do), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

/* ------------------------------------------------------------------ */
/*  org.a11y.atspi.Text                                                */
/* ------------------------------------------------------------------ */

/* Slice `value` by code-point range [start, end) into a static buffer.
 * end < 0 (AT-SPI convention) means "to end". Runs on the single pump
 * thread, so a shared static buffer is safe. */
static const char *text_slice(const char *value, int32_t start, int32_t end) {
    static char buf[1024];
    if (!value)
        value = "";
    int total = iris_a11y__char_count_utf8(value);
    if (start < 0)
        start = 0;
    if (end < 0 || end > total)
        end = total;
    if (start > end)
        start = end;

    size_t out = 0;
    int cp = -1; /* -1: no code point seen yet; ++ on each UTF-8 lead byte */
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if ((*p & 0xC0) != 0x80)
            cp++;
        if (cp >= end)
            break;
        if (cp >= start && out + 1 < sizeof buf)
            buf[out++] = (char)*p;
    }
    buf[out] = '\0';
    return buf;
}

static a11y_node *node_on_path(sd_bus_message *m) {
    lens_id id = 0;
    if (parse_path(sd_bus_message_get_path(m), &id) != 0 || id == 0)
        return NULL;
    return find_node(id);
}

static const char *text_value_of(a11y_node *n) {
    return (n && iris_a11y__supports_text(n->role) && n->value) ? n->value : "";
}

static int text_char_count(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    return sd_bus_reply_method_return(
        m, "i", (int32_t)iris_a11y__char_count_utf8(text_value_of(node_on_path(m))));
}

static int text_get(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    int32_t start = 0, end = 0;
    int rc = sd_bus_message_read(m, "ii", &start, &end);
    if (rc < 0)
        return rc;
    return sd_bus_reply_method_return(m, "s",
                                      text_slice(text_value_of(node_on_path(m)), start, end));
}

static int text_get_all(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    return sd_bus_reply_method_return(m, "s", text_value_of(node_on_path(m)));
}

static int text_caret(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    /* lens exposes a caret rect (for IME) but not a byte/char offset; report
     * the caret at the end so orca reads the full contents. */
    return sd_bus_reply_method_return(
        m, "i", (int32_t)iris_a11y__char_count_utf8(text_value_of(node_on_path(m))));
}

static int text_set_caret(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    int32_t off = 0;
    (void)sd_bus_message_read(m, "i", &off);
    /* Read-only from the AT side: lens has no set-caret-by-offset API. */
    return sd_bus_reply_method_return(m, "b", 0);
}

static int text_n_selections(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    return sd_bus_reply_method_return(m, "i", (int32_t)0);
}

static int text_get_selection(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    int32_t num = 0;
    (void)sd_bus_message_read(m, "i", &num);
    return sd_bus_reply_method_return(m, "ii", (int32_t)0, (int32_t)0);
}

static int text_no_op_b(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    /* AddSelection / RemoveSelection / SetSelection — lens exposes no
     * selection API; report unsupported. */
    return sd_bus_reply_method_return(m, "b", 0);
}

static const sd_bus_vtable g_text_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetCharacterCount", NULL, "i", HANDLER(text_char_count),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetText", "ii", "s", HANDLER(text_get), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetTextAll", NULL, "s", HANDLER(text_get_all), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetCaretOffset", NULL, "i", HANDLER(text_caret), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetCaretOffset", "i", "b", HANDLER(text_set_caret), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetNSelections", NULL, "i", HANDLER(text_n_selections),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetSelection", "i", "ii", HANDLER(text_get_selection),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AddSelection", "ii", "b", HANDLER(text_no_op_b), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RemoveSelection", "i", "b", HANDLER(text_no_op_b), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetSelection", "iii", "b", HANDLER(text_no_op_b), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

/* ------------------------------------------------------------------ */
/*  org.a11y.atspi.Value                                               */
/* ------------------------------------------------------------------ */

static double value_of(lens_id id) {
    a11y_node *n = find_node(id);
    if (!n || !iris_a11y__supports_value(n->role))
        return 0.0;
    return iris_a11y__parse_value(n->value);
}

static int value_get_current(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    lens_id id = 0;
    (void)parse_path(sd_bus_message_get_path(m), &id);
    return sd_bus_reply_method_return(m, "d", value_of(id));
}

static int value_get_zero(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    /* lens does not expose slider min / max / step in semantics. */
    return sd_bus_reply_method_return(m, "d", 0.0);
}

static int value_set_current(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    double v = 0.0;
    (void)sd_bus_message_read(m, "d", &v);
    /* lens sliders are input-driven; no set-value seam yet. */
    return sd_bus_reply_method_return(m, "");
}

/* Property getters (modern pyatspi reads org.freedesktop.DBus.Properties). */
static int value_prop_current(sd_bus *bus, const char *path, const char *iface,
                              const char *property, sd_bus_message *reply, void *userdata,
                              sd_bus_error *ret_error) {
    (void)bus;
    (void)iface;
    (void)property;
    (void)userdata;
    (void)ret_error;
    lens_id id = 0;
    (void)parse_path(path, &id);
    return sd_bus_message_append(reply, "d", value_of(id));
}

static int value_prop_zero(sd_bus *bus, const char *path, const char *iface, const char *property,
                           sd_bus_message *reply, void *userdata, sd_bus_error *ret_error) {
    (void)bus;
    (void)path;
    (void)iface;
    (void)property;
    (void)userdata;
    (void)ret_error;
    return sd_bus_message_append(reply, "d", 0.0);
}

static const sd_bus_vtable g_value_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("CurrentValue", "d", value_prop_current, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("MinimumValue", "d", value_prop_zero, 0, 0),
    SD_BUS_PROPERTY("MaximumValue", "d", value_prop_zero, 0, 0),
    SD_BUS_PROPERTY("MinimumIncrement", "d", value_prop_zero, 0, 0),
    /* Deprecated method forms older AT clients (pyatspi < 2.40) still call. */
    SD_BUS_METHOD("GetCurrentValue", NULL, "d", HANDLER(value_get_current),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetCurrentValue", "d", "", HANDLER(value_set_current),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetMinimumValue", NULL, "d", HANDLER(value_get_zero),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetMaximumValue", NULL, "d", HANDLER(value_get_zero),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetMinimumIncrement", NULL, "d", HANDLER(value_get_zero),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

static const sd_bus_vtable g_application_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetToolkitName", NULL, "s", HANDLER(m_get_toolkit_name),
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetVersion", NULL, "s", HANDLER(m_get_version), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetLocale", "i", "s", HANDLER(m_get_locale), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("Id", "s", p_get_app_id, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END,
};

/* ------------------------------------------------------------------ */
/*  Init / shutdown                                                    */
/* ------------------------------------------------------------------ */

IRIS_API int iris_a11y_init(void) {
    if (g_a11y_bus)
        return 0;

    /* 1. Query the session bus for the AT-SPI bus address. */
    sd_bus *session = NULL;
    int rc = sd_bus_open_user(&session);
    if (rc < 0)
        return -1;

    char addr_buf[1024] = {0};
    char *addr_owned = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    rc = sd_bus_get_property_string(
        session,
        "org.a11y.Bus",
        "/org/a11y/bus",
        "org.a11y.Bus",
        "BusAddress",
        &err,
        &addr_owned
    );
    if (rc >= 0 && addr_owned && addr_owned[0] != '\0'
        && strlen(addr_owned) < sizeof addr_buf) {
        memcpy(addr_buf, addr_owned, strlen(addr_owned) + 1);
    } else {
        /* Some setups expose it via GetAddress method instead. */
        free(addr_owned);
        addr_owned = NULL;
        sd_bus_error_free(&err);
        sd_bus_message *reply = NULL;
        rc = sd_bus_call_method(session, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus",
                                "GetAddress", &err, &reply, "");
        if (rc < 0) {
            sd_bus_error_free(&err);
            sd_bus_unref(session);
            return -1;
        }
        const char *reply_addr = NULL;
        rc = sd_bus_message_read(reply, "s", &reply_addr);
        bool valid =
            rc >= 0 && reply_addr && reply_addr[0] != '\0'
            && strlen(reply_addr) < sizeof addr_buf;
        if (valid)
            memcpy(addr_buf, reply_addr, strlen(reply_addr) + 1);
        sd_bus_message_unref(reply);
        if (!valid) {
            sd_bus_error_free(&err);
            sd_bus_unref(session);
            return -1;
        }
    }
    free(addr_owned);
    sd_bus_error_free(&err);
    sd_bus_unref(session);

    /* 2. Open a fresh connection directly to the AT-SPI bus. The most robust
     *    way across sd-bus versions is to swap DBUS_SESSION_BUS_ADDRESS
     *    temporarily and let sd_bus_open_user do the full handshake. */
    const char *old_env = getenv("DBUS_SESSION_BUS_ADDRESS");
    char *old_env_copy = old_env ? strdup(old_env) : NULL;
    setenv("DBUS_SESSION_BUS_ADDRESS", addr_buf, 1);
    rc = sd_bus_open_user(&g_a11y_bus);
    if (old_env_copy) {
        setenv("DBUS_SESSION_BUS_ADDRESS", old_env_copy, 1);
        free(old_env_copy);
    } else {
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
    }
    if (rc < 0) {
        g_a11y_bus = NULL;
        return -1;
    }

    /* 3. Capture our unique name; AT-SPI references use it. */
    const char *unique = NULL;
    if (sd_bus_get_unique_name(g_a11y_bus, &unique) >= 0 && unique) {
        strncpy(g_unique, unique, sizeof g_unique - 1);
        g_unique[sizeof g_unique - 1] = '\0';
    }

    /* 4. Register the root accessible + Application interface. */
    rc = sd_bus_add_object_vtable(g_a11y_bus, NULL, "/org/a11y/atspi/accessible/root",
                                  "org.a11y.atspi.Application", g_application_vtable, NULL);
    (void)rc;
    rc = sd_bus_add_object_vtable(g_a11y_bus, NULL, "/org/a11y/atspi/accessible/root",
                                  "org.a11y.atspi.Accessible", g_accessible_vtable, NULL);
    (void)rc;

    /* 5. Fallback vtable for /org/a11y/atspi/accessible/<lens_id> paths.
     *    This catches every widget without us registering each one. */
    rc = sd_bus_add_fallback_vtable(g_a11y_bus, NULL, "/org/a11y/atspi/accessible",
                                    "org.a11y.atspi.Accessible", g_accessible_vtable, NULL, NULL);
    (void)rc;

    /* 6. Action / Text / Value interfaces — same fallback path prefix, one
     *    vtable each. Each method handler no-ops for roles that do not
     *    support the interface, so orca querying e.g. Text on a button gets
     *    a benign empty answer rather than an unknown-method error. */
    (void)sd_bus_add_fallback_vtable(g_a11y_bus, NULL, "/org/a11y/atspi/accessible",
                                     "org.a11y.atspi.Action", g_action_vtable, NULL, NULL);
    (void)sd_bus_add_fallback_vtable(g_a11y_bus, NULL, "/org/a11y/atspi/accessible",
                                     "org.a11y.atspi.Text", g_text_vtable, NULL, NULL);
    (void)sd_bus_add_fallback_vtable(g_a11y_bus, NULL, "/org/a11y/atspi/accessible",
                                     "org.a11y.atspi.Value", g_value_vtable, NULL, NULL);

    /* 6. Register with the AT-SPI registry (links our root into the
     *    desktop-wide accessibility tree). Ignore failures — at-spi2-core
     *    may not be running and we still want object exposure. */
    (void)sd_bus_call_method(g_a11y_bus, "org.a11y.atspi.Registry",
                             "/org/a11y/atspi/accessible/root", "org.a11y.atspi.Socket", "Embed",
                             NULL, NULL, "o", "/org/a11y/atspi/accessible/root");

    /* 7. Some sd_bus builds filter incoming method_calls unless we add an
     *    explicit match. Match every message to us; the vtable dispatch
     *    is the actual filter. */
    (void)sd_bus_match_signal_async(g_a11y_bus, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    return 0;
}

IRIS_API int iris_a11y_fd(void) {
    if (!g_a11y_bus)
        return -1;
    int fd = sd_bus_get_fd(g_a11y_bus);
    return fd < 0 ? -1 : fd;
}

IRIS_API short iris_a11y_poll_events(void) {
    if (!g_a11y_bus)
        return 0;
    int ev = sd_bus_get_events(g_a11y_bus);
    return ev < 0 ? 0 : (short)ev;
}

IRIS_API const char *iris_a11y_unique_name(void) {
    return g_a11y_bus ? g_unique : NULL;
}

IRIS_API void iris_a11y_pump(void) {
    if (!g_a11y_bus)
        return;
    for (;;) {
        int rc = sd_bus_process(g_a11y_bus, NULL);
        if (rc <= 0)
            break;
    }
}

IRIS_API void iris_a11y_shutdown(void) {
    if (!g_a11y_bus)
        return;
    sd_bus_unref(g_a11y_bus);
    g_a11y_bus = NULL;
    g_unique[0] = '\0';
    g_state.n = 0;
    g_state.n_prev = 0;
    g_state.focused = 0;
    g_pending_click_active = false;
}

/* ------------------------------------------------------------------ */
/*  Per-frame walk + diff                                              */
/* ------------------------------------------------------------------ */

static void visit_fn(const lens_semantics *s, flux_rect bounds, lens_id id, lens_id parent,
                     void *user) {
    (void)user;
    if (g_state.n >= IRIS_A11Y_MAX_NODES)
        return;
    a11y_node *n = &g_state.nodes[g_state.n++];
    n->id = id;
    n->parent = parent;
    n->role = s->role;
    n->name = s->name;
    n->value = s->value;
    n->flags = s->flags;
    n->bounds = bounds;
}

static int was_present_last_frame(lens_id id) {
    for (size_t i = 0; i < g_state.n_prev; i++)
        if (g_state.prev_ids[i] == id)
            return 1;
    return 0;
}

static void emit_children_changed(lens_id parent_id, lens_id child_id, int add_remove,
                                  int index_in_parent) {
    if (!g_a11y_bus)
        return;
    char child_path[64];
    format_path(child_path, sizeof child_path, child_id);
    char parent_path[64];
    format_path(parent_path, sizeof parent_path, parent_id);

    /* Signal shape: "si" — string (the AT-SPI reference as "bus:path"),
     * int (add=1/remove=-1, followed by index encoded in the same int). */
    char ref[256];
    snprintf(ref, sizeof ref, "%s:%s", g_unique, child_path);
    int detail = add_remove * 1000 + index_in_parent;

    (void)sd_bus_emit_signal(g_a11y_bus, parent_path, "org.a11y.atspi.Accessible",
                             "ChildrenChanged", "si", ref, detail);
}

static void emit_state_changed(lens_id id, const char *state_name, int enabled) {
    if (!g_a11y_bus)
        return;
    char path[64];
    format_path(path, sizeof path, id);
    /* Signal shape: "siu" — string (state name), int (enabled), uint (detail). */
    (void)sd_bus_emit_signal(g_a11y_bus, path, "org.a11y.atspi.Accessible", "StateChanged", "siu",
                             state_name, enabled, 0u);
}

IRIS_API int iris_a11y_update(lens *ui) {
    if (!g_a11y_bus)
        return -1;
    if (!ui)
        return -1;

    /* Snapshot last frame before rebuilding. */
    g_state.n_prev = g_state.n;
    for (size_t i = 0; i < g_state.n; i++)
        g_state.prev_ids[i] = g_state.nodes[i].id;

    /* Walk the current frame. */
    g_state.n = 0;
    lens_accessibility_walk(ui, visit_fn, NULL);

    /* Diff: emit ChildrenChanged for added ids. */
    for (size_t i = 0; i < g_state.n; i++) {
        a11y_node *n = &g_state.nodes[i];
        if (!was_present_last_frame(n->id)) {
            emit_children_changed(n->parent, n->id, 1, (int)i);
        }
    }

    /* Track focus changes and emit StateChanged:focused. */
    lens_id now_focused = 0;
    for (size_t i = 0; i < g_state.n; i++) {
        if (g_state.nodes[i].flags & LENS_A11Y_FOCUSED) {
            now_focused = g_state.nodes[i].id;
            break;
        }
    }
    if (now_focused != g_state.focused) {
        if (g_state.focused)
            emit_state_changed(g_state.focused, "focused", 0);
        if (now_focused)
            emit_state_changed(now_focused, "focused", 1);
        g_state.focused = now_focused;
    }

    return 0;
}

#endif /* IRIS_HAVE_ATSPI */
