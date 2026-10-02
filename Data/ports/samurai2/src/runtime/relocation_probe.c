#include "relocation_probe.h"

#include "compat_bridge.h"
#include "softfp_symbols.h"
#include "symbol_probe.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RELOCATION_HOST_CAPACITY = 16 };

struct relocation_context {
    struct elf32_image *images;
    size_t image_count;
    size_t current_image;
    void *host_handles[RELOCATION_HOST_CAPACITY];
    size_t host_count;
    struct nfsmw_relocation_probe_stats *stats;
};

static void *nfsmw_dso_handle;
static int nfsmw_page_size = 4096;
static int nfsmw_h_errno_slot;

static int *nfsmw_get_h_errno(void)
{
    return &nfsmw_h_errno_slot;
}
static void *retained_host_handles[RELOCATION_HOST_CAPACITY];
static size_t retained_host_count;

static void nfsmw_android_assert2(const char *file, int line,
                                  const char *function, const char *message)
{
    (void)fprintf(stderr, "Android assertion at %s:%d (%s): %s\n",
                  file != NULL ? file : "unknown", line,
                  function != NULL ? function : "unknown",
                  message != NULL ? message : "no message");
    abort();
}

static uintptr_t pointer_value(void *pointer)
{
    uintptr_t result = 0U;

    (void)memcpy(&result, &pointer, sizeof(result));
    return result;
}

static uintptr_t alias_lookup(const char *name)
{
    if (strcmp(name, "__assert2") == 0) {
        typedef void (*assert_function)(const char *, int, const char *,
                                        const char *);
        assert_function function = nfsmw_android_assert2;
        uintptr_t result = 0U;

        if (sizeof(function) == sizeof(result)) {
            (void)memcpy(&result, &function, sizeof(result));
        }
        return result;
    }
    if (strcmp(name, "__dso_handle") == 0) {
        return (uintptr_t)&nfsmw_dso_handle;
    }
    if (strcmp(name, "__page_size") == 0)
        return (uintptr_t)&nfsmw_page_size;
    if (strcmp(name, "__get_h_errno") == 0)
        return (uintptr_t)nfsmw_get_h_errno;
    return 0U;
}

static void open_host_libraries(struct relocation_context *context)
{
    static const char *const candidates[] = {
        "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0",
        "libz.so.1", "libstdc++.so.6", "libgcc_s.so.1",
        "libEGL.so.1", "libEGL.so", "libGLESv2.so.2", "libGLESv2.so"
    };
    size_t index;

    for (index = 0U;
         index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        void *handle;

        if (context->host_count == RELOCATION_HOST_CAPACITY) {
            break;
        }
        handle = dlopen(candidates[index],
                        strstr(candidates[index], "EGL") != NULL ||
                        strstr(candidates[index], "GLES") != NULL ?
                            RTLD_NOW | RTLD_GLOBAL : RTLD_LAZY | RTLD_LOCAL);
        if (handle != NULL) {
            context->host_handles[context->host_count] = handle;
            context->host_count += 1U;
        }
    }
}

static void close_host_libraries(struct relocation_context *context)
{
    while (context->host_count != 0U) {
        context->host_count -= 1U;
        (void)dlclose(context->host_handles[context->host_count]);
    }
}

void nfsmw_relocation_release_hosts(void)
{
    while (retained_host_count != 0U) {
        retained_host_count -= 1U;
        (void)dlclose(retained_host_handles[retained_host_count]);
        retained_host_handles[retained_host_count] = NULL;
    }
}

static void retain_host_libraries(struct relocation_context *context)
{
    size_t index;

    nfsmw_relocation_release_hosts();
    for (index = 0U; index < context->host_count; ++index) {
        retained_host_handles[index] = context->host_handles[index];
        context->host_handles[index] = NULL;
    }
    retained_host_count = context->host_count;
    context->host_count = 0U;
}

static uintptr_t host_lookup(const struct relocation_context *context,
                             const char *name)
{
    void *address = dlsym(RTLD_DEFAULT, name);
    size_t index;

    if (address != NULL) {
        return pointer_value(address);
    }
    for (index = 0U; index < context->host_count; ++index) {
        address = dlsym(context->host_handles[index], name);
        if (address != NULL) {
            return pointer_value(address);
        }
    }
    return 0U;
}

