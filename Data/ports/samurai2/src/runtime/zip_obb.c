#include "zip_obb.h"
#include "puff.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    ZIP_NAME_MAX = 192,
    ZIP_MAX_UNCOMP = 8 * 1024 * 1024,
    ZIP_MAX_CONCAT = 32 * 1024 * 1024,
    ZIP_MAX_SPLITS = 64
};

struct zip_entry {
    char name[ZIP_NAME_MAX];
    uint32_t local_off;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint16_t method;
};

struct zip_catalog {
    char path[512];
    struct zip_entry *entries;
    size_t count;
    int ready;
};

static pthread_mutex_t zip_lock = PTHREAD_MUTEX_INITIALIZER;
static struct zip_catalog obb_cat;
static struct zip_catalog apk_cat;

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int read_fully(FILE *fp, void *buf, size_t n)
{
    return n == 0U || fread(buf, 1, n, fp) == n;
}

static int load_catalog(struct zip_catalog *cat, const char *path)
{
    FILE *fp;
    long size;
    unsigned char tail[65557 + 22];
    size_t tail_len;
    long scan;
    uint32_t cd_off;
    uint16_t entries;
    size_t i;
    unsigned char hdr[46];

    if (cat->ready && strcmp(cat->path, path) == 0)
        return 0;
    free(cat->entries);
    cat->entries = NULL;
    cat->count = 0;
    cat->ready = 0;
    cat->path[0] = '\0';

    fp = fopen(path, "rb");
    if (fp == NULL)
        return -1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    size = ftell(fp);
    if (size < 22) {
        fclose(fp);
        return -1;
    }
    tail_len = (size_t)size < sizeof(tail) ? (size_t)size : sizeof(tail);
    if (fseek(fp, size - (long)tail_len, SEEK_SET) != 0 ||
        !read_fully(fp, tail, tail_len)) {
        fclose(fp);
        return -1;
    }
    scan = (long)tail_len - 22;
    while (scan >= 0) {
        if (tail[scan] == 'P' && tail[scan + 1] == 'K' &&
            tail[scan + 2] == 5 && tail[scan + 3] == 6)
            break;
        scan -= 1;
    }
    if (scan < 0) {
        fclose(fp);
        return -1;
    }
    entries = rd16(tail + scan + 10);
    cd_off = rd32(tail + scan + 16);
    if (entries == 0 || cd_off >= (uint32_t)size) {
        fclose(fp);
        return -1;
    }
    cat->entries = calloc(entries, sizeof(*cat->entries));
    if (cat->entries == NULL) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, (long)cd_off, SEEK_SET) != 0) {
        free(cat->entries);
        cat->entries = NULL;
        fclose(fp);
        return -1;
    }
    for (i = 0; i < entries; i++) {
        uint16_t method;
        uint16_t namelen;
        uint16_t extra;
        uint16_t comment;
        uint32_t comp;
        uint32_t uncomp;
        uint32_t local_off;
        char name[ZIP_NAME_MAX];
        size_t copy;

        if (!read_fully(fp, hdr, sizeof(hdr)) ||
            hdr[0] != 'P' || hdr[1] != 'K' || hdr[2] != 1 || hdr[3] != 2) {
            break;
        }
        method = rd16(hdr + 10);
        comp = rd32(hdr + 20);
        uncomp = rd32(hdr + 24);
        namelen = rd16(hdr + 28);
        extra = rd16(hdr + 30);
        comment = rd16(hdr + 32);
        local_off = rd32(hdr + 42);
        copy = namelen < ZIP_NAME_MAX - 1U ? namelen : ZIP_NAME_MAX - 1U;
        if (copy > 0 && !read_fully(fp, name, copy))
            break;
        name[copy] = '\0';
        if (namelen > copy && fseek(fp, (long)(namelen - copy), SEEK_CUR) != 0)
            break;
        if (extra + comment > 0 &&
            fseek(fp, (long)extra + (long)comment, SEEK_CUR) != 0)
            break;
        if (copy == 0 || name[copy - 1U] == '/')
            continue;
        snprintf(cat->entries[cat->count].name, ZIP_NAME_MAX, "%s", name);
        cat->entries[cat->count].local_off = local_off;
        cat->entries[cat->count].comp_size = comp;
        cat->entries[cat->count].uncomp_size = uncomp;
        cat->entries[cat->count].method = method;
        cat->count += 1U;
    }
    fclose(fp);
    snprintf(cat->path, sizeof(cat->path), "%s", path);
    cat->ready = 1;
    printf("G8-ZIP catalog %s entries=%zu\n", path, cat->count);
    return 0;
}

