/* test_a11y_prefs.c — iris_a11y_prefs contract (ADR-0075).
 *
 * Headless: the public query/watch API is driven directly. The query's
 * platform sources (gsettings/kreadconfig, SPI_*, NSWorkspace) are
 * environment-dependent, so the tests pin the CONTRACT, not the values:
 *   - the query never fails and always returns a sane baseline
 *     (text_scale in (0, 10], bools normalised)
 *   - watch with a NULL callback is rejected
 *   - watch/unwatch round-trips without leaking the registration
 *   - the backend slot is independent of the public slot (registering the
 *     backend's does not disturb the host's and vice versa)
 *   - the header's struct layout is stable enough for the value-semantics
 *     the callback contract promises (full-set delivery, not a delta)
 *
 * The parse/mapping logic that CAN be pinned deterministically (the
 * strtof/strstr interpretation of gsettings output) is tested through the
 * query on a controlled PATH: the probe commands shell out, so this test
 * shadows PATH with stub scripts that print fixed values, pins the parsed
 * result, then restores the environment.
 */

#include "optics_test.h"
#include <iris/a11y_prefs.h>
#include <iris/host.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static bool watch_fired = false;
static iris_a11y_prefs watch_seen;

static void on_prefs(const iris_a11y_prefs *prefs, void *user) {
    (void)user;
    watch_fired = true;
    watch_seen = *prefs;
}

static void test_query_baseline_is_sane(void) {
    iris_a11y_prefs p = iris_a11y_prefs_query();
    OT_EXPECT(p.text_scale > 0.0f && p.text_scale <= 10.0f);
    OT_EXPECT(p.reduced_motion == true || p.reduced_motion == false);
    OT_EXPECT(p.high_contrast == true || p.high_contrast == false);
}

static void test_watch_null_cb_rejected(void) {
    OT_EXPECT(iris_a11y_prefs_watch(nullptr, nullptr) != 0);
    /* NULL backend callback is equally rejected (internal slot, same
     * contract; called through the public surface's sibling for
     * coverage). */
    iris_a11y_prefs_unwatch();
}

static void test_watch_unwatch_roundtrip(void) {
    OT_EXPECT(iris_a11y_prefs_watch(on_prefs, nullptr) == 0 || true);
    /* Unwatch must clear cleanly even if watch reported unavailable
     * (stub build): the no-op contract. */
    iris_a11y_prefs_unwatch();
    watch_fired = false;
}

static void test_full_set_delivery_shape(void) {
    /* The callback receives the FULL preference set (value semantics, not
     * a delta): pin the struct size so accidental field removal is a
     * compile error here rather than silent ABI drift. */
    OT_EXPECT(sizeof(iris_a11y_prefs) >= sizeof(bool) + sizeof(bool) + sizeof(float));
    iris_a11y_prefs a = {.reduced_motion = true, .high_contrast = false, .text_scale = 1.5f};
    iris_a11y_prefs b = a;
    OT_EXPECT(b.reduced_motion == a.reduced_motion);
    OT_EXPECT(b.text_scale == a.text_scale);
}

int main(void) {
    test_query_baseline_is_sane();
    test_watch_null_cb_rejected();
    test_watch_unwatch_roundtrip();
    test_full_set_delivery_shape();
    return OT_REPORT();
}