static uintptr_t real_fmod_set_output;
static uintptr_t real_fmod_es_init;
static uintptr_t real_fmod_es_load;
static uintptr_t real_fmod_es_get_event;
static uintptr_t real_fmod_create_sound;
static uintptr_t real_fmod_create_sound_internal;
static uintptr_t real_fmod_create_stream;
static uintptr_t real_fmod_event_start;
static uintptr_t real_fmod_set_fs;

enum {
    FMOD_ERR_FILE_COULDNOTSEEK = 20,
    FMOD_ERR_FILE_EOF = 22,
    FMOD_ERR_FILE_NOTFOUND = 23,
    FMOD_EVENT_INFOONLY = 0x00000004U
};

struct fmod_host_file {
    unsigned char *data;
    unsigned int size;
    unsigned int pos;
    unsigned int reads;
    unsigned int seeks;
    unsigned int bytes;
    int cached;
};

enum { FMOD_BANK_CACHE_CAP = 96 };

struct fmod_bank {
    char path[256];
    unsigned char *data;
    unsigned int size;
};

static struct fmod_bank fmod_banks[FMOD_BANK_CACHE_CAP];
static size_t fmod_bank_count;

static struct fmod_bank *fmod_bank_lookup(const char *path)
{
    size_t index;

    for (index = 0U; index < fmod_bank_count; ++index) {
        if (strcmp(fmod_banks[index].path, path) == 0)
            return &fmod_banks[index];
    }
    return NULL;
}

static struct fmod_bank *fmod_bank_remember(const char *path,
                                            unsigned char *data,
                                            unsigned int size)
{
    struct fmod_bank *bank;

    if (fmod_bank_count >= FMOD_BANK_CACHE_CAP || path == NULL ||
        data == NULL)
        return NULL;
    bank = &fmod_banks[fmod_bank_count];
    (void)snprintf(bank->path, sizeof(bank->path), "%s", path);
    bank->data = data;
    bank->size = size;
    fmod_bank_count += 1U;
    return bank;
}

static char fmod_last_dir[512];

static int fmod_try_fopen(const char *path, FILE **stream, unsigned int *size)
{
    FILE *file;
    long end;

    file = fopen(path, "rb");
    if (file == NULL) return -1;
    if (fseek(file, 0L, SEEK_END) != 0 || (end = ftell(file)) < 0L) {
        (void)fclose(file);
        return -1;
    }
    rewind(file);
    *stream = file;
    *size = (unsigned int)end;
    return 0;
}

static void fmod_remember_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t length;

    if (slash == NULL) {
        fmod_last_dir[0] = '\0';
        return;
    }
    length = (size_t)(slash - path);
    if (length >= sizeof(fmod_last_dir))
        length = sizeof(fmod_last_dir) - 1U;
    (void)memcpy(fmod_last_dir, path, length);
    fmod_last_dir[length] = '\0';
}

static int fmod_file_open(const char *name, int unicode,
                          unsigned int *filesize, void **handle,
                          void *userdata)
{
    const char *n = name != NULL ? name : "";
    char trial[512];
    char opened[512];
    const char *base;
    FILE *stream = NULL;
    unsigned int size = 0U;
    struct fmod_host_file *file;
    struct fmod_bank *bank;

    (void)unicode;
    (void)userdata;
    while (*n == '/')
        n++;
    opened[0] = '\0';
    if (fmod_try_fopen(n, &stream, &size) == 0) {
        (void)snprintf(opened, sizeof(opened), "%s", n);
    } else if (strncmp(n, "published/", 10) != 0) {
        (void)snprintf(trial, sizeof(trial), "published/%s", n);
        if (fmod_try_fopen(trial, &stream, &size) == 0)
            (void)snprintf(opened, sizeof(opened), "%s", trial);
    }
    if (stream == NULL && fmod_last_dir[0] != '\0') {
        base = strrchr(n, '/');
        base = base != NULL ? base + 1 : n;
        (void)snprintf(trial, sizeof(trial), "%s/%s", fmod_last_dir, base);
        if (fmod_try_fopen(trial, &stream, &size) == 0)
            (void)snprintf(opened, sizeof(opened), "%s", trial);
    }
    if (stream == NULL) {
        (void)printf("G8-FMOD file-open FAIL requested=%s unicode=%d\n", n,
                     unicode);
        return FMOD_ERR_FILE_NOTFOUND;
    }
    file = calloc(1U, sizeof(*file));
    if (file == NULL) {
        (void)fclose(stream);
        return 31;
    }
    bank = fmod_bank_lookup(opened);
    if (bank != NULL) {
        (void)fclose(stream);
        file->data = bank->data;
        file->size = bank->size;
        file->cached = 1;
    } else {
        file->data = malloc(size);
        if (file->data == NULL ||
            fread(file->data, 1U, size, stream) != (size_t)size) {
            free(file->data);
            free(file);
            (void)fclose(stream);
            (void)printf("G8-FMOD file-slurp FAIL %s size=%u\n", opened, size);
            return 31;
        }
        (void)fclose(stream);
        file->size = size;
        {
            size_t nlen = strlen(opened);
            int persist = 1;

            if (nlen >= 4U) {
                const char *ext = opened + (nlen - 4U);
                if (ext[0] == '.' &&
                    (ext[1] == 'm' || ext[1] == 'M') &&
                    (ext[2] == 'p' || ext[2] == 'P') &&
                    (ext[3] == '3'))
                    persist = 0;
            }
            if (persist != 0 &&
                fmod_bank_remember(opened, file->data, size) != NULL)
                file->cached = 1;
        }
        (void)printf("G8-FMOD file-open %s size=%u requested=%s unicode=%d\n",
                     opened, size, n, unicode);
    }
    file->pos = 0U;
    if (filesize != NULL)
        *filesize = size;
    if (handle != NULL)
        *handle = file;
    fmod_remember_dir(opened);
    return 0;
}

