/* a11y_atspi.c — minimum-viable AT-SPI bridge via sd-bus.
 *
 * Connects to the AT-SPI bus (discovered via org.a11y.Bus on the session
 * bus), registers the application as org.a11y.atspi.Application at
 * /org/a11y/atspi/accessible/root, and exposes a fallback vtable covering
 * /org/a11y/atspi/accessible/<lens_id> for every widget that lens's
 * lens_accessibility_walk reports.
 *
 * Reconciliation (iris_a11y_update):
 *   - Walk lens semantics, deep-copying names/values into bridge-owned
 *     storage (the lens pointers are per-frame arena memory).
 *   - Diff against last frame (iris_a11y__diff, a11y_util.c): emit
 *     ChildrenChanged for added/removed ids, PropertyChange for
 *     name/role/value changes, StateChanged for checked/expanded/selected
 *     flips; focus is pointer-tracked and emits StateChanged:focused.
 *
 * Event wire contract (mirrors at-spi2-atk's atk-adaptor/event.c): all
 * object events are signals on the org.a11y.atspi.Event.Object interface
 * with signature "siiva{sv}" — detail name (e.g. "add"/"remove"/
 * "focused"/"accessible-name"), detail1, detail2, a variant payload
 * (child reference "(so)" for ChildrenChanged; new name "s" for
 * accessible-name; int 0 otherwise), and an (empty) properties dict.
 * This is what orca / pyatspi actually subscribe to through the registry.
 *
 * What AT-SPI clients get today (e.g. orca):
 *   - A read-only widget tree with Name, Role, RoleName, State, Parent,
 *     ChildCount, ChildAtIndex, plus Action (DoAction activates through
 *     lens_a11y_activate — ADR-0062), Text, and Value. Enough for orca to
 *     read the UI aloud, announce focus/tree/name/value changes, echo
 *     typed text (TextChanged deltas), and operate controls.
 *
 * What they can't do yet: set values/selections programmatically, or hear
 * live-region announcements. Those land in subsequent revisions.
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
/*  AT-SPI role/state constants and the lens→AT-SPI mapping live in    */
/*  a11y_util.c (pure, unit-tested against at-spi2-core's              */
/*  atspi-constants.h): iris_a11y__map_role / iris_a11y__role_name /   */
/*  iris_a11y__state_bits and the IRIS_ATSPI_ROLE_* /                  */
/*  IRIS_ATSPI_STATE_* enums come from there.                          */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Per-frame snapshot of the semantic tree                            */
/* ------------------------------------------------------------------ */

#define IRIS_A11Y_MAX_NODES 256

typedef struct {
    iris_a11y__node nodes[IRIS_A11Y_MAX_NODES];
    size_t n;

    /* Previous frame's snapshot, for the diff. */
    iris_a11y__node prev[IRIS_A11Y_MAX_NODES];
    size_t n_prev;

    /* Tracks the currently focused id (0 = none). Focus is pointer-tracked
     * rather than diffed so a focused node that vanishes still produces a
     * focused-off event. */
    lens_id focused;

    /* Set by the walk when the tree exceeds IRIS_A11Y_MAX_NODES. */
    bool truncated;
} a11y_state;

static a11y_state g_state;

static const char *node_string(const iris_a11y__node *n) {
    return n ? n->name : "";
}

/* find_node: returns ptr into g_state.nodes or NULL */
static iris_a11y__node *find_node(lens_id id) {
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
/*  Action.DoAction → lens_a11y_activate (ADR-0062)                    */
/* ------------------------------------------------------------------ */

/* The live lens context, captured each iris_a11y_update. DoAction handlers
 * run on the main thread inside the bus pump, so calling straight into
 * lens is safe; the activation lands next frame like any input. Cleared on
 * shutdown. */
static lens *g_lens_ui = NULL;

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
    iris_a11y__node *n = find_node(id);
    return sd_bus_reply_method_return(m, "s", n ? node_string(n) : "");
}

