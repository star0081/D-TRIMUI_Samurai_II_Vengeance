#include "compat_bridge.h"
#include "android_native.h"
#include "elf32_loader.h"
#include "jni_unity.h"
#include "opensl_bridge.h"
#include "softfp_bridge.h"
#include "softfp_symbols.h"
#include "zip_obb.h"

#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#if UINTPTR_MAX != UINT32_MAX
#error "The NFS MW Bionic compatibility bridge requires a 32-bit host"
#endif

#ifndef SYS_stat64
#define SYS_stat64 195
#endif
#ifndef SYS_lstat64
#define SYS_lstat64 196
#endif
#ifndef SYS_fstat64
#define SYS_fstat64 197
#endif
#ifndef SYS_tkill
#define SYS_tkill 238
#endif

enum { BIONIC_FILE_SIZE = 84, BIONIC_DIRENT_NAME = 256 };

enum { ATEXIT_CAPACITY = 512 };

struct bionic_sigaction {
    void (*handler)(int);
    uint32_t mask;
    int32_t flags;
    void (*restorer)(void);
};

_Static_assert(sizeof(struct bionic_sigaction) == 16U,
               "ARM Bionic sigaction layout must be 16 bytes");

enum { BIONIC_SA_RESTORER = 0x04000000 };

struct compat_atexit_entry {
    void (*destructor)(void *);
    void *argument;
    void *dso;
    bool active;
};

static unsigned char bionic_files[3][BIONIC_FILE_SIZE];
static char ctype_storage[257];
static const char *ctype_pointer = &ctype_storage[1];
static int16_t tolower_storage[257];
static const int16_t *tolower_pointer = &tolower_storage[1];
static int16_t toupper_storage[257];
static const int16_t *toupper_pointer = &toupper_storage[1];
static struct compat_atexit_entry atexit_entries[ATEXIT_CAPACITY];
static size_t atexit_count;
static pthread_mutex_t atexit_lock = PTHREAD_MUTEX_INITIALIZER;
enum { GUEST_IMAGE_CAPACITY = 8 };
static const struct elf32_image *guest_images[GUEST_IMAGE_CAPACITY];
static size_t guest_image_count;
static unsigned char guest_dl_token;

void nfsmw_compat_register_image(const struct elf32_image *image)
{
    if (image == NULL || guest_image_count >= GUEST_IMAGE_CAPACITY)
        return;
    guest_images[guest_image_count] = image;
    guest_image_count += 1U;
}

static int is_preloaded_guest_library(const char *name)
{
    const char *base;

    if (name == NULL || name[0] == '\0')
        return 1;
    if (strstr(name, "android-libs") != NULL)
        return 1;
    base = strrchr(name, '/');
    base = base != NULL ? base + 1 : name;
    if (strncmp(base, "lib", 3) == 0)
        base += 3;
    return strncmp(base, "main", 4) == 0 ||
           strncmp(base, "unity", 5) == 0 ||
           strncmp(base, "mono", 4) == 0 ||
           strncmp(base, "il2cpp", 6) == 0 ||
           strncmp(base, "HellCPU", 7) == 0 ||
           strncmp(base, "android", 7) == 0 ||
           strncmp(base, "log.so", 6) == 0 ||
           strcmp(base, "log") == 0;
}

static int gles_softfp_bound;
static void *ensure_gl_lib(int egl);

static void *host_gl_resolver(const char *name)
{
    void *handle;
    void *p;

    handle = ensure_gl_lib(0);
    p = handle != NULL ? dlsym(handle, name) : NULL;
    if (p == NULL) {
        handle = ensure_gl_lib(1);
        p = handle != NULL ? dlsym(handle, name) : NULL;
    }
    return p;
}

static void bind_gles_softfp_once(void)
{
    if (gles_softfp_bound == 1)
        return;
    if (nfsmw_softfp_bind_gles(host_gl_resolver) == 0) {
        gles_softfp_bound = 1;
        (void)printf("G8-SOFTFP GLES thunks bound\n");
    } else if (gles_softfp_bound == 0) {
        gles_softfp_bound = -1;
        (void)printf("G8-SOFTFP GLES bind FAIL (will retry)\n");
    }
}

static void *compat_eglGetProcAddress(const char *name)
{
    static void *(*real_get)(const char *) = NULL;
    uintptr_t thunk;
    void *p;

    bind_gles_softfp_once();
    thunk = nfsmw_softfp_resolve(name);
    if (thunk != 0U)
        return (void *)thunk;
    if (real_get == NULL) {
        void *handle = ensure_gl_lib(1);
        if (handle == NULL)
            handle = ensure_gl_lib(0);
        real_get = handle != NULL ?
            (void *(*)(const char *))dlsym(handle, "eglGetProcAddress") : NULL;
    }
    p = real_get != NULL ? real_get(name) : NULL;
    return p;
}

static void *gles_handle;
static void *egl_handle;
static int gles_dlsym_logs;

static const char *library_basename(const char *name)
{
    const char *base;

    if (name == NULL)
        return "";
    base = strrchr(name, '/');
    return base != NULL ? base + 1 : name;
}

static int name_is_gles_lib(const char *name)
{
    const char *base = library_basename(name);

    return strstr(base, "GLESv2") != NULL ||
           strstr(base, "GLESv1") != NULL ||
           strstr(base, "libGLES") != NULL;
}

static int name_is_egl_lib(const char *name)
{
    const char *base = library_basename(name);

    return strncmp(base, "libEGL.so", 9) == 0 || strcmp(base, "libEGL") == 0;
}

static int name_is_gl_sym(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;
    if (strncmp(name, "egl", 3) == 0)
        return 1;
    return name[0] == 'g' && name[1] == 'l' &&
           name[2] >= 'A' && name[2] <= 'Z';
}

static void *ensure_gl_lib(int egl)
{
    void **slot = egl ? &egl_handle : &gles_handle;
    const char *primary = egl ? "libEGL.so" : "libGLESv2.so";
    const char *alt = egl ? "libGLESv2.so" : "libEGL.so";

    if (*slot != NULL)
        return *slot;
    (void)dlerror();
    *slot = dlopen(primary, RTLD_NOW | RTLD_GLOBAL);
    if (*slot == NULL)
        *slot = dlopen(alt, RTLD_NOW | RTLD_GLOBAL);
    (void)printf("G8-DLOPEN ensure %s -> %p%s%s\n", primary, *slot,
                 *slot == NULL ? " " : "",
                 *slot == NULL ? dlerror() : "");
    return *slot;
}

static int gles_missing_stub(void)
{
    return 0;
}

static void *resolve_gl_sym(void *handle, const char *name)
{
    void *p = NULL;
    void *egl_get;
    void *(*get_proc)(const char *);
    uintptr_t thunk;

    if (name != NULL && strcmp(name, "eglGetProcAddress") == 0)
        return (void *)compat_eglGetProcAddress;
    bind_gles_softfp_once();
    thunk = nfsmw_softfp_resolve(name);
    if (thunk != 0U) {
        if (gles_dlsym_logs < 48)
            (void)printf("G8-DLSYM SOFTFP %s -> %p\n", name, (void *)thunk);
        gles_dlsym_logs += 1;
        return (void *)thunk;
    }
    if (handle != NULL && handle != RTLD_DEFAULT && handle != RTLD_NEXT)
        p = dlsym(handle, name);
    if (p == NULL && gles_handle != NULL && handle != gles_handle)
        p = dlsym(gles_handle, name);
    if (p == NULL && egl_handle != NULL && handle != egl_handle)
        p = dlsym(egl_handle, name);
    if (p == NULL)
        p = dlsym(ensure_gl_lib(0), name);
    if (p == NULL)
        p = dlsym(ensure_gl_lib(1), name);
    if (p == NULL)
        p = dlsym(RTLD_DEFAULT, name);
    if (p == NULL) {
        egl_get = dlsym(ensure_gl_lib(1), "eglGetProcAddress");
        if (egl_get == NULL)
            egl_get = dlsym(ensure_gl_lib(0), "eglGetProcAddress");
        get_proc = (void *(*)(const char *))egl_get;
        if (get_proc != NULL)
            p = get_proc(name);
    }
    if (p == NULL) {
        (void)printf("G8-DLSYM STUB %s h=%p\n", name, handle);
        p = (void *)&gles_missing_stub;
    }
    if (p == NULL || gles_dlsym_logs < 48 ||
        strcmp(name, "glGetString") == 0) {
        (void)printf("G8-DLSYM %s h=%p -> %p\n", name, handle, p);
        gles_dlsym_logs += 1;
    }
    return p;
}

static void *compat_dlopen(const char *name, int flags)
{
    void *android;
    void *handle;

    (void)printf("G8-DLOPEN %s\n", name != NULL ? name : "(null)");
    if (is_preloaded_guest_library(name)) {
        (void)printf("G8-DLOPEN already-mapped %s\n",
                     name != NULL && name[0] != '\0' ? name : "(empty)");
        return &guest_dl_token;
    }
    if (nfsmw_opensl_is_name(name) != 0) {
        (void)printf("G8-OPENSL intercepted dlopen(%s)\n", name);
        return nfsmw_opensl_handle();
    }
    android = sg_android_dlopen(name);
    if (android != NULL)
        return android;
    {
        const char *base = library_basename(name);
        const char *host = NULL;
        if (strcmp(base, "libc.so") == 0)
            host = "libc.so.6";
        else if (strcmp(base, "libm.so") == 0)
            host = "libm.so.6";
        else if (strcmp(base, "libdl.so") == 0)
            host = "libdl.so.2";
        else if (strcmp(base, "libstdc++.so") == 0)
            host = "libstdc++.so.6";
        else if (strcmp(base, "libz.so") == 0)
            host = "libz.so.1";
        else if (strcmp(base, "libgcc.so") == 0 || strcmp(base, "libgcc_s.so") == 0)
            host = "libgcc_s.so.1";
        if (host != NULL)
            name = host;
    }
    if (name_is_gles_lib(name) || name_is_egl_lib(name))
        return ensure_gl_lib(name_is_egl_lib(name));
    flags &= (int)(RTLD_LAZY | RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD |
                   RTLD_NODELETE);
    if ((flags & (RTLD_LAZY | RTLD_NOW)) == (RTLD_LAZY | RTLD_NOW))
        flags = (flags & ~RTLD_LAZY) | RTLD_NOW;
    if ((flags & (RTLD_LAZY | RTLD_NOW)) == 0)
        flags |= RTLD_NOW;
    handle = dlopen(name, flags);
    if (handle == NULL)
        (void)printf("G8-DLOPEN fail %s %s\n",
                     name != NULL ? name : "(null)", dlerror());
    return handle;
}

