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

/* Is `cp` a code point the backends must deliver as COMMITTED TEXT
 * (lens_input.text_utf8)? False for control characters: the C0 range
 * (Return \r, Tab \t, Backspace \b, …) AND the DEL character 0x7f.
 *
 * DEL is the subtle one: xkb maps the Delete keysym (0xffff) and Win32
 * maps Ctrl+Backspace's WM_CHAR to exactly U+007F, so without the second
 * half of the test every Delete press would ALSO deliver "\x7f" as text —
 * the key event already carries the intent (LENS_KEY_DELETE), and the
 * invisible byte would corrupt editor buffers. Cocoa's characters filter
 * has always excluded it ("Control characters (Return "\r", Tab "\t",
 * DEL 0x7f) are not text"); this predicate pins the same rule for all
 * three backends. C1 controls (0x80..0x9f) arrive as text via IME/WM_CHAR
 * in good faith (e.g. U+0085 NEL from a paste), so they stay text.
 *
 * One predicate, three backends: the keyboard contract
 * (platform_internal.h) requires them to agree on what is text. */
bool iris_cp_is_text(uint32_t cp);

#endif /* IRIS_PLATFORM_TEXT_H */