static int m_get_role(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    const char *path = sd_bus_message_get_path(m);
    lens_id id = 0;
    if (parse_path(path, &id) != 0)
        return sd_bus_reply_method_return(m, "u", (uint32_t)IRIS_ATSPI_ROLE_UNKNOWN);
    iris_atspi_role r;
    if (id == 0) {
        r = IRIS_ATSPI_ROLE_FRAME;
    } else {
        iris_a11y__node *n = find_node(id);
        r = n ? iris_a11y__map_role(n->role) : IRIS_ATSPI_ROLE_UNKNOWN;
    }
    return sd_bus_reply_method_return(m, "u", (uint32_t)r);
}

static int m_get_role_name(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    const char *path = sd_bus_message_get_path(m);
    lens_id id = 0;
    iris_atspi_role r = IRIS_ATSPI_ROLE_UNKNOWN;
    if (parse_path(path, &id) == 0) {
        if (id == 0)
            r = IRIS_ATSPI_ROLE_FRAME;
        else {
            iris_a11y__node *n = find_node(id);
            if (n)
                r = iris_a11y__map_role(n->role);
        }
    }
    return sd_bus_reply_method_return(m, "s", iris_a11y__role_name(r));
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
    iris_a11y__node *n = find_node(id);
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
    /* AT-SPI state is two uint32 bitfields returned as an ARRAY of two
     * uint32 ("au" — the vtable declares it and sd-bus validates outgoing
     * replies against the declaration; a "uu" reply is rejected). Built
     * from lens's LENS_A11Y_* flags by the shared mapper (a11y_util.c). */
    uint32_t lo = 0, hi = 0;
    const char *path = sd_bus_message_get_path(m);
    lens_id id = 0;
    if (parse_path(path, &id) == 0 && id != 0) {
        iris_a11y__node *n = find_node(id);
        if (n)
            iris_a11y__state_bits(n->flags, &lo, &hi);
    } else if (id == 0) {
        /* Root: the not-DISABLED default set (enabled/sensitive/showing/
         * focusable) with no per-widget bits. */
        iris_a11y__state_bits(0, &lo, &hi);
    }

    sd_bus_message *reply = NULL;
    int rc = sd_bus_message_new_method_return(m, &reply);
    if (rc < 0)
        return rc;
    rc = sd_bus_message_open_container(reply, 'a', "u");
    if (rc >= 0)
        rc = sd_bus_message_append(reply, "uu", lo, hi);
    if (rc >= 0)
        rc = sd_bus_message_close_container(reply);
    if (rc < 0) {
        sd_bus_message_unref(reply);
        return rc;
    }
    return sd_bus_send(NULL, reply, NULL);
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
    iris_a11y__node *n = find_node(id);
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
        iris_a11y__node *n = find_node(id);
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
    return sd_bus_reply_method_return(m, "s", "0.0.25");
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
static int p_get_app_id(sd_bus *bus, const char *path, const char *interface, const char *property,
                        sd_bus_message *reply, void *u, sd_bus_error *e) {
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
    SD_BUS_VTABLE_END,
};

/* org.a11y.atspi.Event.Object — the interface AT clients actually
 * subscribe to (orca/pyatspi register "object:<signal>[:<detail>]" with
 * the registry, which forwards signals from this interface). One fixed
 * "siiva{sv}" shape for every member: detail name, detail1, detail2,
 * variant payload, properties dict. Declared here so introspection shows
 * the events each object can emit; emission is in iris_a11y_update. */
static const sd_bus_vtable g_event_object_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_SIGNAL("ChildrenChanged", "siiva{sv}", 0),
    SD_BUS_SIGNAL("PropertyChange", "siiva{sv}", 0),
    SD_BUS_SIGNAL("StateChanged", "siiva{sv}", 0),
    SD_BUS_SIGNAL("TextChanged", "siiva{sv}", 0),
    SD_BUS_VTABLE_END,
};

/* ------------------------------------------------------------------ */
/*  org.a11y.atspi.Action                                              */
/* ------------------------------------------------------------------ */