static void *guest_dlsym(const char *name)
{
    size_t index;
    if (name == NULL)
        return NULL;
    if (strcmp(name, "JNI_OnLoad") == 0 || strcmp(name, "JNI_OnUnload") == 0)
        return NULL;
    for (index = 0U; index < guest_image_count; ++index) {
        const uintptr_t address = elf32_find_export(guest_images[index], name);
        if (address != 0U)
            return (void *)address;
    }
    return NULL;
}

void *nfsmw_guest_symbol(const char *name)
{
    return guest_dlsym(name);
}

static void *compat_dlsym(void *handle, const char *name)
{
    void *opensl = nfsmw_opensl_dlsym(name);
    void *android;
    void *guest;

    if (opensl != NULL)
        return opensl;
    android = sg_android_dlsym(handle, name);
    if (android != NULL)
        return android;
    if (handle == &guest_dl_token) {
        guest = guest_dlsym(name);
        return guest;
    }
    if (nfsmw_opensl_is_handle(handle) != 0)
        return NULL;
    if (sg_android_is_handle(handle) != 0)
        return NULL;
    if (name_is_gl_sym(name))
        return resolve_gl_sym(handle, name);
    return dlsym(handle, name);
}

static int compat_dlclose(void *handle)
{
    if (handle == &guest_dl_token)
        return 0;
    if (nfsmw_opensl_is_handle(handle) != 0)
        return 0;
    if (sg_android_is_handle(handle) != 0)
        return 0;
    if (handle != NULL && (handle == gles_handle || handle == egl_handle))
        return 0;
    return dlclose(handle);
}

struct bionic_timespec32 {
    int32_t seconds;
    int32_t nanoseconds;
};

struct bionic_timeval32 {
    int32_t seconds;
    int32_t microseconds;
};

struct bionic_timezone32 {
    int32_t minutes_west;
    int32_t daylight_saving;
};

static int compat_clock_gettime(clockid_t clock_id,
                                struct bionic_timespec32 *guest)
{
    struct timespec host;
    int result;

    if (guest == NULL) {
        errno = EFAULT;
        return -1;
    }
    result = clock_gettime(clock_id, &host);
    if (result != 0) return result;
    if (host.tv_sec < INT32_MIN || host.tv_sec > INT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    guest->seconds = (int32_t)host.tv_sec;
    guest->nanoseconds = (int32_t)host.tv_nsec;
    return 0;
}

static int compat_gettimeofday(struct bionic_timeval32 *guest,
                               struct bionic_timezone32 *zone)
{
    struct timeval host;
    int result;