static int fmod_file_close(void *handle, void *userdata)
{
    struct fmod_host_file *file = handle;

    (void)userdata;
    if (file == NULL) return 0;
    if (file->cached == 0)
        free(file->data);
    free(file);
    return 0;
}

static int fmod_file_read(void *handle, void *buffer, unsigned int sizebytes,
                          unsigned int *bytesread, void *userdata)
{
    struct fmod_host_file *file = handle;
    unsigned int remain;
    unsigned int got;

    (void)userdata;
    if (file == NULL || file->data == NULL) return FMOD_ERR_FILE_NOTFOUND;
    remain = file->pos < file->size ? file->size - file->pos : 0U;
    got = sizebytes < remain ? sizebytes : remain;
    if (buffer != NULL) {
        if (got != 0U)
            (void)memcpy(buffer, file->data + file->pos, got);
        if (sizebytes > got)
            (void)memset((unsigned char *)buffer + got, 0, sizebytes - got);
    }
    file->pos += got;
    file->reads += 1U;
    file->bytes += got;
    if (bytesread != NULL)
        *bytesread = got;
    /*
     * FMOD Ex aborts the current command on FILE_EOF. A short or empty
     * read at the real end of a bank must still return OK.
     */
    return 0;
}

static int fmod_file_seek(void *handle, unsigned int pos, void *userdata)
{
    struct fmod_host_file *file = handle;

    (void)userdata;
    if (file == NULL) return FMOD_ERR_FILE_NOTFOUND;
    if (pos > file->size)
        return FMOD_ERR_FILE_COULDNOTSEEK;
    file->pos = pos;
    file->seeks += 1U;
    return 0;
}

static int wrap_fmod_set_output(void *system, int output)
{
    typedef int (*set_output_fn)(void *, int);
    set_output_fn real;
    int result;
    int type;

    (void)memcpy(&real, &real_fmod_set_output, sizeof(real));
    if (real == NULL) return 25;
    (void)printf("G8-FMOD SetOutput type=%d\n", output);
    result = real(system, output);
    (void)printf("G8-FMOD SetOutput result=%d\n", result);
    if (result == 0) return 0;
    /* FMOD Ex Android 4.44: AUDIOTRACK/OPENSL sit somewhere in 0..24. */
    for (type = 0; type <= 24; ++type) {
        if (type == output) continue;
        result = real(system, type);
        (void)printf("G8-FMOD SetOutput retry type=%d result=%d\n", type, result);
        if (result == 0) return 0;
    }
    return result;
}

static int wrap_fmod_es_init(void *event_system, int max_channels,
                             unsigned int flags, void *extra,
                             unsigned int event_flags)
{
    typedef int (*init_fn)(void *, int, unsigned int, void *, unsigned int);
    init_fn real;
    int result;

    (void)memcpy(&real, &real_fmod_es_init, sizeof(real));
    if (real == NULL) return 25;
    (void)printf("G8-FMOD EventSystem::init maxch=%d flags=0x%x extra=%p "
                 "eflags=0x%x\n",
                 max_channels, flags, extra, event_flags);
    result = real(event_system, max_channels, flags, extra, event_flags);
    (void)printf("G8-FMOD EventSystem::init result=%d\n", result);
    return result;
}