static int split_archive_path(const char *path, char *archive, size_t archive_size,
                              const char **inner)
{
    const char *ext;

    if (path == NULL)
        return -1;
    ext = strstr(path, ".obb/");
    if (ext == NULL)
        ext = strstr(path, ".apk/");
    if (ext == NULL)
        return -1;
    {
        size_t arch_len = (size_t)(ext - path) + 4U;
        if (arch_len + 1U > archive_size)
            return -1;
        memcpy(archive, path, arch_len);
        archive[arch_len] = '\0';
    }
    *inner = ext + 5;
    if ((*inner)[0] == '\0')
        return -1;
    return 0;
}

static struct zip_catalog *catalog_for_archive(const char *archive)
{
    const char *obb = getenv("SG_OBB");
    const char *apk = getenv("SG_APK");
    const char *root = getenv("SG_ROOT");
    char full[512];

    if (strstr(archive, ".obb") != NULL) {
        const char *use = archive;
        if (access(use, R_OK) != 0 && obb != NULL && obb[0] != '\0')
            use = obb;
        if (load_catalog(&obb_cat, use) != 0)
            return NULL;
        return &obb_cat;
    }
    if (strstr(archive, ".apk") != NULL) {
        const char *use = archive;
        if (access(use, R_OK) != 0) {
            if (apk != NULL && apk[0] == '/')
                use = apk;
            else if (root != NULL && apk != NULL)
                snprintf(full, sizeof(full), "%s/%s", root, apk);
            else
                full[0] = '\0';
            if (full[0] != '\0')
                use = full;
        }
        if (load_catalog(&apk_cat, use) != 0)
            return NULL;
        return &apk_cat;
    }
    return NULL;
}

static const struct zip_entry *find_entry(struct zip_catalog *cat,
                                          const char *inner)
{
    size_t i;

    if (cat == NULL || inner == NULL)
        return NULL;
    while (inner[0] == '/')
        inner += 1;
    for (i = 0; i < cat->count; i++) {
        if (strcmp(cat->entries[i].name, inner) == 0)
            return &cat->entries[i];
    }
    return NULL;
}

static int skip_entry(const struct zip_entry *entry)
{
    size_t n;
    if (entry == NULL)
        return 1;
    n = strlen(entry->name);
    if (n >= 4U && strcmp(entry->name + n - 4U, ".mp4") == 0)
        return 1;
    if (entry->uncomp_size > ZIP_MAX_UNCOMP)
        return 1;
    if (entry->method != 0 && entry->method != 8)
        return 1;
    return 0;
}

static int local_data_offset(FILE *fp, const struct zip_entry *entry,
                             uint32_t *data_off)
{
    unsigned char local[30];
    uint16_t namelen;
    uint16_t extra;

    if (fseek(fp, (long)entry->local_off, SEEK_SET) != 0 ||
        !read_fully(fp, local, sizeof(local)))
        return -1;
    if (local[0] != 'P' || local[1] != 'K' || local[2] != 3 || local[3] != 4)
        return -1;
    namelen = rd16(local + 26);
    extra = rd16(local + 28);
    *data_off = entry->local_off + 30U + namelen + extra;
    return 0;
}

/*
 * Unity Android FileUtil: a name without ".splitN" is the concatenation of
 * name.split0 + name.split1 + ... when the unsplit zip entry is missing.
 * FMOD/AudioClip ask for sharedassetsN.resource; the OBB only stores splits.
 */
/*
 * Returns split index for foo.resource.splitN, -1 for foo.resource, -2 otherwise.
 * stem receives the unsplit basename (foo.resource).
 */
static int parse_resource_path(const char *path, char *stem, size_t stem_size)
{
    const char *base;
    const char *p;
    size_t len;

    if (path == NULL || stem == NULL || stem_size == 0)
        return -2;
    base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    p = strstr(base, ".resource.split");
    if (p != NULL && p[15] >= '0' && p[15] <= '9') {
        len = (size_t)(p - base) + 9U;
        if (len >= stem_size)
            return -2;
        memcpy(stem, base, len);
        stem[len] = '\0';
        return atoi(p + 15);
    }
    len = strlen(base);
    if (len >= 9U && strcmp(base + len - 9U, ".resource") == 0) {
        if (len >= stem_size)
            return -2;
        memcpy(stem, base, len + 1U);
        return -1;
    }
    return -2;
}