    if (guest == NULL) {
        errno = EFAULT;
        return -1;
    }
    result = gettimeofday(&host, NULL);
    if (result != 0) return result;
    if (host.tv_sec < INT32_MIN || host.tv_sec > INT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    guest->seconds = (int32_t)host.tv_sec;
    guest->microseconds = (int32_t)host.tv_usec;
    if (zone != NULL) {
        zone->minutes_west = 0;
        zone->daylight_saving = 0;
    }
    return 0;
}

struct bionic_stat32 {
    uint64_t device;
    unsigned char padding0[4];
    uint32_t legacy_inode;
    uint32_t mode;
    uint32_t links;
    uint32_t user;
    uint32_t group;
    uint64_t special_device;
    unsigned char padding3[4];
    int64_t size;
    uint32_t block_size;
    uint64_t blocks;
    struct bionic_timespec32 access_time;
    struct bionic_timespec32 modification_time;
    struct bionic_timespec32 change_time;
    uint64_t inode;
};

struct bionic_statfs32 {
    uint32_t type;
    uint32_t block_size;
    uint32_t blocks;
    uint32_t blocks_free;
    uint32_t blocks_available;
    uint32_t files;
    uint32_t files_free;
    int32_t fsid[2];
    uint32_t name_length;
    uint32_t fragment_size;
    uint32_t flags;
    uint32_t spare[4];
};

struct bionic_dirent32 {
    uint64_t inode;
    int64_t offset;
    uint16_t record_length;
    uint8_t type;
    char name[BIONIC_DIRENT_NAME];
};

struct bionic_pthread_attr32 {
    uint32_t flags;
    void *stack_base;
    uint32_t stack_size;
    uint32_t guard_size;
    int32_t scheduling_policy;
    int32_t scheduling_priority;
};

_Static_assert(sizeof(struct bionic_stat32) == 104U,
               "unexpected Android ARM stat layout");
_Static_assert(sizeof(struct bionic_dirent32) == 280U,
               "unexpected Android ARM dirent layout");
_Static_assert(sizeof(struct bionic_pthread_attr32) == 24U,
               "unexpected Android ARM pthread_attr layout");

static _Thread_local struct bionic_dirent32 directory_entry;

static FILE *host_stream(void *guest)
{
    const uintptr_t address = (uintptr_t)guest;
    const uintptr_t base = (uintptr_t)&bionic_files[0][0];
    const uintptr_t end = base + sizeof(bionic_files);

    if (address >= base && address < end) {
        const size_t index = (size_t)(address - base) / BIONIC_FILE_SIZE;

        if (index == 0U) {
            return stdin;
        }
        if (index == 1U) {
            return stdout;
        }
        return stderr;
    }
    return (FILE *)guest;
}

static int compat_fclose(void *stream) { return fclose(host_stream(stream)); }
static int compat_fflush(void *stream) { return fflush(host_stream(stream)); }
static size_t compat_fread(void *buffer, size_t size, size_t count,
                           void *stream)
{
    return fread(buffer, size, count, host_stream(stream));
}
static size_t compat_fwrite(const void *buffer, size_t size, size_t count,
                            void *stream)
{
    return fwrite(buffer, size, count, host_stream(stream));
}
static int compat_fseek(void *stream, int32_t offset, int origin)
{
    return fseek(host_stream(stream), (long)offset, origin);
}
static int compat_fseeko(void *stream, int64_t offset, int origin)
{
    if (offset < INT32_MIN || offset > INT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return fseek(host_stream(stream), (long)offset, origin);
}
static int32_t compat_ftell(void *stream)
{
    return (int32_t)ftell(host_stream(stream));
}
static int64_t compat_ftello(void *stream)
{
    return (int64_t)ftell(host_stream(stream));
}
static int compat_fwide(void *stream, int mode)
{
    return fwide(host_stream(stream), mode);
}
static int is_proc_cpuinfo(const char *path)
{
    return path != NULL && strcmp(path, "/proc/cpuinfo") == 0;
}

/*
 * FMOD Ex vendors NDK cpu-features.c. It tokenizes /proc/cpuinfo for
 * "neon" and "vfp". The aarch64 kernel lists "asimd" and "fp" instead, so
 * the mixer plugin never registers (NEEDSHARDWARE). Serve a 32-bit ARM
 * cpuinfo; the A133P Cortex-A53 executes those NEON opcodes in AArch32.
 */
static FILE *open_fake_armv7_cpuinfo(void)
{
    static const char fake[] =
        "Processor\t: ARMv7 Processor rev 4 (v7l)\n"
        "processor\t: 0\n"
        "BogoMIPS\t: 48.00\n"
        "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 "
        "vfpv3d16 tls vfpv4 idiva idivt\n"
        "CPU implementer\t: 0x41\n"
        "CPU architecture: 7\n"
        "CPU variant\t: 0x0\n"
        "CPU part\t: 0xd03\n"
        "CPU revision\t: 4\n"
        "\n"
        "Hardware\t: sun50iw10p1\n";
    const size_t fake_size = sizeof(fake) - 1U;
    FILE *stream = tmpfile();

    if (stream == NULL) {
        stream = fmemopen((void *)(uintptr_t)fake, fake_size, "r");
    } else if (fwrite(fake, 1U, fake_size, stream) != fake_size ||
               fseek(stream, 0L, SEEK_SET) != 0) {
        (void)fclose(stream);
        stream = NULL;
    }
    if (stream != NULL) {
        (void)printf("G8-CPUINFO serving fake ARMv7 neon cpuinfo\n");
    }
    return stream;
}

static int is_fmod_bank_path(const char *path)
{
    return path != NULL &&
           (strstr(path, ".fev") != NULL || strstr(path, ".fsb") != NULL);
}

static int path_ends_with(const char *path, const char *suffix)
{
    size_t length;
    size_t suffix_length;

    if (path == NULL || suffix == NULL)
        return 0;
    length = strlen(path);
    suffix_length = strlen(suffix);
    return length >= suffix_length &&
           strcmp(path + length - suffix_length, suffix) == 0;
}

static void g8_path_log(const char *kind, const char *from, const char *to)
{
    static int logs;
    if (logs >= 32)
        return;
    logs += 1;
    printf("G8-PATH %s %s -> %s\n", kind, from, to);
}

static const char *rewrite_guest_path(const char *path, char *buf, size_t buf_size)
{
    const char *obb;
    const char *needle;
    const char *inner;
    const char *root;

    if (path == NULL)
        return NULL;
    needle = strstr(path, "/Android/Android/obb/");
    if (needle != NULL) {
        snprintf(buf, buf_size, "%.*s/Android/obb/%s",
                 (int)(needle - path), path,
                 needle + strlen("/Android/Android/obb/"));
        g8_path_log("dup", path, buf);
        return buf;
    }

    /* Unity 4 concatenates the APK path with "/assets/...". The files are
       already extracted next to the port. */
    inner = strstr(path, ".apk/");
    if (inner != NULL) {
        inner += 5;
        root = getenv("SG_ROOT");
        if (inner[0] != '\0' && root != NULL && root[0] != '\0') {
            snprintf(buf, buf_size, "%s/%s", root, inner);
            if (access(buf, F_OK) == 0) {
                g8_path_log("apk", path, buf);
                return buf;
            }
            {
                const char *base = strrchr(inner, '/');
                base = base != NULL ? base + 1 : inner;
                snprintf(buf, buf_size, "%s/assets/bin/Data/%s", root, base);
                if (access(buf, F_OK) == 0) {
                    g8_path_log("apk-data", path, buf);
                    return buf;
                }
            }
        }
    }

    /* Unity concatenates dataPath+".obb"+" /assets/..." after nativeFile(obb).
       Mapping those onto the OBB file makes every split probe look like a
       266 MiB hit, so LoadScene never finishes. Only rewrite the OBB itself. */
    inner = strstr(path, ".obb/");
    if (inner != NULL) {
        inner += 5;
        root = getenv("SG_ROOT");
        /* Leftover *.resource.splitN on disk makes Unity Audio use a
           2-part SplitFile. Do not map those; serve the concat instead. */
        if (inner[0] != '\0' && root != NULL && root[0] != '\0' &&
            strstr(inner, ".resource.split") == NULL) {
            snprintf(buf, buf_size, "%s/%s", root, inner);
            if (access(buf, F_OK) == 0) {
                g8_path_log("inner", path, buf);
                return buf;
            }
            {
                const char *base = strrchr(inner, '/');
                base = base != NULL ? base + 1 : inner;
                snprintf(buf, buf_size, "%s/assets/bin/Data/%s", root, base);
                if (access(buf, F_OK) == 0) {
                    g8_path_log("data", path, buf);
                    return buf;
                }
            }
        }
        return path;
    }

    obb = getenv("SG_OBB");
    if (obb != NULL && obb[0] != '\0' && path_ends_with(path, ".obb") &&
        access(path, F_OK) != 0 && access(obb, F_OK) == 0) {
        g8_path_log("obb", path, obb);
        return obb;
    }
    return path;
}

static int path_is_test_tmp(const char *path)
{
    size_t length;
    if (path == NULL)
        return 0;
    length = strlen(path);
    return length >= 8U && strcmp(path + length - 8U, "test.tmp") == 0;
}

/* Madfinger IsObbFileAccessible writes dir/test.tmp then checks it is
   readable and non-empty. C# File.Create leaves a 0-byte file → False. */
static void ensure_test_tmp(const char *path)
{
    FILE *stream;
    long size;

    if (!path_is_test_tmp(path))
        return;
    stream = fopen(path, "rb");
    if (stream != NULL) {
        if (fseek(stream, 0, SEEK_END) == 0)
            size = ftell(stream);
        else
            size = 0;
        fclose(stream);
        if (size > 0)
            return;
    }
    stream = fopen(path, "wb");
    if (stream == NULL)
        return;
    fwrite("RRR", 1U, 3U, stream);
    fclose(stream);
    printf("G8-TMP seeded %s\n", path);
}

static int path_is_zip_inner(const char *path)
{
    return path != NULL &&
           (strstr(path, ".obb/") != NULL || strstr(path, ".apk/") != NULL);
}

static int unity_data_zip_path(const char *path, char *out, size_t out_size)
{
    const char *base;
    const char *obb;
    size_t n;

    if (path == NULL || out == NULL)
        return -1;
    obb = getenv("SG_OBB");
    if (obb == NULL || obb[0] == '\0')
        return -1;
    base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    n = strlen(base);
    if (n == 0U)
        return -1;
    if (strncmp(base, "sharedassets", 12) != 0 &&
        strncmp(base, "level", 5) != 0 &&
        !(n > 9U && strcmp(base + n - 9U, ".resource") == 0) &&
        !(n > 7U && strcmp(base + n - 7U, ".assets") == 0))
        return -1;
    snprintf(out, out_size, "%s/assets/bin/Data/%s", obb, base);
    return 0;
}

static void g8_resource_log(const char *kind, const char *path, int rc)
{
    static int logs;

    if (logs >= 24)
        return;
    logs += 1;
    printf("G8-ZIP resource-%s %s rc=%d\n", kind, path != NULL ? path : "?",
           rc);
}

static void *compat_fopen(const char *path, const char *mode)
{
    FILE *stream;
    FILE *resource;
    char rewritten[4096];
    const char *orig = path;
    int resource_rc;

    if (path != NULL && (mode == NULL || strchr(mode, 'w') == NULL)) {
        resource_rc = zip_obb_resource_fopen(path, &resource);
        if (resource_rc >= 0) {
            g8_resource_log("fopen", path, resource_rc);
            return resource_rc == 0 ? resource : NULL;
        }
    }

    path = rewrite_guest_path(path, rewritten, sizeof(rewritten));
    if (path_is_test_tmp(path)) {
        printf("G8-FOPEN %s mode=%s\n", path, mode != NULL ? mode : "");
        if (mode == NULL || strchr(mode, 'w') == NULL)
            ensure_test_tmp(path);
    }

    if (is_proc_cpuinfo(path)) {
        (void)printf("G8-CPUINFO fopen(%s, %s)\n", path,
                     mode != NULL ? mode : "");
        return open_fake_armv7_cpuinfo();
    }
    stream = fopen(path, mode);
    if (path != NULL) {
        static int fopen_logs;
        if (fopen_logs < 64 ||
            strstr(path, "globalgamemanagers") != NULL ||
            strstr(path, "level") != NULL ||
            strstr(path, "sharedassets") != NULL ||
            strstr(path, ".resource") != NULL ||
            strstr(path, ".apk") != NULL ||
            strstr(path, ".obb") != NULL ||
            strstr(path, "metadata") != NULL) {
            printf("G8-FOPEN %s %s\n", path, stream != NULL ? "ok" : "FAIL");
            fopen_logs += 1;
        }
    }
    if (stream == NULL && path != NULL &&
        strstr(path, "global-metadata.dat") != NULL) {
        char alt[4096];
        const char *root = getenv("SG_ROOT");
        snprintf(alt, sizeof(alt),
                 "%s/assets/bin/Data/Managed/Metadata/global-metadata.dat",
                 root != NULL && root[0] != '\0' ? root : ".");
        stream = fopen(alt, mode != NULL ? mode : "rb");
        printf("G8-META fopen %s -> %s %s\n", path, alt,
               stream != NULL ? "ok" : "FAIL");
    }
    if (stream == NULL && path != NULL) {
        const char *base = strrchr(path, '/');
        base = base != NULL ? base + 1 : path;
        if (strcmp(base, "globalgamemanagers") == 0 ||
            strcmp(base, "globalgamemanagers.assets") == 0 ||
            strncmp(base, "level", 5) == 0 ||
            strncmp(base, "sharedassets", 12) == 0 ||
            strcmp(base, "unity default resources") == 0 ||
            strcmp(base, "splash.png") == 0) {
            char alt[4096];
            const char *root = getenv("SG_ROOT");
            snprintf(alt, sizeof(alt), "%s/assets/bin/Data/%s",
                     root != NULL && root[0] != '\0' ? root : ".", base);
            stream = fopen(alt, mode != NULL ? mode : "rb");
            printf("G8-DATA fopen %s -> %s %s\n", path, alt,
                   stream != NULL ? "ok" : "FAIL");
            if (stream == NULL) {
                const char *obb = getenv("SG_OBB");
                if (obb != NULL && obb[0] != '\0') {
                    char zippath[4096];
                    snprintf(zippath, sizeof(zippath),
                             "%s/assets/bin/Data/%s", obb, base);
                    stream = zip_obb_fopen(zippath);
                    printf("G8-ZIP fopen-via-data %s %s\n", zippath,
                           stream != NULL ? "ok" : "FAIL");
                }
            }
        }
    }
    if (stream == NULL && path_is_zip_inner(orig) &&
        (mode == NULL || strchr(mode, 'w') == NULL)) {
        stream = zip_obb_fopen(orig);
        printf("G8-ZIP fopen %s %s\n", orig,
               stream != NULL ? "ok" : "FAIL");
    }
    if (stream == NULL && (mode == NULL || strchr(mode, 'w') == NULL)) {
        char zippath[4096];
        if (unity_data_zip_path(orig != NULL ? orig : path, zippath,
                                sizeof(zippath)) == 0) {
            stream = zip_obb_fopen(zippath);
            printf("G8-ZIP fopen-via-name %s %s\n", zippath,
                   stream != NULL ? "ok" : "FAIL");
        }
    }
    return stream;
}

static int compat_open(const char *path, int flags, ...)
{
    char rewritten[4096];
    const char *orig = path;
    int opened;

    if (path != NULL && (flags & O_CREAT) == 0 &&
        (flags & O_ACCMODE) == O_RDONLY) {
        FILE *resource;
        int resource_rc = zip_obb_resource_fopen(path, &resource);

        if (resource_rc == 0) {
            opened = dup(fileno(resource));
            fclose(resource);
            g8_resource_log("open", path, 0);
            return opened;
        }
        if (resource_rc == 1) {
            g8_resource_log("open", path, 1);
            return -1;
        }
    }

    path = rewrite_guest_path(path, rewritten, sizeof(rewritten));
    if (is_proc_cpuinfo(path)) {
        FILE *stream;
        int descriptor;

        (void)printf("G8-CPUINFO open(%s)\n", path);
        stream = open_fake_armv7_cpuinfo();
        if (stream == NULL) {
            return -1;
        }
        descriptor = dup(fileno(stream));
        (void)fclose(stream);
        return descriptor;
    }
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        int mode;
        int created;

        va_start(arguments, flags);
        mode = va_arg(arguments, int);
        va_end(arguments);
        created = open(path, flags, mode);
        if (created >= 0 && path_is_test_tmp(path))
            printf("G8-OPEN create %s fd=%d flags=0x%x\n", path, created,
                   flags);
        return created;
    }
    if (path_is_test_tmp(path)) {
        printf("G8-OPEN %s flags=0x%x\n", path, flags);
        if ((flags & O_ACCMODE) == O_RDONLY)
            ensure_test_tmp(path);
    }
    opened = open(path, flags);
    if (opened < 0 && path_is_zip_inner(orig) &&
        (flags & O_ACCMODE) == O_RDONLY) {
        FILE *stream = zip_obb_fopen(orig);
        if (stream != NULL) {
            opened = dup(fileno(stream));
            fclose(stream);
            printf("G8-ZIP open %s fd=%d\n", orig, opened);
        }
    }
    if (opened < 0 && (flags & O_ACCMODE) == O_RDONLY) {
        char zippath[4096];
        FILE *stream;

        if (unity_data_zip_path(orig != NULL ? orig : path, zippath,
                                sizeof(zippath)) == 0) {
            stream = zip_obb_fopen(zippath);
            if (stream != NULL) {
                opened = dup(fileno(stream));
                fclose(stream);
                printf("G8-ZIP open-via-name %s fd=%d\n", zippath, opened);
            }
        }
    }
    if (opened < 0) {
        static int open_fail_logs;
        const char *probe = orig != NULL ? orig : path;
        if (open_fail_logs < 24 && probe != NULL &&
            (strstr(probe, "sharedassets") != NULL ||
             strstr(probe, ".resource") != NULL)) {
            printf("G8-OPEN FAIL %s flags=0x%x\n", probe, flags);
            open_fail_logs += 1;
        }
    }
    return opened;
}

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

static int compat_openat(int dirfd, const char *path, int flags, ...)
{
    int mode = 0;

    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, int);
        va_end(arguments);
    }
    if (path != NULL && (path[0] == '/' || dirfd == AT_FDCWD)) {
        if ((flags & O_CREAT) != 0)
            return compat_open(path, flags, mode);
        return compat_open(path, flags);
    }
    if ((flags & O_CREAT) != 0)
        return openat(dirfd, path, flags, mode);
    return openat(dirfd, path, flags);
}

