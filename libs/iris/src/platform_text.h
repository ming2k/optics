/* platform_text.h — shared UTF-8 helpers for the platform backends.
 *
 * Every backend funnels native text (IME commits, preedits, clipboard,
 * drops) into the same fixed-size lens_input buffers. Truncating those at a
 * raw byte count can split a multi-byte sequence and produce invalid UTF-8,
 * which downstream consumers (D-Bus string marshalling, text shapers) are
 * entitled to reject. These helpers keep every truncation on a code-point
 * boundary so all three backends behave identically (the Win32 backend's
 * local utf8_floor_boundary was the reference behaviour).
 *
 * Pure and dependency-free; compiles on every platform so the logic is
 * unit-testable headlessly (tests/iris/test_platform_text.c).
 */
#ifndef IRIS_PLATFORM_TEXT_H
#define IRIS_PLATFORM_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Clamp a UTF-8 string of `len` bytes to at most `cap` bytes without
 * cutting a multi-byte sequence in half. Returns the new length (<= cap). */
size_t iris_utf8_floor_boundary(const char *s, size_t len, size_t cap);

/* Append up to `n` bytes of NUL-terminated-safe UTF-8 `src` to the
 * NUL-terminated buffer `dst` (capacity `cap`, including the NUL), never
 * splitting a code point. Returns the number of bytes appended. */
size_t iris_utf8_append(char *dst, size_t cap, const char *src, size_t n);

/* Copy NUL-terminated UTF-8 `src` into `dst` (capacity `cap`, including the
 * NUL), truncating on a code-point boundary when it does not fit. */
void iris_utf8_copy(char *dst, size_t cap, const char *src);

/* Compare-and-update "memento" for report-only-on-change protocols (e.g.
 * text-input-v3 set_surrounding_text): keeps a heap copy of the last
 * reported (text, cursor) in the out-params and returns true exactly when
 * (text, len, cursor) differs from it — meaning the caller should
 * re-report. On allocation failure the memento is left unchanged and true
 * is returned (report now, retry the copy next time). */
bool iris_text_memento_update(char **saved, size_t *saved_len, uint32_t *saved_cursor,
                              const char *text, size_t len, uint32_t cursor);

/* Forget the memento (frees the heap copy). Call when the reporting
 * session ends, so the next session re-reports even identical text. */
void iris_text_memento_clear(char **saved, size_t *saved_len, uint32_t *saved_cursor);

#endif /* IRIS_PLATFORM_TEXT_H */