static size_t collect_parts(struct zip_catalog *cat, const char *inner,
                            const struct zip_entry **parts, size_t max,
                            uint32_t *total_out)
{
    const struct zip_entry *exact;
    char name[ZIP_NAME_MAX];
    char rstem[ZIP_NAME_MAX];
    size_t n = 0;
    uint32_t total = 0;
    unsigned i;
    int ridx;

    *total_out = 0;
    ridx = parse_resource_path(inner, rstem, sizeof(rstem));
    if (ridx >= 0)
        return 0;
    exact = find_entry(cat, inner);
    if (exact != NULL) {
        if (skip_entry(exact) || max == 0)
            return 0;
        parts[0] = exact;
        *total_out = exact->uncomp_size;
        return 1;
    }
    if (strstr(inner, ".split") != NULL)
        return 0;
    for (i = 0; i < (unsigned)max; i++) {
        const struct zip_entry *e;
        int len = snprintf(name, sizeof(name), "%s.split%u", inner, i);

        if (len < 0 || (size_t)len >= sizeof(name))
            break;
        e = find_entry(cat, name);
        if (e == NULL)
            break;
        if (skip_entry(e) || total > ZIP_MAX_CONCAT - e->uncomp_size)
            return 0;
        parts[n++] = e;
        total += e->uncomp_size;
    }
    *total_out = total;
    return n;
}

static int mkdir_parents(const char *file_path)
{
    char buf[512];
    char *p;

    if (file_path == NULL || file_path[0] == '\0')
        return -1;
    snprintf(buf, sizeof(buf), "%s", file_path);
    for (p = buf + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(buf, 0777);
            *p = '/';
        }
    }
    return 0;
}

static size_t resolve_parts(struct zip_catalog *cat, const char *inner,
                            char *resolved, size_t resolved_size,
                            const struct zip_entry **parts, size_t max,
                            uint32_t *total_out)
{
    size_t n;
    const char *base;
    char alt[ZIP_NAME_MAX];
    char rstem[ZIP_NAME_MAX];
    int ridx;

    ridx = parse_resource_path(inner, rstem, sizeof(rstem));
    if (ridx >= 0) {
        *total_out = 0;
        return 0;
    }
    n = collect_parts(cat, inner, parts, max, total_out);
    if (n > 0) {
        snprintf(resolved, resolved_size, "%s", inner);
        return n;
    }
    base = strrchr(inner, '/');
    base = base != NULL ? base + 1 : inner;
    if (strncmp(inner, "assets/bin/Data/", 16) == 0)
        return 0;
    snprintf(alt, sizeof(alt), "assets/bin/Data/%s", base);
    n = collect_parts(cat, alt, parts, max, total_out);
    if (n > 0) {
        snprintf(resolved, resolved_size, "%s", alt);
        printf("G8-ZIP alias %s -> %s parts=%zu size=%u\n", inner, alt, n,
               *total_out);
    }
    return n;
}

static int extract_entry(FILE *zip, const struct zip_entry *entry, FILE *out)
{
    uint32_t data_off;
    unsigned char *comp = NULL;
    unsigned char *raw = NULL;
    int rc = -1;

    if (local_data_offset(zip, entry, &data_off) != 0 ||
        fseek(zip, (long)data_off, SEEK_SET) != 0)
        return -1;
    comp = malloc(entry->comp_size ? entry->comp_size : 1U);
    raw = malloc(entry->uncomp_size ? entry->uncomp_size : 1U);
    if (comp == NULL || raw == NULL ||
        (entry->comp_size > 0 &&
         fread(comp, 1, entry->comp_size, zip) != entry->comp_size))
        goto done;
    if (entry->method == 0) {
        if (entry->comp_size != entry->uncomp_size)
            goto done;
        memcpy(raw, comp, entry->uncomp_size);
    } else {
        unsigned long destlen = entry->uncomp_size;
        unsigned long srclen = entry->comp_size;

        if (puff(raw, &destlen, comp, &srclen) != 0 ||
            destlen != entry->uncomp_size)
            goto done;
    }
    if (entry->uncomp_size > 0 &&
        fwrite(raw, 1, entry->uncomp_size, out) != entry->uncomp_size)
        goto done;
    rc = 0;
done:
    free(comp);
    free(raw);
    return rc;
}