static int wrap_fmod_es_load(void *event_system, const char *name,
                             void *info, void **project)
{
    typedef int (*load_fn)(void *, const char *, void *, void **);
    load_fn real;
    int result;

    (void)memcpy(&real, &real_fmod_es_load, sizeof(real));
    if (real == NULL) return 25;
    (void)printf("G8-FMOD EventSystem::load name=%s info=%p\n",
                 name != NULL ? name : "(null)", info);
    result = real(event_system, name, info, project);
    (void)printf("G8-FMOD EventSystem::load result=%d project=%p\n", result,
                 project != NULL ? *project : NULL);
    return result;
}

static int wrap_fmod_es_get_event(void *event_system, const char *name,
                                  unsigned int flags, void **event)
{
    typedef int (*get_event_fn)(void *, const char *, unsigned int, void **);
    get_event_fn real;
    int result;

    (void)memcpy(&real, &real_fmod_es_get_event, sizeof(real));
    if (real == NULL) return 25;
    result = real(event_system, name, flags, event);
    if (result != 0)
        (void)printf("G8-FMOD getEvent name=%s flags=0x%x result=%d event=%p\n",
                     name != NULL ? name : "(null)", flags, result,
                     event != NULL ? *event : NULL);
    if (result == 33 && (flags & FMOD_EVENT_INFOONLY) == 0U) {
        int info;
        void *info_event = NULL;

        info = real(event_system, name, flags | FMOD_EVENT_INFOONLY,
                    &info_event);
        (void)printf("G8-FMOD getEvent INFOONLY name=%s result=%d event=%p\n",
                     name != NULL ? name : "(null)", info, info_event);
    }
    return result;
}

static unsigned int fmod_software_mode(unsigned int mode)
{
    unsigned int use = mode;

    if ((use & 0x00000020U) != 0U)
        use = (use & ~0x00000020U) | 0x00000040U;
    if ((use & 0x00010000U) != 0U)
        use &= ~0x00010000U;
    return use;
}

static int wrap_fmod_create_sound(void *system, const char *name,
                                  unsigned int mode, void *exinfo,
                                  void **sound)
{
    typedef int (*create_sound_fn)(void *, const char *, unsigned int, void *,
                                   void **);
    create_sound_fn real;
    unsigned int use = fmod_software_mode(mode);
    int result;

    (void)memcpy(&real, &real_fmod_create_sound, sizeof(real));
    if (real == NULL) return 25;
    result = real(system, name, use, exinfo, sound);
    if (result != 0)
        (void)printf("G8-FMOD createSound name=%s mode=0x%x use=0x%x "
                     "result=%d\n",
                     name != NULL ? name : "(null)", mode, use, result);
    return result;
}

static int wrap_fmod_create_sound_internal(void *system, const char *name,
                                           unsigned int mode,
                                           unsigned int extra_a,
                                           unsigned int extra_b, void *exinfo,
                                           void **file, int reuse,
                                           void **sound)
{
    typedef int (*csi_fn)(void *, const char *, unsigned int, unsigned int,
                          unsigned int, void *, void **, int, void **);
    csi_fn real;
    unsigned int use = fmod_software_mode(mode);
    int result;

    (void)memcpy(&real, &real_fmod_create_sound_internal, sizeof(real));
    if (real == NULL) return 25;
    result = real(system, name, use, extra_a, extra_b, exinfo, file, reuse,
                  sound);
    if (result != 0)
        (void)printf("G8-FMOD createSoundInternal name=%s mode=0x%x use=0x%x "
                     "result=%d\n",
                     name != NULL ? name : "(null)", mode, use, result);
    return result;
}

static int wrap_fmod_create_stream(void *system, const char *name,
                                   unsigned int mode, void *exinfo,
                                   void **sound)
{
    typedef int (*create_stream_fn)(void *, const char *, unsigned int, void *,
                                    void **);
    create_stream_fn real;
    unsigned int use = fmod_software_mode(mode);
    int result;

    (void)memcpy(&real, &real_fmod_create_stream, sizeof(real));
    if (real == NULL) return 25;
    result = real(system, name, use, exinfo, sound);
    if (result != 0)
        (void)printf("G8-FMOD createStream name=%s mode=0x%x use=0x%x "
                     "result=%d\n",
                     name != NULL ? name : "(null)", mode, use, result);
    return result;
}

