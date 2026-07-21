/* fuzz_main.c — standalone driver for fuzz harnesses.
 *
 * Provides main() when the harness is built without libFuzzer. Reads a
 * single test case from argv[1] (or stdin if no arg) and forwards it to
 * LLVMFuzzerTestOneInput once. This is the repro path for a crash found
 * by libFuzzer / AFL: copy the offending file, run the executable on it,
 * observe the same crash under a debugger.
 *
 * When libFuzzer is linked (LIBFUZZER=1 at configure time) this object is
 * omitted; libFuzzer supplies its own main(). */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
    (void)argc;
    FILE *f = stdin;
    if (argv[1]) {
        f = fopen(argv[1], "rb");
        if (!f) {
            perror(argv[1]);
            return 2;
        }
    }

    /* Read up to 16 MiB; fuzzer inputs that size are rare, and capping
     * the buffer keeps an accidental huge file from exhausting memory. */
    size_t cap = 16 * 1024 * 1024;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        fprintf(stderr, "oom\n");
        if (f != stdin)
            fclose(f);
        return 3;
    }
    size_t n = fread(buf, 1, cap, f);
    if (f != stdin)
        fclose(f);

    int rc = LLVMFuzzerTestOneInput(buf, n);
    free(buf);
    return rc;
}