static void *compat_fopen64(const char *path, const char *mode)
{
    return compat_fopen(path, mode);
}

static int compat_access(const char *path, int mode)
{
    char rewritten[4096];
    const char *orig = path;
    struct stat host;
    int resource_rc;

    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    resource_rc = zip_obb_resource_stat(path, &host);
    if (resource_rc == 0)
        return 0;
    if (resource_rc == 1)
        return -1;

    path = rewrite_guest_path(path, rewritten, sizeof(rewritten));
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (access(path, mode) == 0)
        return 0;
    if (path_is_zip_inner(orig) && zip_obb_access(orig) == 0)
        return 0;
    {
        char zippath[4096];
        if (unity_data_zip_path(orig != NULL ? orig : path, zippath,
                                sizeof(zippath)) == 0 &&
            zip_obb_access(zippath) == 0)
            return 0;
    }
    return -1;
}

static void *compat_fdopen(int descriptor, const char *mode)
{
    return fdopen(descriptor, mode);
}
static int compat_vfprintf(void *stream, const char *format, va_list arguments)
{
    return vfprintf(host_stream(stream), format, arguments);
}
static int compat_fprintf(void *stream, const char *format, ...)
{
    va_list arguments;
    int result;

    va_start(arguments, format);
    result = compat_vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

static int *compat_errno(void) { return &errno; }

static void __attribute__((noreturn)) compat_abort(void)
{
    const uintptr_t caller =
        (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));

    (void)fprintf(stderr, "G-ABORT guest-return=0x%08lx\n",
                  (unsigned long)caller);
    (void)fflush(NULL);
    (void)raise(SIGABRT);
    _exit(134);
}

static void __attribute__((noreturn))
compat_assert2(const char *file, int line, const char *function,
               const char *condition)
{
    (void)fprintf(stderr, "G-ASSERT %s:%d %s: %s\n",
                  file != NULL ? file : "unknown", line,
                  function != NULL ? function : "unknown",
                  condition != NULL ? condition : "unknown");
    compat_abort();
}

static int compat_android_log_vprint(int priority, const char *tag,
                                     const char *format, va_list arguments)
{
    int result;

    (void)fprintf(stderr, "android[%d] %s: ", priority,
                  tag != NULL ? tag : "NFSMW");
    result = vfprintf(stderr, format != NULL ? format : "", arguments);
    (void)fputc('\n', stderr);
    return result;
}

static int compat_android_log_print(int priority, const char *tag,
                                    const char *format, ...)
{
    va_list arguments;
    int result;

    va_start(arguments, format);
    result = compat_android_log_vprint(priority, tag, format, arguments);
    va_end(arguments);
    return result;
}

static int compat_android_log_write(int priority, const char *tag,
                                    const char *text)
{
    return compat_android_log_print(priority, tag, "%s",
                                    text != NULL ? text : "");
}

static void compat_android_log_assert(const char *condition, const char *tag,
                                      const char *format, ...)
{
    va_list arguments;

    (void)fprintf(stderr, "Android assertion (%s) %s: ",
                  condition != NULL ? condition : "none",
                  tag != NULL ? tag : "NFSMW");
    va_start(arguments, format);
    (void)vfprintf(stderr, format != NULL ? format : "", arguments);
    va_end(arguments);
    (void)fputc('\n', stderr);
    abort();
}

static int compat_android_bitmap_get_info(void *environment, void *bitmap,
                                          void *information)
{
    (void)environment;
    return nfsmw_jni_bitmap_info(bitmap, information);
}

static int compat_android_bitmap_lock(void *environment, void *bitmap,
                                      void **pixels)
{
    (void)environment;
    return nfsmw_jni_bitmap_lock(bitmap, pixels);
}

static int compat_android_bitmap_unlock(void *environment, void *bitmap)
{
    (void)environment;
    return nfsmw_jni_bitmap_unlock(bitmap);
}

static int compat_cxa_atexit(void (*destructor)(void *), void *argument,
                             void *dso)
{
    int result = -1;

    (void)pthread_mutex_lock(&atexit_lock);
    if (destructor != NULL && atexit_count < ATEXIT_CAPACITY) {
        atexit_entries[atexit_count].destructor = destructor;
        atexit_entries[atexit_count].argument = argument;
        atexit_entries[atexit_count].dso = dso;
        atexit_entries[atexit_count].active = true;
        atexit_count += 1U;
        result = 0;
    }
    (void)pthread_mutex_unlock(&atexit_lock);
    return result;
}

static void compat_cxa_finalize(void *dso)
{
    size_t index;

    (void)pthread_mutex_lock(&atexit_lock);
    index = atexit_count;
    while (index != 0U) {
        struct compat_atexit_entry entry;

        index -= 1U;
        if (!atexit_entries[index].active ||
            (dso != NULL && atexit_entries[index].dso != dso)) {
            continue;
        }
        entry = atexit_entries[index];
        atexit_entries[index].active = false;
        (void)pthread_mutex_unlock(&atexit_lock);
        entry.destructor(entry.argument);
        (void)pthread_mutex_lock(&atexit_lock);
    }
    (void)pthread_mutex_unlock(&atexit_lock);
}

static int compat_cxa_thread_atexit(void (*destructor)(void *),
                                    void *argument, void *dso)
{
    return compat_cxa_atexit(destructor, argument, dso);
}

static void translate_stat(struct bionic_stat32 *guest,
                           const struct stat *host)
{
    (void)memset(guest, 0, sizeof(*guest));
    guest->device = (uint64_t)host->st_dev;
    guest->legacy_inode = (uint32_t)host->st_ino;
    guest->mode = (uint32_t)host->st_mode;
    guest->links = (uint32_t)host->st_nlink;
    guest->user = (uint32_t)host->st_uid;
    guest->group = (uint32_t)host->st_gid;
    guest->special_device = (uint64_t)host->st_rdev;
    guest->size = (int64_t)host->st_size;
    guest->block_size = (uint32_t)host->st_blksize;
    guest->blocks = (uint64_t)host->st_blocks;
    guest->access_time.seconds = (int32_t)host->st_atim.tv_sec;
    guest->access_time.nanoseconds = (int32_t)host->st_atim.tv_nsec;
    guest->modification_time.seconds = (int32_t)host->st_mtim.tv_sec;
    guest->modification_time.nanoseconds = (int32_t)host->st_mtim.tv_nsec;
    guest->change_time.seconds = (int32_t)host->st_ctim.tv_sec;
    guest->change_time.nanoseconds = (int32_t)host->st_ctim.tv_nsec;
    guest->inode = (uint64_t)host->st_ino;
}

static int compat_stat(const char *path, struct bionic_stat32 *guest)
{
    struct bionic_stat32 scratch;
    struct bionic_stat32 *out = guest != NULL ? guest : &scratch;
    long result;
    char rewritten[4096];
    const char *orig;

    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    orig = path;
    {
        struct stat host;
        int resource_rc = zip_obb_resource_stat(path, &host);

        if (resource_rc == 0) {
            translate_stat(out, &host);
            g8_resource_log("stat", path, 0);
            return 0;
        }
        if (resource_rc == 1)
            return -1;
    }
    path = rewrite_guest_path(path, rewritten, sizeof(rewritten));
    ensure_test_tmp(path);
    /* Kernel ARM stat64 matches Android ARM struct stat (104 bytes). */
    result = syscall(SYS_stat64, path, out);
    if (result == 0) {
        static int obb_stat_logs;
        if (out->size == 0 ||
            (path != NULL && strstr(path, ".obb") != NULL &&
             obb_stat_logs < 24)) {
            printf("G8-STAT %s size=%lld\n", path, (long long)out->size);
            if (path != NULL && strstr(path, ".obb") != NULL)
                obb_stat_logs += 1;
        }
        return 0;
    }
    if (path_is_zip_inner(orig)) {
        struct stat host;
        if (zip_obb_stat(orig, &host) == 0) {
            static int zip_stat_logs;
            translate_stat(out, &host);
            if (zip_stat_logs < 32) {
                printf("G8-ZIP stat %s size=%lld\n", orig,
                       (long long)host.st_size);
                zip_stat_logs += 1;
            }
            return 0;
        }
    }
    {
        char zippath[4096];
        struct stat host;
        if (unity_data_zip_path(orig, zippath, sizeof(zippath)) == 0 &&
            zip_obb_stat(zippath, &host) == 0) {
            translate_stat(out, &host);
            printf("G8-ZIP stat-via-name %s size=%lld\n", zippath,
                   (long long)host.st_size);
            return 0;
        }
    }
    return -1;
}