static int wrap_fmod_event_start(void *event)
{
    typedef int (*start_fn)(void *);
    start_fn real;
    int result;

    (void)memcpy(&real, &real_fmod_event_start, sizeof(real));
    if (real == NULL) return 25;
    result = real(event);
    if (result != 0)
        (void)printf("G8-FMOD Event::start result=%d\n", result);
    return result;
}

static int wrap_fmod_set_fs(void *system, void *open, void *close, void *read,
                            void *seek, void *asyncread, void *asynccancel,
                            int blockalign)
{
    typedef int (*set_fs_fn)(void *, void *, void *, void *, void *, void *,
                             void *, int);
    set_fs_fn real;

    (void)open;
    (void)close;
    (void)read;
    (void)seek;
    (void)asyncread;
    (void)asynccancel;
    (void)memcpy(&real, &real_fmod_set_fs, sizeof(real));
    if (real == NULL) return 25;
    (void)printf("G8-FMOD setFileSystem blockalign=%d using host files unbuffered\n",
                 blockalign);
    (void)blockalign;
    return real(system, (void *)(uintptr_t)fmod_file_open,
                (void *)(uintptr_t)fmod_file_close,
                (void *)(uintptr_t)fmod_file_read,
                (void *)(uintptr_t)fmod_file_seek, NULL, NULL, 0);
}

static uintptr_t relocation_lookup(const char *name, unsigned int binding,
                                   void *opaque)
{
    struct relocation_context *context = opaque;
    uintptr_t address;
    size_t index;

    (void)binding;
    for (index = 0U; index < context->image_count; ++index) {
        if (index == context->current_image)
            continue;
        address = elf32_find_export(&context->images[index], name);
        if (address != 0U) {
            if (strcmp(name, "FMOD_System_SetOutput") == 0 ||
                strcmp(name, "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE") ==
                    0) {
                void *wrapper = (void *)(uintptr_t)wrap_fmod_set_output;

                real_fmod_set_output = address;
                context->stats->guest_resolutions += 1U;
                (void)printf("G8-FMOD wrapping %s\n", name);
                return pointer_value(wrapper);
            }
            if (strcmp(name, "_ZN4FMOD11EventSystem4initEijPvj") == 0) {
                void *wrapper = (void *)(uintptr_t)wrap_fmod_es_init;

                real_fmod_es_init = address;
                context->stats->guest_resolutions += 1U;
                (void)printf("G8-FMOD wrapping %s\n", name);
                return pointer_value(wrapper);
            }
            if (strcmp(name,
                       "_ZN4FMOD11EventSystem4loadEPKcP19FMOD_EVENT_LOADINFOPPNS_"
                       "12EventProjectE") == 0) {
                void *wrapper = (void *)(uintptr_t)wrap_fmod_es_load;

                real_fmod_es_load = address;
                context->stats->guest_resolutions += 1U;
                (void)printf("G8-FMOD wrapping %s\n", name);
                return pointer_value(wrapper);
            }
            if (strcmp(name, "_ZN4FMOD11EventSystem8getEventEPKcjPPNS_5EventE") ==
                0) {
                void *wrapper = (void *)(uintptr_t)wrap_fmod_es_get_event;

                real_fmod_es_get_event = address;
                context->stats->guest_resolutions += 1U;
                (void)printf("G8-FMOD wrapping %s\n", name);
                return pointer_value(wrapper);
            }
            if (strcmp(name,
                       "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINF"
                       "OPPNS_5SoundE") == 0) {
                void *wrapper = (void *)(uintptr_t)wrap_fmod_create_sound;

                real_fmod_create_sound = address;
                context->stats->guest_resolutions += 1U;
                (void)printf("G8-FMOD wrapping %s\n", name);
                return pointer_value(wrapper);
            }
            if (strcmp(name,
                       "_ZN4FMOD7SystemI19createSoundInternalEPKcjjjP22FMOD_"
                       "CREATESOUNDEXINFOPPNS_4FileEbPPNS_6SoundIE") == 0) {
                void *wrapper =
                    (void *)(uintptr_t)wrap_fmod_create_sound_internal;

                real_fmod_create_sound_internal = address;
                context->stats->guest_resolutions += 1U;
                (void)printf("G8-FMOD wrapping %s\n", name);
                return pointer_value(wrapper);
            }
            if (strcmp(name,
                       "_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXI"
                       "NFOPPNS_5SoundE") == 0) {
                void *wrapper = (void *)(uintptr_t)wrap_fmod_create_stream;

                real_fmod_create_stream = address;
                context->stats->guest_resolutions += 1U;
                (void)printf("G8-FMOD wrapping %s\n", name);
                return pointer_value(wrapper);
            }
            if (strcmp(name, "_ZN4FMOD5Event5startEv") == 0) {
                void *wrapper = (void *)(uintptr_t)wrap_fmod_event_start;

                real_fmod_event_start = address;
                context->stats->guest_resolutions += 1U;
                (void)printf("G8-FMOD wrapping %s\n", name);
                return pointer_value(wrapper);
            }
            if (strncmp(name, "_ZN4FMOD6System13setFileSystemE", 31) == 0) {
                void *wrapper = (void *)(uintptr_t)wrap_fmod_set_fs;

                real_fmod_set_fs = address;
                context->stats->guest_resolutions += 1U;
                (void)printf("G8-FMOD wrapping %s\n", name);
                return pointer_value(wrapper);
            }
            context->stats->guest_resolutions += 1U;
            return address;
        }
    }
    if (nfsmw_symbol_requires_softfp(name)) {
        address = nfsmw_softfp_resolve(name);
        if (address != 0U) {
            context->stats->softfp_resolutions += 1U;
            return address;
        }
    }
    address = alias_lookup(name);
    if (address != 0U) {
        context->stats->alias_resolutions += 1U;
        return address;
    }
    address = nfsmw_compat_resolve(name);
    if (address != 0U) {
        context->stats->alias_resolutions += 1U;
        return address;
    }
    if (nfsmw_symbol_requires_bionic_bridge(name)) {
        context->stats->blocked_resolutions += 1U;
        return 0U;
    }
    address = host_lookup(context, name);
    if (address != 0U) {
        context->stats->host_resolutions += 1U;
        return address;
    }
    context->stats->missing_resolutions += 1U;
    return 0U;
}

