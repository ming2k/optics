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

#include <stddef.h>

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

#endif /* IRIS_PLATFORM_TEXT_H */