static int action_get_n(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u;
    (void)e;
    lens_id id = 0;
    iris_a11y__node *n =
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
    iris_a11y__node *n =
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
    iris_a11y__node *n =
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
    iris_a11y__node *n =
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
    iris_a11y__node *n =
        (parse_path(sd_bus_message_get_path(m), &id) == 0 && id != 0) ? find_node(id) : NULL;
    int ok = 0;
    if (n && idx == 0 && iris_a11y__supports_action(n->role) && g_lens_ui) {
        /* ADR-0062: hand the activation to lens's interaction path; it
         * reports clicked for the node next frame (disabled is respected,
         * pointer occlusion is not). No pointer synthesis. */
        lens_a11y_activate(g_lens_ui, id);
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

static iris_a11y__node *node_on_path(sd_bus_message *m) {
    lens_id id = 0;
    if (parse_path(sd_bus_message_get_path(m), &id) != 0 || id == 0)
        return NULL;
    return find_node(id);
}

static const char *text_value_of(iris_a11y__node *n) {
    /* `value` is an array (never NULL); the emptiness test is on [0]. */
    return (n && iris_a11y__supports_text(n->role) && n->value[0] != '\0') ? n->value : "";
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
    iris_a11y__node *n = find_node(id);
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
    rc = sd_bus_get_property_string(session, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus",
                                    "BusAddress", &err, &addr_owned);
    if (rc >= 0 && addr_owned && addr_owned[0] != '\0' && strlen(addr_owned) < sizeof addr_buf) {
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
            rc >= 0 && reply_addr && reply_addr[0] != '\0' && strlen(reply_addr) < sizeof addr_buf;
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

    /* 2. Open a fresh connection directly to the AT-SPI bus: set the
     *    discovered address on a private connection and start it. This
     *    never touches DBUS_SESSION_BUS_ADDRESS (the old setenv dance
     *    raced every other thread reading the environment).
     *    sd_bus_set_bus_client(true) is required: without it the
     *    connection is a raw (non-bus) one and sd_bus_start never sends
     *    the Hello that assigns our unique name. */
    rc = sd_bus_new(&g_a11y_bus);
    if (rc >= 0)
        rc = sd_bus_set_address(g_a11y_bus, addr_buf);
    if (rc >= 0)
        rc = sd_bus_set_bus_client(g_a11y_bus, true);
    if (rc >= 0)
        rc = sd_bus_start(g_a11y_bus);
    if (rc < 0) {
        g_a11y_bus = sd_bus_unref(g_a11y_bus);
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

    /* 7. Event.Object on the root and the fallback prefix: declares the
     *    signals iris_a11y_update emits (ChildrenChanged / PropertyChange /
     *    StateChanged, "siiva{sv}") so introspection advertises them. */
    (void)sd_bus_add_object_vtable(g_a11y_bus, NULL, "/org/a11y/atspi/accessible/root",
                                   "org.a11y.atspi.Event.Object", g_event_object_vtable, NULL);
    (void)sd_bus_add_fallback_vtable(g_a11y_bus, NULL, "/org/a11y/atspi/accessible",
                                     "org.a11y.atspi.Event.Object", g_event_object_vtable, NULL,
                                     NULL);

    /* 8. Register with the AT-SPI registry (links our root into the
     *    desktop-wide accessibility tree). Ignore failures — at-spi2-core
     *    may not be running and we still want object exposure. */
    (void)sd_bus_call_method(g_a11y_bus, "org.a11y.atspi.Registry",
                             "/org/a11y/atspi/accessible/root", "org.a11y.atspi.Socket", "Embed",
                             NULL, NULL, "o", "/org/a11y/atspi/accessible/root");

    /* 9. Some sd_bus builds filter incoming method_calls unless we add an
     *    explicit match. Match every message to us; the vtable dispatch
     *    is the actual filter. */
    (void)sd_bus_match_signal_async(g_a11y_bus, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    return 0;
}

/* Event-loop integration point (src/a11y_internal.h): the backend polls
 * this fd and pumps on signal, so AT-SPI method calls are answered on the
 * iris main thread, serialized with iris_a11y_update(). Not exported. */
int iris_a11y__fd(void) {
    if (!g_a11y_bus)
        return -1;
    int fd = sd_bus_get_fd(g_a11y_bus);
    return fd < 0 ? -1 : fd;
}

short iris_a11y__poll_events(void) {
    if (!g_a11y_bus)
        return 0;
    int ev = sd_bus_get_events(g_a11y_bus);
    return ev < 0 ? 0 : (short)ev;
}

IRIS_API const char *iris_a11y_unique_name(void) {
    return g_a11y_bus ? g_unique : NULL;
}

void iris_a11y__pump(void) {
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
    g_state.truncated = false;
    g_lens_ui = NULL;
}

/* ------------------------------------------------------------------ */
/*  Per-frame walk + diff                                              */
/* ------------------------------------------------------------------ */
static void visit_fn(const lens_semantics *s, flux_rect bounds, lens_id id, lens_id parent,
                     void *user) {
    (void)user;
    (void)bounds; /* no consumer yet (a future Component interface will) */
    if (g_state.n >= IRIS_A11Y_MAX_NODES) {
        g_state.truncated = true;
        return;
    }
    /* Deep copy: s->name / s->value point into lens's per-frame arena and
     * dangle past the next lens_begin; the snapshot must own its strings
     * so AT-SPI method handlers (which run whenever the bus is pumped)
     * never read reclaimed arena memory. */
    iris_a11y__node_fill(&g_state.nodes[g_state.n], s, id, parent, g_state.nodes, g_state.n);
    g_state.n++;
}

/* Emit one org.a11y.atspi.Event.Object signal — the "siiva{sv}" shape
 * AT clients subscribe to (see the file header). `any_type` selects the
 * variant payload: "(so)" (child reference: any_s = bus name, any_o =
 * object path), "s" (string), "u" (uint32), or "i" (int32 any_i). The
 * trailing properties dict is always empty. */
static void emit_object_event(lens_id path_id, const char *member, const char *detail,
                              int32_t detail1, int32_t detail2, const char *any_type,
                              const char *any_s, const char *any_o, int32_t any_i) {
    if (!g_a11y_bus)
        return;
    char path[64];
    format_path(path, sizeof path, path_id);

    sd_bus_message *sig = NULL;
    int rc =
        sd_bus_message_new_signal(g_a11y_bus, &sig, path, "org.a11y.atspi.Event.Object", member);
    if (rc < 0)
        return;
    rc = sd_bus_message_append(sig, "sii", detail, detail1, detail2);
    if (rc >= 0)
        rc = sd_bus_message_open_container(sig, 'v', any_type);
    if (rc >= 0) {
        if (strcmp(any_type, "(so)") == 0)
            rc = sd_bus_message_append(sig, "(so)", any_s, any_o);
        else if (strcmp(any_type, "s") == 0)
            rc = sd_bus_message_append(sig, "s", any_s ? any_s : "");
        else if (strcmp(any_type, "u") == 0)
            rc = sd_bus_message_append(sig, "u", (uint32_t)any_i);
        else
            rc = sd_bus_message_append(sig, "i", any_i);
    }
    if (rc >= 0)
        rc = sd_bus_message_close_container(sig); /* v  */
    if (rc >= 0)
        rc = sd_bus_message_open_container(sig, 'a', "{sv}");
    if (rc >= 0)
        rc = sd_bus_message_close_container(sig); /* a{sv} */
    if (rc >= 0)
        (void)sd_bus_send(NULL, sig, NULL); /* sig carries the bus ref */
    sd_bus_message_unref(sig);
}

/* StateChanged:focused — detail1 is 1/0, payload an int 0 (the shape
 * at-spi2-atk emits; clients re-read the object). */
static void emit_state_changed(lens_id id, const char *state_name, int enabled) {
    emit_object_event(id, "StateChanged", state_name, enabled, 0, "i", NULL, NULL, 0);
}

/* Translate one diff event (a11y_util.c) into its Event.Object signal. */
static void emit_diff_event(const iris_a11y__event *ev) {
    switch (ev->kind) {
    case IRIS_A11Y__EV_ADD:
    case IRIS_A11Y__EV_REMOVE: {
        /* Emitted on the parent; the payload is the child reference. For a
         * removal the child path no longer resolves — clients drop their
         * cached copy, which is the point. */
        const iris_a11y__node *child = ev->node ? ev->node : ev->prev_node;
        char child_path[64];
        format_path(child_path, sizeof child_path, child->id);
        emit_object_event(ev->parent, "ChildrenChanged",
                          ev->kind == IRIS_A11Y__EV_ADD ? "add" : "remove", ev->index, 0, "(so)",
                          g_unique, child_path, 0);
        break;
    }
    case IRIS_A11Y__EV_NAME:
        emit_object_event(ev->id, "PropertyChange", "accessible-name", 0, 0, "s", ev->node->name,
                          NULL, 0);
        break;
    case IRIS_A11Y__EV_VALUE:
        /* at-spi2-atk sends an int-0 payload here too; clients re-query
         * the Value interface for the new CurrentValue. */
        emit_object_event(ev->id, "PropertyChange", "accessible-value", 0, 0, "i", NULL, NULL, 0);
        break;
    case IRIS_A11Y__EV_ROLE:
        emit_object_event(ev->id, "PropertyChange", "accessible-role", 0, 0, "u", NULL, NULL,
                          (int32_t)iris_a11y__map_role(ev->role));
        break;
    case IRIS_A11Y__EV_STATE_ON:
    case IRIS_A11Y__EV_STATE_OFF: {
        const char *name = ev->state == IRIS_ATSPI_STATE_CHECKED    ? "checked"
                           : ev->state == IRIS_ATSPI_STATE_EXPANDED ? "expanded"
                           : ev->state == IRIS_ATSPI_STATE_SELECTED ? "selected"
                                                                    : "unknown";
        emit_state_changed(ev->id, name, ev->kind == IRIS_A11Y__EV_STATE_ON ? 1 : 0);
        break;
    }
    case IRIS_A11Y__EV_TEXT: {
        /* at-spi2-atk shape: detail1 = code-point offset of the edit,
         * detail2 = run length in code points, payload = the run itself.
         * A replace (both runs non-empty) is delete-then-insert. */
        const iris_a11y__text_delta *d = &ev->text;
        char buf[IRIS_A11Y__NODE_VALUE_MAX + 1];
        if (d->removed > 0) {
            size_t nb = d->removed_bytes < sizeof buf - 1 ? d->removed_bytes : sizeof buf - 1;
            memcpy(buf, d->removed_text, nb);
            buf[nb] = '\0';
            emit_object_event(ev->id, "TextChanged", "delete", d->offset, d->removed, "s", buf,
                              NULL, 0);
        }
        if (d->inserted > 0) {
            size_t nb = d->inserted_bytes < sizeof buf - 1 ? d->inserted_bytes : sizeof buf - 1;
            memcpy(buf, d->inserted_text, nb);
            buf[nb] = '\0';
            emit_object_event(ev->id, "TextChanged", "insert", d->offset, d->inserted, "s", buf,
                              NULL, 0);
        }
        break;
    }
    }
}

IRIS_API int iris_a11y_update(lens *ui) {
    if (!g_a11y_bus)
        return -1;
    if (!ui)
        return -1;
    g_lens_ui = ui; /* for Action.DoAction → lens_a11y_activate (ADR-0062) */

    /* Retire the current frame into `prev`, then walk the new one. */
    g_state.n_prev = g_state.n;
    memcpy(g_state.prev, g_state.nodes, g_state.n * sizeof g_state.prev[0]);
    g_state.n = 0;
    g_state.truncated = false;
    lens_accessibility_walk(ui, visit_fn, NULL);
    if (g_state.truncated) {
        /* Once per process: a tree this big is a real defect, but a warning
         * per frame would drown the log. */
        static bool warned = false;
        if (!warned) {
            fprintf(stderr,
                    "iris a11y: semantic tree exceeds %d nodes; the accessibility "
                    "view is silently truncated past that (fix the host UI or raise "
                    "IRIS_A11Y_MAX_NODES)\n",
                    IRIS_A11Y_MAX_NODES);
            warned = true;
        }
    }

    /* Diff prev vs cur and emit the corresponding Event.Object signals.
     * Worst case is every node removed + re-added plus a property change
     * each; the event buffer is sized for that. */
    static iris_a11y__event events[IRIS_A11Y_MAX_NODES * 2 + 64];
    size_t n_events = iris_a11y__diff(g_state.prev, g_state.n_prev, g_state.nodes, g_state.n,
                                      events, sizeof events / sizeof events[0]);
    if (n_events > sizeof events / sizeof events[0])
        n_events = sizeof events / sizeof events[0];
    for (size_t i = 0; i < n_events; i++)
        emit_diff_event(&events[i]);

    /* Track focus changes and emit StateChanged:focused. Pointer-tracked
     * (not diffed) so a focused node that vanishes still unfocuses. */
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