int nfsmw_relocation_probe(struct elf32_image *images, size_t image_count,
                           struct nfsmw_relocation_probe_stats *stats,
                           char *error, size_t error_size)
{
    struct relocation_context context;
    size_t index;

    if (images == NULL || image_count == 0U || stats == NULL) {
        (void)snprintf(error, error_size,
                       "invalid relocation-probe arguments");
        return -1;
    }
    (void)memset(stats, 0, sizeof(*stats));
    (void)memset(&context, 0, sizeof(context));
    context.images = images;
    context.image_count = image_count;
    context.stats = stats;
    open_host_libraries(&context);

    for (index = 0U; index < image_count; ++index) {
        struct elf32_relocation_stats module_stats;
        const char *first_unresolved = NULL;
        size_t module_total;

        context.current_image = index;
        if (elf32_relocate(&images[index], relocation_lookup, &context,
                           &module_stats, &first_unresolved,
                           error, error_size) != 0) {
            close_host_libraries(&context);
            return -1;
        }
        module_total = module_stats.relative + module_stats.absolute +
                       module_stats.global_data + module_stats.jump_slots;
        stats->total_relocations += module_total;
        stats->unresolved_relocations += module_stats.unresolved;
        (void)printf("G3-REL %s total=%zu relative=%zu abs32=%zu glob=%zu "
                     "jump=%zu unresolved=%zu first=%s\n",
                     images[index].soname, module_total,
                     module_stats.relative, module_stats.absolute,
                     module_stats.global_data, module_stats.jump_slots,
                     module_stats.unresolved,
                     first_unresolved != NULL ? first_unresolved : "none");
    }
    retain_host_libraries(&context);
    (void)printf("G3-REL providers guest=%zu softfp=%zu host=%zu alias=%zu "
                 "blocked=%zu missing=%zu\n",
                 stats->guest_resolutions, stats->softfp_resolutions,
                 stats->host_resolutions, stats->alias_resolutions,
                 stats->blocked_resolutions, stats->missing_resolutions);
    if (stats->unresolved_relocations == 0U) {
        (void)printf("G3 FULL RELOCATION PASS processed=%zu unresolved=0; "
                     "host providers retained\n",
                     stats->total_relocations);
    } else {
        (void)printf("G3-REL PHASE-A PASS processed=%zu unresolved=%zu; "
                     "host providers retained\n",
                     stats->total_relocations,
                     stats->unresolved_relocations);
    }
    return 0;
}