int zip_obb_stat(const char *path, struct stat *out)
{
    char archive[512];
    const char *inner;
    struct zip_catalog *cat;
    const struct zip_entry *parts[ZIP_MAX_SPLITS];
    char resolved[ZIP_NAME_MAX];
    uint32_t total = 0;
    size_t n;
    int result = -1;

    if (out == NULL) {
        errno = EFAULT;
        return -1;
    }
    pthread_mutex_lock(&zip_lock);
    if (split_archive_path(path, archive, sizeof(archive), &inner) != 0) {
        errno = ENOENT;
        pthread_mutex_unlock(&zip_lock);
        return -1;
    }
    cat = catalog_for_archive(archive);
    n = resolve_parts(cat, inner, resolved, sizeof(resolved), parts,
                      ZIP_MAX_SPLITS, &total);
    if (n > 0 && total > 0) {
        memset(out, 0, sizeof(*out));
        out->st_mode = S_IFREG | 0444;
        out->st_nlink = 1;
        out->st_size = (off_t)total;
        out->st_blksize = 4096;
        out->st_blocks = (blkcnt_t)((total + 511U) / 512U);
        result = 0;
    } else {
        errno = ENOENT;
    }
    pthread_mutex_unlock(&zip_lock);
    return result;
}

int zip_obb_access(const char *path)
{
    struct stat st;
    return zip_obb_stat(path, &st);
}

FILE *zip_obb_fopen(const char *path)
{
    char archive[512];
    const char *inner;
    struct zip_catalog *cat;
    const struct zip_entry *parts[ZIP_MAX_SPLITS];
    uint32_t total = 0;
    size_t n;
    size_t i;
    FILE *zip;
    FILE *out = NULL;
    char zip_path[512];
    char resolved[ZIP_NAME_MAX];
    char cache[512];
    const char *root;
    struct stat cached;
    int used_cache = 0;

    pthread_mutex_lock(&zip_lock);
    if (split_archive_path(path, archive, sizeof(archive), &inner) != 0) {
        pthread_mutex_unlock(&zip_lock);
        errno = ENOENT;
        return NULL;
    }
    cat = catalog_for_archive(archive);
    n = resolve_parts(cat, inner, resolved, sizeof(resolved), parts,
                      ZIP_MAX_SPLITS, &total);
    if (n == 0 || cat == NULL || total == 0) {
        pthread_mutex_unlock(&zip_lock);
        errno = ENOENT;
        return NULL;
    }
    root = getenv("SG_ROOT");
    cache[0] = '\0';
    if (root != NULL && root[0] != '\0')
        snprintf(cache, sizeof(cache), "%s/%s", root, resolved);
    if (cache[0] != '\0' && stat(cache, &cached) == 0 &&
        cached.st_size == (off_t)total) {
        pthread_mutex_unlock(&zip_lock);
        out = fopen(cache, "rb");
        if (out != NULL)
            return out;
        pthread_mutex_lock(&zip_lock);
    }
    snprintf(zip_path, sizeof(zip_path), "%s", cat->path);
    zip = fopen(zip_path, "rb");
    if (zip == NULL) {
        pthread_mutex_unlock(&zip_lock);
        return NULL;
    }
    out = NULL;
    if (cache[0] != '\0') {
        (void)mkdir_parents(cache);
        out = fopen(cache, "wb+");
        if (out != NULL)
            used_cache = 1;
    }
    if (out == NULL)
        out = tmpfile();
    if (out == NULL) {
        fclose(zip);
        pthread_mutex_unlock(&zip_lock);
        errno = EIO;
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (extract_entry(zip, parts[i], out) != 0) {
            fclose(zip);
            fclose(out);
            if (used_cache)
                (void)unlink(cache);
            pthread_mutex_unlock(&zip_lock);
            errno = EIO;
            return NULL;
        }
    }
    fclose(zip);
    if (fflush(out) != 0 || fseek(out, 0, SEEK_SET) != 0) {
        fclose(out);
        if (used_cache)
            (void)unlink(cache);
        pthread_mutex_unlock(&zip_lock);
        errno = EIO;
        return NULL;
    }
    if (n > 1U) {
        static int concat_logs;
        if (concat_logs < 16) {
            printf("G8-ZIP concat %s parts=%zu size=%u cache=%s\n", resolved, n,
                   total, used_cache ? cache : "tmp");
            concat_logs += 1;
        }
    } else if (used_cache) {
        static int cache_logs;
        if (cache_logs < 8) {
            printf("G8-ZIP cache %s size=%u\n", cache, total);
            cache_logs += 1;
        }
    }
    pthread_mutex_unlock(&zip_lock);
    if (used_cache) {
        fclose(out);
        out = fopen(cache, "rb");
    }
    return out;
}