static int compat_lstat(const char *path, struct bionic_stat32 *guest)
{
    struct bionic_stat32 scratch;
    struct bionic_stat32 *out = guest != NULL ? guest : &scratch;
    long result;
    char rewritten[4096];

    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    {
        struct stat host;
        int resource_rc = zip_obb_resource_stat(path, &host);

        if (resource_rc == 0) {
            translate_stat(out, &host);
            return 0;
        }
        if (resource_rc == 1)
            return -1;
    }
    path = rewrite_guest_path(path, rewritten, sizeof(rewritten));
    result = syscall(SYS_lstat64, path, out);
    return result == 0 ? 0 : -1;
}

static int compat_fstat(int descriptor, struct bionic_stat32 *guest)
{
    struct bionic_stat32 scratch;
    struct bionic_stat32 *out = guest != NULL ? guest : &scratch;
    long result;
    char pathbuf[256];
    ssize_t nread;

    if (descriptor < 0) {
        errno = EBADF;
        return -1;
    }
    result = syscall(SYS_fstat64, descriptor, out);
    pathbuf[0] = '\0';
    if (result == 0) {
        char link[64];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", descriptor);
        nread = readlink(link, pathbuf, sizeof(pathbuf) - 1U);
        if (nread > 0)
            pathbuf[nread] = '\0';
        if (path_is_test_tmp(pathbuf) && out->size == 0) {
            ssize_t wrote = write(descriptor, "RRR", 3);
            if (wrote > 0)
                (void)lseek(descriptor, 0, SEEK_SET);
            else
                ensure_test_tmp(pathbuf);
            (void)syscall(SYS_fstat64, descriptor, out);
            printf("G8-TMP fstat-seed fd=%d wrote=%zd size=%lld\n",
                   descriptor, wrote, (long long)out->size);
        }
        printf("G8-FSTAT fd=%d size=%lld path=%s\n", descriptor,
               (long long)out->size, pathbuf[0] != '\0' ? pathbuf : "?");
    }
    return result == 0 ? 0 : -1;
}

static int compat_fxstat64(int version, int descriptor,
                           struct bionic_stat32 *guest)
{
    (void)version;
    return compat_fstat(descriptor, guest);
}

static int compat_statfs(const char *path, struct bionic_statfs32 *guest)
{
    struct statfs host;
    const int result = statfs(path, &host);

    if (result == 0 && guest != NULL) {
        (void)memset(guest, 0, sizeof(*guest));
        guest->type = (uint32_t)host.f_type;
        guest->block_size = (uint32_t)host.f_bsize;
        guest->blocks = (uint32_t)host.f_blocks;
        guest->blocks_free = (uint32_t)host.f_bfree;
        guest->blocks_available = (uint32_t)host.f_bavail;
        guest->files = (uint32_t)host.f_files;
        guest->files_free = (uint32_t)host.f_ffree;
        (void)memcpy(guest->fsid, &host.f_fsid, sizeof(guest->fsid));
        guest->name_length = (uint32_t)host.f_namelen;
        guest->fragment_size = (uint32_t)host.f_frsize;
        guest->flags = (uint32_t)host.f_flags;
    }
    return result;
}

static void translate_dirent(struct bionic_dirent32 *guest,
                             const struct dirent *host)
{
    (void)memset(guest, 0, sizeof(*guest));
    guest->inode = (uint64_t)host->d_ino;
    guest->offset = (int64_t)host->d_off;
    guest->record_length = (uint16_t)sizeof(*guest);
    guest->type = host->d_type;
    (void)snprintf(guest->name, sizeof(guest->name), "%s", host->d_name);
}

static void *compat_opendir(const char *path) { return opendir(path); }
static int compat_closedir(void *directory) { return closedir(directory); }
static struct bionic_dirent32 *compat_readdir(void *directory)
{
    struct dirent *entry = readdir(directory);

    if (entry == NULL) {
        return NULL;
    }
    translate_dirent(&directory_entry, entry);
    return &directory_entry;
}

static int compat_readdir_r(void *directory, struct bionic_dirent32 *entry,
                            struct bionic_dirent32 **result)
{
    struct dirent *host;

    errno = 0;
    host = readdir(directory);
    if (host == NULL) {
        *result = NULL;
        return errno;
    }
    translate_dirent(entry, host);
    *result = entry;
    return 0;
}

static uintptr_t guest_slot_value(const void *guest)
{
    uintptr_t value = 0U;

    (void)memcpy(&value, guest, sizeof(value));
    return value;
}

static void set_guest_slot(void *guest, const void *value)
{
    (void)memcpy(guest, &value, sizeof(value));
}

static int mutex_type_from_guest(const void *attribute)
{
    int32_t type = 0;

    if (attribute != NULL) {
        (void)memcpy(&type, attribute, sizeof(type));
    }
    if (type == 1) {
        return PTHREAD_MUTEX_RECURSIVE;
    }
    if (type == 2) {
        return PTHREAD_MUTEX_ERRORCHECK;
    }
    return PTHREAD_MUTEX_NORMAL;
}

static pthread_mutex_t *compat_mutex_get(void *guest, bool create,
                                         const void *attribute)
{
    uintptr_t raw = guest_slot_value(guest);
    pthread_mutex_t *host;

    if (raw > UINT32_C(0x0000ffff)) {
        return (pthread_mutex_t *)raw;
    }
    if (!create) {
        return NULL;
    }
    host = calloc(1U, sizeof(*host));
    if (host != NULL) {
        pthread_mutexattr_t host_attribute;
        int type = mutex_type_from_guest(attribute);

        if (raw != 0U) {
            const unsigned int encoded =
                (unsigned int)((raw >> 14U) & UINT32_C(3));
            type = encoded == 1U ? PTHREAD_MUTEX_RECURSIVE :
                   encoded == 2U ? PTHREAD_MUTEX_ERRORCHECK : type;
        }
        (void)pthread_mutexattr_init(&host_attribute);
        (void)pthread_mutexattr_settype(&host_attribute, type);
        if (pthread_mutex_init(host, &host_attribute) != 0) {
            free(host);
            host = NULL;
        }
        (void)pthread_mutexattr_destroy(&host_attribute);
        if (host != NULL) {
            set_guest_slot(guest, host);
        }
    }
    return host;
}

static int compat_pthread_mutex_init(void *guest, const void *attribute)
{
    set_guest_slot(guest, NULL);
    return compat_mutex_get(guest, true, attribute) != NULL ? 0 : ENOMEM;
}
static int compat_pthread_mutex_destroy(void *guest)
{
    pthread_mutex_t *host = compat_mutex_get(guest, false, NULL);
    int result = 0;

    if (host != NULL) {
        result = pthread_mutex_destroy(host);
        if (result == 0) {
            free(host);
            set_guest_slot(guest, NULL);
        }
    }
    return result;
}
static int compat_pthread_mutex_lock(void *guest)
{
    pthread_mutex_t *host = compat_mutex_get(guest, true, NULL);
    return host != NULL ? pthread_mutex_lock(host) : ENOMEM;
}
static int compat_pthread_mutex_unlock(void *guest)
{
    pthread_mutex_t *host = compat_mutex_get(guest, false, NULL);
    return host != NULL ? pthread_mutex_unlock(host) : EINVAL;
}
static int compat_pthread_mutex_trylock(void *guest)
{
    pthread_mutex_t *host = compat_mutex_get(guest, true, NULL);
    return host != NULL ? pthread_mutex_trylock(host) : ENOMEM;
}

static pthread_cond_t *compat_cond_get(void *guest, bool create)
{
    uintptr_t raw = guest_slot_value(guest);
    pthread_cond_t *host;

    if (raw > UINT32_C(0x0000ffff)) {
        return (pthread_cond_t *)raw;
    }
    if (!create) {
        return NULL;
    }
    host = calloc(1U, sizeof(*host));
    if (host != NULL && pthread_cond_init(host, NULL) != 0) {
        free(host);
        host = NULL;
    }
    if (host != NULL) {
        set_guest_slot(guest, host);
    }
    return host;
}

static int compat_pthread_cond_init(void *guest, const void *attribute)
{
    (void)attribute;
    set_guest_slot(guest, NULL);
    return compat_cond_get(guest, true) != NULL ? 0 : ENOMEM;
}
static int compat_pthread_cond_destroy(void *guest)
{
    pthread_cond_t *host = compat_cond_get(guest, false);
    int result = 0;

    if (host != NULL) {
        result = pthread_cond_destroy(host);
        if (result == 0) {
            free(host);
            set_guest_slot(guest, NULL);
        }
    }
    return result;
}
static int compat_pthread_cond_signal(void *guest)
{
    pthread_cond_t *host = compat_cond_get(guest, true);
    return host != NULL ? pthread_cond_signal(host) : ENOMEM;
}
static int compat_pthread_cond_broadcast(void *guest)
{
    pthread_cond_t *host = compat_cond_get(guest, true);
    return host != NULL ? pthread_cond_broadcast(host) : ENOMEM;
}
static int compat_pthread_cond_wait(void *condition, void *mutex)
{
    pthread_cond_t *host_condition = compat_cond_get(condition, true);
    pthread_mutex_t *host_mutex = compat_mutex_get(mutex, true, NULL);
    return host_condition != NULL && host_mutex != NULL ?
        pthread_cond_wait(host_condition, host_mutex) : ENOMEM;
}
static int compat_pthread_cond_timedwait(
    void *condition, void *mutex, const struct bionic_timespec32 *timeout)
{
    pthread_cond_t *host_condition = compat_cond_get(condition, true);
    pthread_mutex_t *host_mutex = compat_mutex_get(mutex, true, NULL);
    struct timespec host_timeout;

    if (host_condition == NULL || host_mutex == NULL || timeout == NULL) {
        return EINVAL;
    }
    host_timeout.tv_sec = (time_t)timeout->seconds;
    host_timeout.tv_nsec = (long)timeout->nanoseconds;
    return pthread_cond_timedwait(host_condition, host_mutex, &host_timeout);
}

