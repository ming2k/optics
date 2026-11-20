/*
 * iris/dnd.h — Cross-platform Drag-and-Drop (DnD) Subsystem.
 *
 * Provides cross-process drag source initiation and drop target reception.
 * See ADR-0086 for architectural background.
 */

#ifndef OPTICS_WINDOW_DND_H
#define OPTICS_WINDOW_DND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */

#if defined(_WIN32) && !defined(OPTICS_WINDOW_STATIC)
#ifdef OPTICS_WINDOW_BUILDING
#define OPTICS_WINDOW_API __declspec(dllexport)
#else
#define OPTICS_WINDOW_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define OPTICS_WINDOW_API __attribute__((visibility("default")))
#else
#define OPTICS_WINDOW_API
#endif

/* ================================================================== */
/*  Drag-and-Drop Actions & Types                                     */
/* ================================================================== */

/* Operations supported during drag / requested upon drop. */
typedef enum optics_window_dnd_action : uint32_t {
    OPTICS_WINDOW_DND_ACTION_NONE = 0,
    OPTICS_WINDOW_DND_ACTION_COPY = 1u << 0,
    OPTICS_WINDOW_DND_ACTION_MOVE = 1u << 1,
    OPTICS_WINDOW_DND_ACTION_LINK = 1u << 2,
    OPTICS_WINDOW_DND_ACTION_ASK = 1u << 3,
} optics_window_dnd_action;

/* Well-known MIME formats for data exchange. */
#define OPTICS_WINDOW_DND_MIME_TEXT_PLAIN "text/plain;charset=utf-8"
#define OPTICS_WINDOW_DND_MIME_TEXT_URI "text/uri-list"
#define OPTICS_WINDOW_DND_MIME_IMAGE_PNG "image/png"

/* Callbacks invoked for outgoing drag sources. */
typedef struct optics_window_dnd_source_callbacks {
    /* Called when a target requests payload data for a negotiated MIME type.
     * The implementation writes data to `write_fd` and closes it or leaves it
     * for Iris to manage. */
    void (*provide_data)(const char *mime, int write_fd, void *user);

    /* Called when the drop completes successfully with the negotiated action. */
    void (*finished)(optics_window_dnd_action action_performed, void *user);

    /* Called when the drag is cancelled without a drop. */
    void (*cancelled)(void *user);

    /* Optional user context pointer passed to callbacks. */
    void *user;
} optics_window_dnd_source_callbacks;

/* Drag source specification passed to iris_dnd_start(). */
typedef struct optics_window_dnd_source {
    /* Bitmask of allowed actions (e.g. IRIS_DND_ACTION_COPY | IRIS_DND_ACTION_MOVE). */
    uint32_t actions;

    /* Array of offered MIME type strings. */
    const char *const *mime_types;
    uint32_t mime_count;

    /* If text payload is static, can be provided directly without callbacks. */
    const char *static_text;
    size_t static_text_len;

    /* Callbacks for dynamic/streamed data provision. */
    optics_window_dnd_source_callbacks callbacks;
} optics_window_dnd_source;

/* ================================================================== */
/*  API Functions                                                     */
/* ================================================================== */

/* Start an outgoing drag session on the current active window.
 * Returns 0 on success, or -1 if unsupported or drag initiation failed. */
OPTICS_WINDOW_API int optics_window_dnd_start(const optics_window_dnd_source *source);

/* Query whether an outgoing or incoming drag session is currently active. */
OPTICS_WINDOW_API bool optics_window_dnd_is_active(void);

/* Cancel the active drag session if one is in flight. */
OPTICS_WINDOW_API void optics_window_dnd_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_DND_H */
