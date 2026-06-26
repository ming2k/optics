/* a11y_util.h — pure, headless helpers for the AT-SPI bridge.
 *
 * This translation unit compiles UNCONDITIONALLY (no sd-bus dependency) so
 * the role / value logic can be unit-tested and reused by the D-Bus bridge
 * (a11y_atspi.c). Keeping the pure predicates out of the sd-bus-gated source
 * also means the stub build (no libsystemd) still has a coherent answer to
 * "which interfaces does this role support" if a future host needs it.
 */
#ifndef IRIS_A11Y_UTIL_H
#define IRIS_A11Y_UTIL_H

#include <lens/lens.h>
#include <stdbool.h>

/* Roles that expose org.a11y.atspi.Action — the AT can invoke them.
 * Buttons, checkboxes, radios, and disclosure toggles all do something when
 * activated; labels / panels / scroll areas do not. */
bool iris_a11y__supports_action(lens_role r);

/* Roles that expose org.a11y.atspi.Text — single- or multi-line text input. */
bool iris_a11y__supports_text(lens_role r);

/* Roles that expose org.a11y.atspi.Value — anything with a scalar range. */
bool iris_a11y__supports_value(lens_role r);

/* The localized-independent action name AT-SPI clients announce for the
 * primary action of `r` ("click" / "toggle" / "press"), or NULL when the
 * role is not actionable. */
const char *iris_a11y__action_name(lens_role r);

/* Parse a lens semantic `value` string (a slider readout like "1.5" or
 * "42") into a double. Returns 0.0 for NULL / empty / non-numeric input. */
double iris_a11y__parse_value(const char *s);

/* Count UTF-8 code points (what AT-SPI Text offsets are measured in) in `s`.
 * Returns 0 for NULL. */
int iris_a11y__char_count_utf8(const char *s);

#endif /* IRIS_A11Y_UTIL_H */