static pthread_rwlock_t *compat_rwlock_get(void *guest, bool create)
{
    uintptr_t raw = guest_slot_value(guest);
    pthread_rwlock_t *host;

    if (raw > UINT32_C(0x0000ffff)) {
        return (pthread_rwlock_t *)raw;
    }
    if (!create) {
        return NULL;
    }
    host = calloc(1U, sizeof(*host));
    if (host != NULL && pthread_rwlock_init(host, NULL) != 0) {
        free(host);
        host = NULL;
    }
    if (host != NULL) {
        set_guest_slot(guest, host);
    }
    return host;
}

static int compat_pthread_rwlock_init(void *guest, const void *attribute)
{
    (void)attribute;
    (void)memset(guest, 0, 40U);
    return compat_rwlock_get(guest, true) != NULL ? 0 : ENOMEM;
}
static int compat_pthread_rwlock_destroy(void *guest)
{
    pthread_rwlock_t *host = compat_rwlock_get(guest, false);
    int result = 0;
    if (host != NULL) {
        result = pthread_rwlock_destroy(host);
        if (result == 0) {
            free(host);
            (void)memset(guest, 0, 40U);
        }
    }
    return result;
}
static int compat_pthread_rwlock_rdlock(void *guest)
{
    pthread_rwlock_t *host = compat_rwlock_get(guest, true);
    return host != NULL ? pthread_rwlock_rdlock(host) : ENOMEM;
}
static int compat_pthread_rwlock_wrlock(void *guest)
{
    pthread_rwlock_t *host = compat_rwlock_get(guest, true);
    return host != NULL ? pthread_rwlock_wrlock(host) : ENOMEM;
}
static int compat_pthread_rwlock_unlock(void *guest)
{
    pthread_rwlock_t *host = compat_rwlock_get(guest, false);
    return host != NULL ? pthread_rwlock_unlock(host) : EINVAL;
}

static sem_t *compat_sem_get(void *guest, bool create, unsigned int value)
{
    uintptr_t raw = guest_slot_value(guest);
    sem_t *host;

    if (raw > UINT32_C(0x0000ffff)) {
        return (sem_t *)raw;
    }
    if (!create) {
        return NULL;
    }
    host = calloc(1U, sizeof(*host));
    if (host != NULL && sem_init(host, 0, value) != 0) {
        free(host);
        host = NULL;
    }
    if (host != NULL) {
        set_guest_slot(guest, host);
    }
    return host;
}

static int compat_sem_init(void *guest, int shared, unsigned int value)
{
    if (shared != 0) {
        return -1;
    }
    set_guest_slot(guest, NULL);
    return compat_sem_get(guest, true, value) != NULL ? 0 : -1;
}
static int compat_sem_destroy(void *guest)
{
    sem_t *host = compat_sem_get(guest, false, 0U);
    int result = 0;
    if (host != NULL) {
        result = sem_destroy(host);
        if (result == 0) {
            free(host);
            set_guest_slot(guest, NULL);
        }
    }
    return result;
}
static int compat_sem_wait(void *guest)
{
    sem_t *host = compat_sem_get(guest, true, 0U);
    return host != NULL ? sem_wait(host) : -1;
}
static int compat_sem_post(void *guest)
{
    sem_t *host = compat_sem_get(guest, true, 0U);
    return host != NULL ? sem_post(host) : -1;
}
static int compat_sem_trywait(void *guest)
{
    sem_t *host = compat_sem_get(guest, true, 0U);
    return host != NULL ? sem_trywait(host) : -1;
}
static int compat_sem_getvalue(void *guest, int *value)
{
    sem_t *host = compat_sem_get(guest, true, 0U);
    return host != NULL ? sem_getvalue(host, value) : -1;
}
static int compat_sem_timedwait(void *guest,
                                const struct bionic_timespec32 *timeout)
{
    sem_t *host = compat_sem_get(guest, true, 0U);
    struct timespec host_timeout;
    if (host == NULL || timeout == NULL) {
        return -1;
    }
    host_timeout.tv_sec = (time_t)timeout->seconds;
    host_timeout.tv_nsec = (long)timeout->nanoseconds;
    return sem_timedwait(host, &host_timeout);
}

static int compat_pthread_attr_init(struct bionic_pthread_attr32 *attribute)
{
    (void)memset(attribute, 0, sizeof(*attribute));
    attribute->stack_size = 1024U * 1024U;
    return 0;
}
static int compat_pthread_attr_destroy(struct bionic_pthread_attr32 *attribute)
{
    (void)attribute;
    return 0;
}
static int compat_pthread_attr_setstacksize(
    struct bionic_pthread_attr32 *attribute, uint32_t size)
{
    attribute->stack_size = size;
    return 0;
}
static int compat_pthread_attr_setstack(struct bionic_pthread_attr32 *attribute,
                                        void *base, uint32_t size)
{
    attribute->stack_base = base;
    attribute->stack_size = size;
    return 0;
}
static int compat_pthread_attr_getstack(
    const struct bionic_pthread_attr32 *attribute, void **base, uint32_t *size)
{
    if (base != NULL) {
        *base = attribute->stack_base;
    }
    if (size != NULL) {
        *size = attribute->stack_size;
    }
    return 0;
}
static int compat_pthread_attr_setschedparam(
    struct bionic_pthread_attr32 *attribute, const struct sched_param *parameter)
{
    attribute->scheduling_priority = parameter->sched_priority;
    return 0;
}
static int compat_pthread_attr_setschedpolicy(
    struct bionic_pthread_attr32 *attribute, int policy)
{
    attribute->scheduling_policy = policy;
    return 0;
}
static int compat_pthread_attr_setdetachstate(
    struct bionic_pthread_attr32 *attribute, int state)
{
    if (state != 0) {
        attribute->flags |= 1U;
    } else {
        attribute->flags &= ~1U;
    }
    return 0;
}

static int host_attr(const struct bionic_pthread_attr32 *guest,
                     pthread_attr_t *host)
{
    int result = pthread_attr_init(host);
    if (result != 0 || guest == NULL) {
        return result;
    }
    if (guest->stack_base != NULL && guest->stack_size != 0U) {
        result = pthread_attr_setstack(host, guest->stack_base,
                                       guest->stack_size);
    } else if (guest->stack_size != 0U) {
        result = pthread_attr_setstacksize(host, guest->stack_size);
    }
    if (result == 0 && (guest->flags & 1U) != 0U) {
        result = pthread_attr_setdetachstate(host, PTHREAD_CREATE_DETACHED);
    }
    return result;
}

static int compat_pthread_create(uint32_t *thread,
                                 const struct bionic_pthread_attr32 *attribute,
                                 void *(*routine)(void *), void *argument)
{
    pthread_attr_t host_attribute;
    pthread_t host_thread;
    int result = host_attr(attribute, &host_attribute);

    if (result == 0) {
        result = pthread_create(&host_thread, &host_attribute,
                                routine, argument);
    }
    (void)pthread_attr_destroy(&host_attribute);
    if (result == 0) {
        *thread = (uint32_t)host_thread;
    }
    return result;
}

static uint32_t compat_pthread_self(void)
{
    return (uint32_t)pthread_self();
}
static int compat_pthread_equal(uint32_t left, uint32_t right)
{
    return pthread_equal((pthread_t)left, (pthread_t)right);
}
static int compat_pthread_join(uint32_t thread, void **result)
{
    return pthread_join((pthread_t)thread, result);
}
static int compat_pthread_detach(uint32_t thread)
{
    return pthread_detach((pthread_t)thread);
}
static void compat_pthread_exit(void *result) { pthread_exit(result); }
static int compat_pthread_setschedparam(uint32_t thread, int policy,
                                        const struct sched_param *parameter)
{
    return pthread_setschedparam((pthread_t)thread, policy, parameter);
}
static int compat_pthread_getschedparam(uint32_t thread, int *policy,
                                        struct sched_param *parameter)
{
    return pthread_getschedparam((pthread_t)thread, policy, parameter);
}
static int compat_pthread_getattr_np(uint32_t thread,
                                     struct bionic_pthread_attr32 *guest)
{
    pthread_attr_t host;
    void *base = NULL;
    size_t size = 0U;
    int result = pthread_getattr_np((pthread_t)thread, &host);

    if (result == 0) {
        (void)compat_pthread_attr_init(guest);
        (void)pthread_attr_getstack(&host, &base, &size);
        guest->stack_base = base;
        guest->stack_size = (uint32_t)size;
        (void)pthread_attr_destroy(&host);
    }
    return result;
}

static int compat_pthread_once(int32_t *once,
                               void (*initialization)(void))
{
    return pthread_once((pthread_once_t *)once, initialization);
}
static int compat_pthread_key_create(uint32_t *key,
                                     void (*destructor)(void *))
{
    pthread_key_t host_key;
    const int result = pthread_key_create(&host_key, destructor);
    if (result == 0) {
        *key = (uint32_t)host_key;
    }
    return result;
}
static int compat_pthread_key_delete(uint32_t key)
{
    return pthread_key_delete((pthread_key_t)key);
}
static int compat_pthread_setspecific(uint32_t key, const void *value)
{
    return pthread_setspecific((pthread_key_t)key, value);
}
static void *compat_pthread_getspecific(uint32_t key)
{
    return pthread_getspecific((pthread_key_t)key);
}

static int compat_mutexattr_init(int32_t *attribute)
{
    *attribute = 0;
    return 0;
}
static int compat_mutexattr_destroy(int32_t *attribute)
{
    (void)attribute;
    return 0;
}
static int compat_mutexattr_settype(int32_t *attribute, int type)
{
    *attribute = type;
    return 0;
}
static int compat_mutexattr_setpshared(int32_t *attribute, int shared)
{
    (void)attribute;
    return shared == 0 ? 0 : ENOTSUP;
}

