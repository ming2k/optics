/*
 * iris/dnd.h — Cross-platform Drag-and-Drop (DnD) Subsystem.
 *
 * Provides cross-process drag source initiation and drop target reception.
 * See ADR-0086 for architectural background.
 */

#ifndef IRIS_DND_H
#define IRIS_DND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */

#if defined(_WIN32) && !defined(IRIS_STATIC)
#ifdef IRIS_BUILDING
#define IRIS_API __declspec(dllexport)
#else
#define IRIS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define IRIS_API __attribute__((visibility("default")))
#else
#define IRIS_API
#endif

/* ================================================================== */
/*  Drag-and-Drop Actions & Types                                     */
/* ================================================================== */

/* Operations supported during drag / requested upon drop. */
typedef enum iris_dnd_action {
    IRIS_DND_ACTION_NONE = 0,
    IRIS_DND_ACTION_COPY = 1u << 0,
    IRIS_DND_ACTION_MOVE = 1u << 1,
    IRIS_DND_ACTION_LINK = 1u << 2,
    IRIS_DND_ACTION_ASK = 1u << 3,
} iris_dnd_action;

/* Well-known MIME formats for data exchange. */
#define IRIS_DND_MIME_TEXT_PLAIN "text/plain;charset=utf-8"
#define IRIS_DND_MIME_TEXT_URI "text/uri-list"
#define IRIS_DND_MIME_IMAGE_PNG "image/png"

/* Callbacks invoked for outgoing drag sources. */
typedef struct iris_dnd_source_callbacks {
    /* Called when a target requests payload data for a negotiated MIME type.
     * The implementation writes data to `write_fd` and closes it or leaves it
     * for Iris to manage. */
    void (*provide_data)(const char *mime, int write_fd, void *user);

    /* Called when the drop completes successfully with the negotiated action. */
    void (*finished)(iris_dnd_action action_performed, void *user);

    /* Called when the drag is cancelled without a drop. */
    void (*cancelled)(void *user);

    /* Optional user context pointer passed to callbacks. */
    void *user;
} iris_dnd_source_callbacks;

/* Drag source specification passed to iris_dnd_start(). */
typedef struct iris_dnd_source {
    /* Bitmask of allowed actions (e.g. IRIS_DND_ACTION_COPY | IRIS_DND_ACTION_MOVE). */
    uint32_t actions;

    /* Array of offered MIME type strings. */
    const char *const *mime_types;
    uint32_t mime_count;

    /* If text payload is static, can be provided directly without callbacks. */
    const char *static_text;
    size_t static_text_len;

    /* Callbacks for dynamic/streamed data provision. */
    iris_dnd_source_callbacks callbacks;
} iris_dnd_source;

/* ================================================================== */
/*  API Functions                                                     */
/* ================================================================== */

/* Start an outgoing drag session on the current active window.
 * Returns 0 on success, or -1 if unsupported or drag initiation failed. */
IRIS_API int iris_dnd_start(const iris_dnd_source *source);

/* Query whether an outgoing or incoming drag session is currently active. */
IRIS_API bool iris_dnd_is_active(void);

/* Cancel the active drag session if one is in flight. */
IRIS_API void iris_dnd_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_DND_H */
