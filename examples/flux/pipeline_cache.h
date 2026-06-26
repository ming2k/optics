/*
 * examples/pipeline_cache.h — a file-based pipeline-cache helper.
 *
 * flux itself never touches the filesystem: the library owns the
 * in-memory VkPipelineCache, the consumer owns cross-session storage
 * (see flux_pipeline_cache_load_fn / _save_fn in <flux/core.h>). This
 * header is the reference file-backed implementation — copy it into
 * your own project, or use it verbatim from the examples.
 *
 * Usage:
 *
 *     flux_pipeline_cache_file cache = FLUX_PIPELINE_CACHE_FILE_INIT;
 *     flux_pipeline_cache_file_set_path(&cache, "my_cache.bin");
 *
 *     flux_device_desc ddesc = FLUX_DEVICE_DESC_INIT;
 *     ...
 *     ddesc.pipeline_cache_load     = flux_pipeline_cache_file_load;
 *     ddesc.pipeline_cache_save     = flux_pipeline_cache_file_save;
 *     ddesc.pipeline_cache_userdata = &cache;
 *
 * Save is atomic (tmp + rename); the parent directory is created on
 * first write. Failures are silent — the cache is an optimisation.
 */
#ifndef EXAMPLES_PIPELINE_CACHE_H
#define EXAMPLES_PIPELINE_CACHE_H

#include <flux/core.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FLUX_PIPELINE_CACHE_FILE_MAX_PATH 1024

typedef struct flux_pipeline_cache_file {
    char path[FLUX_PIPELINE_CACHE_FILE_MAX_PATH];
} flux_pipeline_cache_file;

#define FLUX_PIPELINE_CACHE_FILE_INIT {{0}}

static inline void flux_pipeline_cache_file_set_path(flux_pipeline_cache_file *c,
                                                     const char *path) {
    snprintf(c->path, sizeof(c->path), "%s", path);
}

/* Resolve a default path the way the example expects: an explicit
 * $FLUX_PIPELINE_CACHE override wins; otherwise the file lives under
 * $XDG_CACHE_HOME/flux or $HOME/.cache/flux. Returns false (empty
 * path) when nothing usable is set, leaving persistence disabled. */
static inline bool flux_pipeline_cache_file_set_default_path(flux_pipeline_cache_file *c,
                                                             const char *name) {
    const char *override = getenv("FLUX_PIPELINE_CACHE");
    if (override && override[0]) {
        snprintf(c->path, sizeof(c->path), "%s", override);
        return true;
    }
    const char *base = getenv("XDG_CACHE_HOME");
    if (!base || !base[0]) {
        base = getenv("HOME");
        if (!base || !base[0]) {
            c->path[0] = '\0';
            return false;
        }
        snprintf(c->path, sizeof(c->path), "%s/.cache/flux/%s", base, name);
    } else {
        snprintf(c->path, sizeof(c->path), "%s/flux/%s", base, name);
    }
    return true;
}

/* mkdir -p of the parent directory of `path`. Best-effort. */
static inline void flux_pipeline_cache_file_mkdir_parent(const char *path) {
    char buf[FLUX_PIPELINE_CACHE_FILE_MAX_PATH];
    snprintf(buf, sizeof(buf), "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf)
        return;
    *slash = '\0';
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(buf, 0700);
            *p = '/';
        }
    }
    (void)mkdir(buf, 0700);
}

/* flux_pipeline_cache_load_fn: read a prior blob. Returns a malloc'd
 * buffer the library frees; *out_size = 0 / NULL when no cache. */
static inline void *flux_pipeline_cache_file_load(void *userdata, size_t *out_size) {
    *out_size = 0;
    const flux_pipeline_cache_file *c = userdata;
    if (!c || c->path[0] == '\0')
        return NULL;

    FILE *fp = fopen(c->path, "rb");
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long n = ftell(fp);
    if (n <= 0 || n > 64 * 1024 * 1024) {
        fclose(fp);
        return NULL;
    } /* sanity cap: 64 MiB */
    rewind(fp);
    void *buf = malloc((size_t)n);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    if (got != (size_t)n) {
        free(buf);
        return NULL;
    }
    *out_size = got;
    return buf;
}

/* flux_pipeline_cache_save_fn: store the blob atomically. */
static inline void flux_pipeline_cache_file_save(void *userdata, const void *data, size_t size) {
    flux_pipeline_cache_file *c = userdata;
    if (!c || c->path[0] == '\0' || size == 0)
        return;

    flux_pipeline_cache_file_mkdir_parent(c->path);

    char tmp[FLUX_PIPELINE_CACHE_FILE_MAX_PATH + 32];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%d", c->path, (int)getpid());
    if (n < 0 || n >= (int)sizeof(tmp))
        return;

    FILE *fp = fopen(tmp, "wb");
    if (!fp)
        return;
    size_t wrote = fwrite(data, 1, size, fp);
    int flush = fflush(fp);
    int close = fclose(fp);
    if (wrote != size || flush != 0 || close != 0) {
        (void)unlink(tmp);
        return;
    }
    if (rename(tmp, c->path) != 0) {
        (void)unlink(tmp);
    }
}

#endif /* EXAMPLES_PIPELINE_CACHE_H */