static int compat_sigaction(int signal_number, const void *action,
                            void *old_action)
{
    const struct bionic_sigaction *guest_action = action;
    struct bionic_sigaction *guest_old_action = old_action;
    struct sigaction host_action;
    struct sigaction host_old_action;
    struct sigaction *host_action_pointer = NULL;
    struct sigaction *host_old_action_pointer = NULL;
    int index;
    int result;

    if (guest_action != NULL) {
        (void)memset(&host_action, 0, sizeof(host_action));
        host_action.sa_handler = guest_action->handler;
        host_action.sa_flags =
            guest_action->flags & ~BIONIC_SA_RESTORER;
        (void)sigemptyset(&host_action.sa_mask);
        for (index = 1; index <= 32; ++index) {
            if ((guest_action->mask &
                 (UINT32_C(1) << (unsigned int)(index - 1))) != 0U) {
                (void)sigaddset(&host_action.sa_mask, index);
            }
        }
        host_action_pointer = &host_action;
    }
    if (guest_old_action != NULL) {
        host_old_action_pointer = &host_old_action;
    }
    result = sigaction(signal_number, host_action_pointer,
                       host_old_action_pointer);
    if (result == 0 && guest_old_action != NULL) {
        guest_old_action->handler = host_old_action.sa_handler;
        guest_old_action->mask = 0U;
        for (index = 1; index <= 32; ++index) {
            if (sigismember(&host_old_action.sa_mask, index) == 1) {
                guest_old_action->mask |=
                    UINT32_C(1) << (unsigned int)(index - 1);
            }
        }
        guest_old_action->flags = host_old_action.sa_flags;
        guest_old_action->restorer = NULL;
    }
    return result;
}

static void host_mask_from_bionic(uint32_t guest_mask, sigset_t *host_mask)
{
    int index;

    (void)sigemptyset(host_mask);
    for (index = 1; index <= 32; ++index) {
        if ((guest_mask &
             (UINT32_C(1) << (unsigned int)(index - 1))) != 0U) {
            (void)sigaddset(host_mask, index);
        }
    }
}

static uint32_t bionic_mask_from_host(const sigset_t *host_mask)
{
    uint32_t guest_mask = 0U;
    int index;

    for (index = 1; index <= 32; ++index) {
        if (sigismember(host_mask, index) == 1) {
            guest_mask |= UINT32_C(1) << (unsigned int)(index - 1);
        }
    }
    return guest_mask;
}

static int compat_sigprocmask(int how, const uint32_t *guest_set,
                              uint32_t *guest_old_set)
{
    sigset_t host_set;
    sigset_t host_old_set;
    const sigset_t *host_set_pointer = NULL;
    sigset_t *host_old_set_pointer = NULL;
    int result;

    if (guest_set != NULL) {
        host_mask_from_bionic(*guest_set, &host_set);
        host_set_pointer = &host_set;
    }
    if (guest_old_set != NULL) {
        host_old_set_pointer = &host_old_set;
    }
    result = sigprocmask(how, host_set_pointer, host_old_set_pointer);
    if (result == 0 && guest_old_set != NULL) {
        *guest_old_set = bionic_mask_from_host(&host_old_set);
    }
    return result;
}

uint32_t nfsmw_bionic_signal_mask_capture(void)
{
    sigset_t host_mask;

    if (sigprocmask(SIG_SETMASK, NULL, &host_mask) != 0) {
        return 0U;
    }
    return bionic_mask_from_host(&host_mask);
}

void nfsmw_bionic_signal_mask_restore(uint32_t mask)
{
    sigset_t host_mask;

    host_mask_from_bionic(mask, &host_mask);
    (void)sigprocmask(SIG_SETMASK, &host_mask, NULL);
}

void nfsmw_bionic_longjmp_error(void)
{
    (void)fprintf(stderr, "Invalid Bionic ARM jump buffer\n");
    abort();
}

static uint32_t signal_selftest_environment[65]
    __attribute__((aligned(8)));

static void signal_selftest_handler(int signal_number)
{
    (void)signal_number;
    nfsmw_bionic_siglongjmp(signal_selftest_environment, 9);
}

int nfsmw_compat_signal_selftest(void)
{
    const struct bionic_sigaction test_action = {
        signal_selftest_handler, 0U, 0, NULL
    };
    struct bionic_sigaction saved_action;
    uint32_t saved_mask;
    int jump_value;

    if (compat_sigprocmask(SIG_SETMASK, NULL, &saved_mask) != 0 ||
        compat_sigaction(SIGUSR1, &test_action, &saved_action) != 0) {
        return -1;
    }
    jump_value = nfsmw_bionic_sigsetjmp(signal_selftest_environment, 1);
    if (jump_value == 0 && raise(SIGUSR1) != 0) {
        jump_value = -1;
    }
    (void)compat_sigaction(SIGUSR1, &saved_action, NULL);
    (void)compat_sigprocmask(SIG_SETMASK, &saved_mask, NULL);
    return jump_value == 9 ? 0 : -1;
}

void nfsmw_compat_init(void)
{
    size_t index;

    (void)memset(bionic_files, 0, sizeof(bionic_files));
    (void)memset(atexit_entries, 0, sizeof(atexit_entries));
    atexit_count = 0U;
    guest_image_count = 0U;
    (void)memset(ctype_storage, 0, sizeof(ctype_storage));
    for (index = 0U; index < 256U; ++index) {
        unsigned char flags = 0U;
        int16_t lower = (int16_t)index;
        if (index >= (size_t)'A' && index <= (size_t)'Z') {
            flags |= 0x01U;
            lower = (int16_t)(index + ((size_t)'a' - (size_t)'A'));
        }
        if (index >= (size_t)'a' && index <= (size_t)'z') {
            flags |= 0x02U;
        }
        if (index >= (size_t)'0' && index <= (size_t)'9') {
            flags |= 0x04U;
        }
        if (index < 32U || index == 127U) {
            flags |= 0x08U;
        }
        if (index >= (size_t)'!' && index <= (size_t)'~' &&
            (flags & 0x07U) == 0U) {
            flags |= 0x10U;
        }
        if (index == (size_t)' ' || (index >= 9U && index <= 13U)) {
            flags |= 0x20U;
        }
        if ((index >= (size_t)'0' && index <= (size_t)'9') ||
            (index >= (size_t)'A' && index <= (size_t)'F') ||
            (index >= (size_t)'a' && index <= (size_t)'f')) {
            flags |= 0x40U;
        }
        if (index == (size_t)' ' || index == (size_t)'\t') {
            flags |= 0x80U;
        }
        ctype_storage[index + 1U] = (char)flags;
        tolower_storage[index + 1U] = lower;
        toupper_storage[index + 1U] = (int16_t)(
            (index >= (size_t)'a' && index <= (size_t)'z') ?
            (index - ((size_t)'a' - (size_t)'A')) : index);
    }
    tolower_storage[0] = -1;
    toupper_storage[0] = -1;
    bind_gles_softfp_once();
}

void nfsmw_compat_finalize(void)
{
    compat_cxa_finalize(NULL);
}

static int compat_system_property_get(const char *name, char *value)
{
    (void)name;
    if (value != NULL)
        value[0] = '\0';
    return 0;
}

static size_t compat_strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = src != NULL ? strlen(src) : 0U;
    if (dst != NULL && size > 0U) {
        size_t copy = length < size - 1U ? length : size - 1U;
        if (src != NULL && copy > 0U)
            memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return length;
}

static int compat_tkill(int thread_id, int signal_number)
{
    return (int)syscall(SYS_tkill, thread_id, signal_number);
}

static int compat_isfinitef(float value)
{
    (void)value;
    return 1;
}

static int compat_pthread_setname_np(uint32_t thread, const char *name)
{
    (void)thread;
    (void)name;
    return 0;
}

static int compat_pthread_sigmask(int how, const uint32_t *guest_set,
                                  uint32_t *guest_old_set)
{
    return compat_sigprocmask(how, guest_set, guest_old_set);
}

static int compat_pthread_attr_getdetachstate(
    const struct bionic_pthread_attr32 *attribute, int *state)
{
    if (state == NULL)
        return EINVAL;
    *state = (attribute != NULL && (attribute->flags & 1U) != 0U) ? 1 : 0;
    return 0;
}

static int compat_pthread_cond_timedwait_monotonic_np(
    void *condition, void *mutex, const struct bionic_timespec32 *timeout)
{
    return compat_pthread_cond_timedwait(condition, mutex, timeout);
}

static int compat_pthread_cond_timedwait_relative_np(
    void *condition, void *mutex, const struct bionic_timespec32 *relative)
{
    struct bionic_timespec32 absolute;
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return errno;
    absolute.seconds = (int32_t)now.tv_sec +
        (relative != NULL ? relative->seconds : 0);
    absolute.nanoseconds = (int32_t)now.tv_nsec +
        (relative != NULL ? relative->nanoseconds : 0);
    if (absolute.nanoseconds >= 1000000000) {
        absolute.seconds += 1;
        absolute.nanoseconds -= 1000000000;
    }
    return compat_pthread_cond_timedwait(condition, mutex, &absolute);
}

static void compat_pthread_cleanup_push(void *cleanup, void (*fn)(void *),
                                        void *argument)
{
    (void)cleanup;
    (void)fn;
    (void)argument;
}

static void compat_pthread_cleanup_pop(void *cleanup, int execute)
{
    (void)cleanup;
    (void)execute;
}

static void compat_sha1_init(void *context) { (void)context; }
static void compat_sha1_update(void *context, const void *data, size_t length)
{
    (void)context;
    (void)data;
    (void)length;
}
static void compat_sha1_final(unsigned char *digest, void *context)
{
    (void)context;
    if (digest != NULL)
        (void)memset(digest, 0, 20U);
}

#define RESOLVE_FUNCTION(symbol, function)                                  \
    do {                                                                    \
        if (strcmp(name, (symbol)) == 0) {                                  \
            return (uintptr_t)&(function);                                  \
        }                                                                   \
    } while (0)