static pthread_mutex_t resource_lock = PTHREAD_MUTEX_INITIALIZER;
static char resource_done[48][128];
static int resource_ndone;

static int resource_is_done(const char *stem)
{
    int i;

    for (i = 0; i < resource_ndone; i++) {
        if (strcmp(resource_done[i], stem) == 0)
            return 1;
    }
    return 0;
}

static void resource_mark_done(const char *stem)
{
    if (resource_is_done(stem) || resource_ndone >= 48)
        return;
    snprintf(resource_done[resource_ndone], sizeof(resource_done[0]), "%s",
             stem);
    resource_ndone += 1;
}

static int resource_zip_stat(const char *stem, struct stat *out)
{
    char zippath[768];
    const char *obb;

    obb = getenv("SG_OBB");
    if (obb == NULL || obb[0] == '\0')
        return -1;
    snprintf(zippath, sizeof(zippath), "%s/assets/bin/Data/%s", obb, stem);
    return zip_obb_stat(zippath, out);
}

static void ensure_resource_unsplit(const char *stem)
{
    char zippath[768];
    char disk[512];
    char part[512];
    const char *obb;
    const char *root;
    struct stat want;
    struct stat have;
    FILE *stream;
    int i;
    int already;

    pthread_mutex_lock(&resource_lock);
    already = resource_is_done(stem);
    pthread_mutex_unlock(&resource_lock);
    if (already)
        return;
    obb = getenv("SG_OBB");
    root = getenv("SG_ROOT");
    if (obb == NULL || obb[0] == '\0' || root == NULL || root[0] == '\0')
        return;
    snprintf(zippath, sizeof(zippath), "%s/assets/bin/Data/%s", obb, stem);
    if (zip_obb_stat(zippath, &want) != 0)
        return;
    snprintf(disk, sizeof(disk), "%s/assets/bin/Data/%s", root, stem);
    if (stat(disk, &have) != 0 || have.st_size != want.st_size) {
        stream = zip_obb_fopen(zippath);
        if (stream != NULL)
            fclose(stream);
        if (stat(disk, &have) != 0 || have.st_size != want.st_size)
            return;
    }
    for (i = 0; i < 64; i++) {
        snprintf(part, sizeof(part), "%s/assets/bin/Data/%s.split%d", root, stem,
                 i);
        (void)unlink(part);
    }
    pthread_mutex_lock(&resource_lock);
    resource_mark_done(stem);
    pthread_mutex_unlock(&resource_lock);
    printf("G8-ZIP resource-unsplit %s size=%ld\n", stem, (long)want.st_size);
}

int zip_obb_resource_stat(const char *path, struct stat *out)
{
    char stem[256];
    int idx;

    idx = parse_resource_path(path, stem, sizeof(stem));
    if (idx < -1)
        return -1;
    if (resource_zip_stat(stem, out) != 0)
        return -1;
    if (idx >= 0) {
        ensure_resource_unsplit(stem);
        errno = ENOENT;
        return 1;
    }
    return 0;
}

int zip_obb_resource_fopen(const char *path, FILE **out)
{
    char stem[256];
    char disk[512];
    char zippath[768];
    const char *root;
    const char *obb;
    struct stat want;
    int idx;

    if (out == NULL)
        return -1;
    *out = NULL;
    idx = parse_resource_path(path, stem, sizeof(stem));
    if (idx < -1)
        return -1;
    if (resource_zip_stat(stem, &want) != 0)
        return -1;
    if (idx >= 0) {
        ensure_resource_unsplit(stem);
        errno = ENOENT;
        return 1;
    }
    ensure_resource_unsplit(stem);
    root = getenv("SG_ROOT");
    obb = getenv("SG_OBB");
    if (root != NULL && root[0] != '\0') {
        snprintf(disk, sizeof(disk), "%s/assets/bin/Data/%s", root, stem);
        *out = fopen(disk, "rb");
        if (*out != NULL)
            return 0;
    }
    if (obb == NULL || obb[0] == '\0') {
        errno = ENOENT;
        return 1;
    }
    snprintf(zippath, sizeof(zippath), "%s/assets/bin/Data/%s", obb, stem);
    *out = zip_obb_fopen(zippath);
    return *out != NULL ? 0 : 1;
}