uintptr_t nfsmw_compat_resolve(const char *name)
{
    if (name == NULL) {
        return 0U;
    }
    if (strcmp(name, "__sF") == 0) return (uintptr_t)&bionic_files[0][0];
    if (strcmp(name, "_ctype_") == 0) return (uintptr_t)&ctype_pointer;
    if (strcmp(name, "_tolower_tab_") == 0) return (uintptr_t)&tolower_pointer;
    if (strcmp(name, "_toupper_tab_") == 0) return (uintptr_t)&toupper_pointer;
    RESOLVE_FUNCTION("__errno", compat_errno);
    RESOLVE_FUNCTION("abort", compat_abort);
    RESOLVE_FUNCTION("__assert2", compat_assert2);
    RESOLVE_FUNCTION("clock_gettime", compat_clock_gettime);
    RESOLVE_FUNCTION("gettimeofday", compat_gettimeofday);
    RESOLVE_FUNCTION("dlopen", compat_dlopen);
    RESOLVE_FUNCTION("dlsym", compat_dlsym);
    RESOLVE_FUNCTION("dlclose", compat_dlclose);
    RESOLVE_FUNCTION("eglGetProcAddress", compat_eglGetProcAddress);
    RESOLVE_FUNCTION("fclose", compat_fclose);
    RESOLVE_FUNCTION("fdopen", compat_fdopen);
    RESOLVE_FUNCTION("fflush", compat_fflush);
    RESOLVE_FUNCTION("fopen", compat_fopen);
    RESOLVE_FUNCTION("fopen64", compat_fopen64);
    RESOLVE_FUNCTION("open", compat_open);
    RESOLVE_FUNCTION("openat", compat_openat);
    RESOLVE_FUNCTION("openat64", compat_openat);
    RESOLVE_FUNCTION("open64", compat_open);
    RESOLVE_FUNCTION("access", compat_access);
    RESOLVE_FUNCTION("fprintf", compat_fprintf);
    RESOLVE_FUNCTION("fread", compat_fread);
    RESOLVE_FUNCTION("fseek", compat_fseek);
    RESOLVE_FUNCTION("fseeko", compat_fseeko);
    RESOLVE_FUNCTION("fseeko64", compat_fseeko);
    RESOLVE_FUNCTION("ftell", compat_ftell);
    RESOLVE_FUNCTION("ftello", compat_ftello);
    RESOLVE_FUNCTION("fwide", compat_fwide);
    RESOLVE_FUNCTION("fwrite", compat_fwrite);
    RESOLVE_FUNCTION("vfprintf", compat_vfprintf);
    RESOLVE_FUNCTION("stat", compat_stat);
    RESOLVE_FUNCTION("stat64", compat_stat);
    RESOLVE_FUNCTION("lstat", compat_lstat);
    RESOLVE_FUNCTION("lstat64", compat_lstat);
    RESOLVE_FUNCTION("fstat", compat_fstat);
    RESOLVE_FUNCTION("fstat64", compat_fstat);
    RESOLVE_FUNCTION("__fxstat64", compat_fxstat64);
    RESOLVE_FUNCTION("statfs", compat_statfs);
    RESOLVE_FUNCTION("opendir", compat_opendir);
    RESOLVE_FUNCTION("closedir", compat_closedir);
    RESOLVE_FUNCTION("readdir", compat_readdir);
    RESOLVE_FUNCTION("readdir_r", compat_readdir_r);
    RESOLVE_FUNCTION("__android_log_print", compat_android_log_print);
    RESOLVE_FUNCTION("__android_log_vprint", compat_android_log_vprint);
    RESOLVE_FUNCTION("__android_log_write", compat_android_log_write);
    RESOLVE_FUNCTION("__android_log_assert", compat_android_log_assert);
    RESOLVE_FUNCTION("AndroidBitmap_getInfo", compat_android_bitmap_get_info);
    RESOLVE_FUNCTION("AndroidBitmap_lockPixels", compat_android_bitmap_lock);
    RESOLVE_FUNCTION("AndroidBitmap_unlockPixels", compat_android_bitmap_unlock);
    RESOLVE_FUNCTION("__cxa_atexit", compat_cxa_atexit);
    RESOLVE_FUNCTION("__cxa_finalize", compat_cxa_finalize);
    RESOLVE_FUNCTION("__cxa_thread_atexit_impl", compat_cxa_thread_atexit);
    RESOLVE_FUNCTION("pthread_mutexattr_init", compat_mutexattr_init);
    RESOLVE_FUNCTION("pthread_mutexattr_destroy", compat_mutexattr_destroy);
    RESOLVE_FUNCTION("pthread_mutexattr_settype", compat_mutexattr_settype);
    RESOLVE_FUNCTION("pthread_mutexattr_setpshared", compat_mutexattr_setpshared);
    RESOLVE_FUNCTION("pthread_mutex_init", compat_pthread_mutex_init);
    RESOLVE_FUNCTION("pthread_mutex_destroy", compat_pthread_mutex_destroy);
    RESOLVE_FUNCTION("pthread_mutex_lock", compat_pthread_mutex_lock);
    RESOLVE_FUNCTION("pthread_mutex_unlock", compat_pthread_mutex_unlock);
    RESOLVE_FUNCTION("pthread_mutex_trylock", compat_pthread_mutex_trylock);
    RESOLVE_FUNCTION("pthread_cond_init", compat_pthread_cond_init);
    RESOLVE_FUNCTION("pthread_cond_destroy", compat_pthread_cond_destroy);
    RESOLVE_FUNCTION("pthread_cond_signal", compat_pthread_cond_signal);
    RESOLVE_FUNCTION("pthread_cond_broadcast", compat_pthread_cond_broadcast);
    RESOLVE_FUNCTION("pthread_cond_wait", compat_pthread_cond_wait);
    RESOLVE_FUNCTION("pthread_cond_timedwait", compat_pthread_cond_timedwait);
    RESOLVE_FUNCTION("pthread_cond_timedwait_monotonic_np",
                     compat_pthread_cond_timedwait_monotonic_np);
    RESOLVE_FUNCTION("pthread_cond_timedwait_relative_np",
                     compat_pthread_cond_timedwait_relative_np);
    RESOLVE_FUNCTION("pthread_rwlock_init", compat_pthread_rwlock_init);
    RESOLVE_FUNCTION("pthread_rwlock_destroy", compat_pthread_rwlock_destroy);
    RESOLVE_FUNCTION("pthread_rwlock_rdlock", compat_pthread_rwlock_rdlock);
    RESOLVE_FUNCTION("pthread_rwlock_wrlock", compat_pthread_rwlock_wrlock);
    RESOLVE_FUNCTION("pthread_rwlock_unlock", compat_pthread_rwlock_unlock);
    RESOLVE_FUNCTION("pthread_attr_init", compat_pthread_attr_init);
    RESOLVE_FUNCTION("pthread_attr_destroy", compat_pthread_attr_destroy);
    RESOLVE_FUNCTION("pthread_attr_setstacksize", compat_pthread_attr_setstacksize);
    RESOLVE_FUNCTION("pthread_attr_setstack", compat_pthread_attr_setstack);
    RESOLVE_FUNCTION("pthread_attr_getstack", compat_pthread_attr_getstack);
    RESOLVE_FUNCTION("pthread_attr_setschedparam", compat_pthread_attr_setschedparam);
    RESOLVE_FUNCTION("pthread_attr_setschedpolicy", compat_pthread_attr_setschedpolicy);
    RESOLVE_FUNCTION("pthread_attr_setdetachstate", compat_pthread_attr_setdetachstate);
    RESOLVE_FUNCTION("pthread_attr_getdetachstate", compat_pthread_attr_getdetachstate);
    RESOLVE_FUNCTION("pthread_create", compat_pthread_create);
    RESOLVE_FUNCTION("pthread_self", compat_pthread_self);
    RESOLVE_FUNCTION("pthread_equal", compat_pthread_equal);
    RESOLVE_FUNCTION("pthread_join", compat_pthread_join);
    RESOLVE_FUNCTION("pthread_detach", compat_pthread_detach);
    RESOLVE_FUNCTION("pthread_exit", compat_pthread_exit);
    RESOLVE_FUNCTION("pthread_setschedparam", compat_pthread_setschedparam);
    RESOLVE_FUNCTION("pthread_getschedparam", compat_pthread_getschedparam);
    RESOLVE_FUNCTION("pthread_getattr_np", compat_pthread_getattr_np);
    RESOLVE_FUNCTION("pthread_once", compat_pthread_once);
    RESOLVE_FUNCTION("pthread_key_create", compat_pthread_key_create);
    RESOLVE_FUNCTION("pthread_key_delete", compat_pthread_key_delete);
    RESOLVE_FUNCTION("pthread_setspecific", compat_pthread_setspecific);
    RESOLVE_FUNCTION("pthread_getspecific", compat_pthread_getspecific);
    RESOLVE_FUNCTION("sem_init", compat_sem_init);
    RESOLVE_FUNCTION("sem_destroy", compat_sem_destroy);
    RESOLVE_FUNCTION("sem_wait", compat_sem_wait);
    RESOLVE_FUNCTION("sem_post", compat_sem_post);
    RESOLVE_FUNCTION("sem_trywait", compat_sem_trywait);
    RESOLVE_FUNCTION("sem_getvalue", compat_sem_getvalue);
    RESOLVE_FUNCTION("sem_timedwait", compat_sem_timedwait);
    RESOLVE_FUNCTION("sigaction", compat_sigaction);
    RESOLVE_FUNCTION("sigprocmask", compat_sigprocmask);
    RESOLVE_FUNCTION("setjmp", nfsmw_bionic_setjmp);
    RESOLVE_FUNCTION("sigsetjmp", nfsmw_bionic_sigsetjmp);
    RESOLVE_FUNCTION("longjmp", nfsmw_bionic_longjmp);
    RESOLVE_FUNCTION("siglongjmp", nfsmw_bionic_siglongjmp);
    RESOLVE_FUNCTION("pthread_setname_np", compat_pthread_setname_np);
    RESOLVE_FUNCTION("pthread_sigmask", compat_pthread_sigmask);
    RESOLVE_FUNCTION("__pthread_cleanup_push", compat_pthread_cleanup_push);
    RESOLVE_FUNCTION("__pthread_cleanup_pop", compat_pthread_cleanup_pop);
    RESOLVE_FUNCTION("__system_property_get", compat_system_property_get);
    RESOLVE_FUNCTION("strlcpy", compat_strlcpy);
    RESOLVE_FUNCTION("tkill", compat_tkill);
    RESOLVE_FUNCTION("__isfinitef", compat_isfinitef);
    RESOLVE_FUNCTION("SHA1Init", compat_sha1_init);
    RESOLVE_FUNCTION("SHA1Update", compat_sha1_update);
    RESOLVE_FUNCTION("SHA1Final", compat_sha1_final);
    {
        const uintptr_t android = sg_android_resolve(name);
        if (android != 0U)
            return android;
    }
    return 0U;
}
