#include "android_native.h"
#include "screen_size.h"
#include "opensl_bridge.h"
#include "zip_obb.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    ANDROID_MAGIC = 0xA2D10000u
};

struct sg_window {
    int32_t magic;
    int32_t refs;
    int32_t width;
    int32_t height;
};

static struct sg_window window;
static unsigned char android_lib_token;
static unsigned char sensor_token;
static unsigned char config_token;
static unsigned char input_queue_token;
static unsigned char asset_manager_token;

enum { ASSET_MAGIC = 0x41534e54u, PATH_MAX_ASSET = 4096 };

struct sg_asset {
    uint32_t magic;
    FILE *file;
    char *buffer;
    long length;
    long position;
};

static const char *android_root(void)
{
    const char *root = getenv("SG_ROOT");
    return (root != NULL && root[0] != '\0') ? root : ".";
}

static FILE *open_asset_file(const char *filename, char *opened, size_t opened_size)
{
    const char *root = android_root();
    const char *name = filename != NULL ? filename : "";
    char path[PATH_MAX_ASSET];
    FILE *file;
    const char *prefix;
    size_t index;
    static const char *dirs[] = {
        "assets",
        "assets/bin/Data",
        "files",
        "files/Managed/Metadata",
        "files/il2cpp/Metadata",
        NULL
    };

    if (opened != NULL && opened_size > 0U)
        opened[0] = '\0';
    if (name[0] == '/') {
        file = fopen(name, "rb");
        if (file != NULL) {
            if (opened != NULL)
                snprintf(opened, opened_size, "%s", name);
            return file;
        }
    }
    if (strncmp(name, "assets/", 7) == 0)
        name += 7;
    for (index = 0; dirs[index] != NULL; ++index) {
        snprintf(path, sizeof(path), "%s/%s/%s", root, dirs[index], name);
        file = fopen(path, "rb");
        if (file != NULL) {
            if (opened != NULL)
                snprintf(opened, opened_size, "%s", path);
            return file;
        }
    }
    prefix = strstr(name, "bin/Data/");
    if (prefix != NULL) {
        snprintf(path, sizeof(path), "%s/assets/%s", root, prefix);
        file = fopen(path, "rb");
        if (file != NULL) {
            if (opened != NULL)
                snprintf(opened, opened_size, "%s", path);
            return file;
        }
    }
    {
        const char *obb = getenv("SG_OBB");
        const char *base = strrchr(name, '/');
        char zippath[PATH_MAX_ASSET];

        base = base != NULL ? base + 1 : name;
        if (obb != NULL && obb[0] != '\0' && base[0] != '\0') {
            snprintf(zippath, sizeof(zippath), "%s/assets/bin/Data/%s", obb,
                     base);
            file = zip_obb_fopen(zippath);
            if (file != NULL) {
                if (opened != NULL)
                    snprintf(opened, opened_size, "%s", zippath);
                return file;
            }
        }
    }
    return NULL;
}

static void *asset_from_java(void *env, void *manager)
{
    (void)env;
    (void)manager;
    printf("SG-ASSET fromJava\n");
    return &asset_manager_token;
}

static void *asset_open(void *manager, const char *filename, int mode)
{
    struct sg_asset *asset;
    char opened[PATH_MAX_ASSET];
    FILE *file;
    (void)manager;
    (void)mode;
    file = open_asset_file(filename, opened, sizeof(opened));
    printf("SG-ASSET open %s -> %s %s\n", filename != NULL ? filename : "(null)",
           opened[0] != '\0' ? opened : "(none)", file != NULL ? "ok" : "FAIL");
    if (file == NULL)
        return NULL;
    asset = calloc(1U, sizeof(*asset));
    if (asset == NULL) {
        fclose(file);
        return NULL;
    }
    asset->magic = ASSET_MAGIC;
    asset->file = file;
    if (fseek(file, 0, SEEK_END) == 0) {
        asset->length = ftell(file);
        if (asset->length < 0)
            asset->length = 0;
        rewind(file);
    }
    return asset;
}

static struct sg_asset *as_asset(void *value)
{
    struct sg_asset *asset = (struct sg_asset *)value;
    if (asset == NULL || asset->magic != ASSET_MAGIC)
        return NULL;
    return asset;
}

static void asset_close(void *value)
{
    struct sg_asset *asset = as_asset(value);
    if (asset == NULL)
        return;
    if (asset->file != NULL)
        fclose(asset->file);
    free(asset->buffer);
    free(asset);
}

static int asset_read(void *value, void *buffer, size_t count)
{
    struct sg_asset *asset = as_asset(value);
    size_t got;
    if (asset == NULL || buffer == NULL)
        return -1;
    got = fread(buffer, 1U, count, asset->file);
    asset->position += (long)got;
    return (int)got;
}

static long asset_get_length(void *value)
{
    struct sg_asset *asset = as_asset(value);
    return asset != NULL ? asset->length : 0;
}

static int64_t asset_get_length64(void *value)
{
    return (int64_t)asset_get_length(value);
}

static long asset_seek(void *value, long offset, int whence)
{
    struct sg_asset *asset = as_asset(value);
    if (asset == NULL || fseek(asset->file, offset, whence) != 0)
        return -1;
    asset->position = ftell(asset->file);
    return asset->position;
}

static int64_t asset_seek64(void *value, int64_t offset, int whence)
{
    return (int64_t)asset_seek(value, (long)offset, whence);
}

static const void *asset_get_buffer(void *value)
{
    struct sg_asset *asset = as_asset(value);
    if (asset == NULL)
        return NULL;
    if (asset->buffer != NULL)
        return asset->buffer;
    if (asset->length <= 0)
        return NULL;
    asset->buffer = malloc((size_t)asset->length);
    if (asset->buffer == NULL)
        return NULL;
    rewind(asset->file);
    if (fread(asset->buffer, 1U, (size_t)asset->length, asset->file) !=
        (size_t)asset->length) {
        free(asset->buffer);
        asset->buffer = NULL;
        return NULL;
    }
    rewind(asset->file);
    asset->position = 0;
    return asset->buffer;
}

static int asset_remaining(void *value)
{
    struct sg_asset *asset = as_asset(value);
    if (asset == NULL)
        return 0;
    if (asset->length < asset->position)
        return 0;
    return (int)(asset->length - asset->position);
}

void sg_android_init(int width, int height)
{
    window.magic = (int32_t)ANDROID_MAGIC;
    window.refs = 1;
    window.width = width > 0 ? width : nfsmw_screen_width();
    window.height = height > 0 ? height : nfsmw_screen_height();
}

void *sg_android_window(void)
{
    if (window.magic != (int32_t)ANDROID_MAGIC)
        sg_android_init(0, 0);
    return &window;
}

static struct sg_window *as_window(void *value)
{
    struct sg_window *candidate = (struct sg_window *)value;
    if (candidate == NULL || candidate->magic != (int32_t)ANDROID_MAGIC)
        return &window;
    return candidate;
}

void *sg_android_dlopen(const char *name)
{
    if (name == NULL)
        return NULL;
    if (strstr(name, "libandroid") != NULL)
        return &android_lib_token;
    return NULL;
}

int sg_android_is_handle(void *handle)
{
    return handle == &android_lib_token || handle == &sensor_token;
}

static int32_t window_get_width(void *handle)
{
    (void)handle;
    if (window.width < 160)
        window.width = nfsmw_screen_width();
    return nfsmw_screen_width();
}

static int32_t window_get_height(void *handle)
{
    (void)handle;
    if (window.height < 120)
        window.height = nfsmw_screen_height();
    return nfsmw_screen_height();
}

static void *window_acquire(void *handle)
{
    struct sg_window *native = as_window(handle);
    native->refs += 1;
    return native;
}

static void window_release(void *handle)
{
    struct sg_window *native = as_window(handle);
    if (native->refs > 0)
        native->refs -= 1;
}

static void *window_from_surface(void *env, void *surface)
{
    (void)env;
    (void)surface;
    return sg_android_window();
}

static int32_t window_set_buffers_geometry(void *handle, int32_t width,
                                           int32_t height, int32_t format)
{
    struct sg_window *native = as_window(handle);
    /* Unity falls back to a 128x128 buffer when this is handed a tiny
       size. Keep the real panel size. */
    (void)handle;
    (void)width;
    (void)height;
    (void)format;
    native->width = nfsmw_screen_width();
    native->height = nfsmw_screen_height();
    printf("SG-WIN setBuffersGeometry kept %dx%d\n", native->width,
           native->height);
    return 0;
}

static int32_t window_lock(void *handle, void *out_buffer, void *in_out_dirty)
{
    (void)handle;
    (void)out_buffer;
    (void)in_out_dirty;
    return -1;
}

static int32_t window_unlock(void *handle)
{
    (void)handle;
    return 0;
}

static void *looper_prepare(int opts)
{
    static unsigned char looper;
    (void)opts;
    return &looper;
}

static void *looper_for_thread(void)
{
    return looper_prepare(0);
}

static int32_t looper_poll_all(int timeout, int *out_fd, int *out_events,
                               void **out_data)
{
    static int logs;

    nfsmw_opensl_pump();
    if (timeout < 0)
        usleep(16000);
    else if (timeout > 0)
        usleep((useconds_t)timeout * 1000U);
    if (out_fd != NULL)
        *out_fd = -1;
    if (out_events != NULL)
        *out_events = 0;
    if (out_data != NULL)
        *out_data = NULL;
    if (logs < 8) {
        printf("SG-ALOOPER pollAll timeout=%d -> TIMEOUT\n", timeout);
        logs += 1;
    }
    return -3;
}

static void looper_wake(void *looper)
{
    (void)looper;
}

static void *config_new(void)
{
    return &config_token;
}

static void config_delete(void *config)
{
    (void)config;
}

static void config_from_asset(void *config, void *manager)
{
    (void)config;
    (void)manager;
}

static int input_queue_attach(void *queue, void *looper, int ident,
                              void *callback, void *data)
{
    (void)queue;
    (void)looper;
    (void)ident;
    (void)callback;
    (void)data;
    (void)input_queue_token;
    return 0;
}

static void input_queue_detach(void *queue, void *looper)
{
    (void)queue;
    (void)looper;
}

static int32_t input_queue_get(void *queue, void **event)
{
    (void)queue;
    if (event != NULL)
        *event = NULL;
    return -1;
}

static void input_queue_finish(void *queue, void *event, int handled)
{
    (void)queue;
    (void)event;
    (void)handled;
}

static int32_t input_queue_predispatch(void *queue, void *event)
{
    (void)queue;
    (void)event;
    return 0;
}

static const char *sensor_name(void *sensor)
{
    (void)sensor;
    return "none";
}

static int32_t sensor_type(void *sensor)
{
    (void)sensor;
    return 0;
}

static float sensor_zero_f(void *sensor)
{
    (void)sensor;
    return 0.0f;
}

static int32_t sensor_min_delay(void *sensor)
{
    (void)sensor;
    return 0;
}

static void *sensor_manager_instance(void)
{
    return &sensor_token;
}

static void *sensor_default(void *manager, int type)
{
    (void)manager;
    (void)type;
    return NULL;
}

static int sensor_list(void *manager, void ***list)
{
    (void)manager;
    if (list != NULL)
        *list = NULL;
    return 0;
}

static void *sensor_create_queue(void *manager, void *looper, int ident,
                                 void *callback, void *data)
{
    (void)manager;
    (void)looper;
    (void)ident;
    (void)callback;
    (void)data;
    return &sensor_token;
}

static int sensor_destroy_queue(void *manager, void *queue)
{
    (void)manager;
    (void)queue;
    return 0;
}

static int sensor_enable(void *queue, void *sensor)
{
    (void)queue;
    (void)sensor;
    return -1;
}

static int sensor_disable(void *queue, void *sensor)
{
    (void)queue;
    (void)sensor;
    return 0;
}

static int sensor_has_events(void *queue)
{
    (void)queue;
    return 0;
}

static int sensor_get_events(void *queue, void *events, size_t count)
{
    (void)queue;
    (void)events;
    (void)count;
    return 0;
}

static int sensor_set_rate(void *queue, void *sensor, int32_t usec)
{
    (void)queue;
    (void)sensor;
    (void)usec;
    return 0;
}

#define RESOLVE(symbol, function)                                       \
    do {                                                                \
        if (strcmp(name, (symbol)) == 0)                                \
            return (uintptr_t)(function);                               \
    } while (0)

uintptr_t sg_android_resolve(const char *name)
{
    if (name == NULL)
        return 0U;
    RESOLVE("ANativeWindow_fromSurface", window_from_surface);
    RESOLVE("ANativeWindow_acquire", window_acquire);
    RESOLVE("ANativeWindow_release", window_release);
    RESOLVE("ANativeWindow_getWidth", window_get_width);
    RESOLVE("ANativeWindow_getHeight", window_get_height);
    RESOLVE("ANativeWindow_setBuffersGeometry", window_set_buffers_geometry);
    RESOLVE("ANativeWindow_lock", window_lock);
    RESOLVE("ANativeWindow_unlockAndPost", window_unlock);
    RESOLVE("ALooper_prepare", looper_prepare);
    RESOLVE("ALooper_forThread", looper_for_thread);
    RESOLVE("ALooper_pollAll", looper_poll_all);
    RESOLVE("ALooper_pollOnce", looper_poll_all);
    RESOLVE("ALooper_wake", looper_wake);
    RESOLVE("AConfiguration_new", config_new);
    RESOLVE("AConfiguration_delete", config_delete);
    RESOLVE("AConfiguration_fromAssetManager", config_from_asset);
    RESOLVE("AAssetManager_fromJava", asset_from_java);
    RESOLVE("AAssetManager_open", asset_open);
    RESOLVE("AAsset_close", asset_close);
    RESOLVE("AAsset_read", asset_read);
    RESOLVE("AAsset_getLength", asset_get_length);
    RESOLVE("AAsset_getLength64", asset_get_length64);
    RESOLVE("AAsset_seek", asset_seek);
    RESOLVE("AAsset_seek64", asset_seek64);
    RESOLVE("AAsset_getBuffer", asset_get_buffer);
    RESOLVE("AAsset_getRemainingLength", asset_remaining);
    RESOLVE("AAsset_getRemainingLength64", asset_get_length64);
    RESOLVE("AInputQueue_attachLooper", input_queue_attach);
    RESOLVE("AInputQueue_detachLooper", input_queue_detach);
    RESOLVE("AInputQueue_getEvent", input_queue_get);
    RESOLVE("AInputQueue_finishEvent", input_queue_finish);
    RESOLVE("AInputQueue_preDispatchEvent", input_queue_predispatch);
    RESOLVE("ASensor_getName", sensor_name);
    RESOLVE("ASensor_getVendor", sensor_name);
    RESOLVE("ASensor_getType", sensor_type);
    RESOLVE("ASensor_getMinDelay", sensor_min_delay);
    RESOLVE("ASensor_getResolution", sensor_zero_f);
    RESOLVE("AMotionEvent_getDownTime", sensor_has_events);
    RESOLVE("AMotionEvent_getEventTime", sensor_has_events);
    RESOLVE("AMotionEvent_getFlags", sensor_has_events);
    RESOLVE("AMotionEvent_getEdgeFlags", sensor_has_events);
    RESOLVE("AMotionEvent_getMetaState", sensor_has_events);
    RESOLVE("AMotionEvent_getAction", sensor_has_events);
    RESOLVE("AMotionEvent_getPointerCount", sensor_has_events);
    RESOLVE("AMotionEvent_getPointerId", sensor_has_events);
    RESOLVE("AMotionEvent_getHistorySize", sensor_has_events);
    RESOLVE("AMotionEvent_getX", sensor_enable);
    RESOLVE("AMotionEvent_getY", sensor_enable);
    RESOLVE("AMotionEvent_getPressure", sensor_enable);
    RESOLVE("AMotionEvent_getSize", sensor_enable);
    RESOLVE("AMotionEvent_getOrientation", sensor_enable);
    RESOLVE("AMotionEvent_getToolMajor", sensor_enable);
    RESOLVE("AMotionEvent_getToolMinor", sensor_enable);
    RESOLVE("AMotionEvent_getTouchMajor", sensor_enable);
    RESOLVE("AMotionEvent_getTouchMinor", sensor_enable);
    RESOLVE("AMotionEvent_getXPrecision", sensor_enable);
    RESOLVE("AMotionEvent_getYPrecision", sensor_enable);
    RESOLVE("AMotionEvent_getHistoricalX", sensor_enable);
    RESOLVE("AMotionEvent_getHistoricalY", sensor_enable);
    RESOLVE("AMotionEvent_getHistoricalPressure", sensor_enable);
    RESOLVE("AMotionEvent_getHistoricalSize", sensor_enable);
    RESOLVE("AMotionEvent_getHistoricalEventTime", sensor_has_events);
    RESOLVE("AInputEvent_getDeviceId", sensor_has_events);
    RESOLVE("AInputEvent_getSource", sensor_has_events);
    RESOLVE("AInputEvent_getType", sensor_has_events);
    RESOLVE("AKeyEvent_getAction", sensor_has_events);
    RESOLVE("AKeyEvent_getKeyCode", sensor_has_events);
    RESOLVE("AKeyEvent_getMetaState", sensor_has_events);
    RESOLVE("AMotionEvent_getAction", sensor_has_events);
    RESOLVE("AMotionEvent_getPointerCount", sensor_has_events);
    RESOLVE("AMotionEvent_getPointerId", sensor_has_events);
    RESOLVE("AMotionEvent_getHistorySize", sensor_has_events);
    RESOLVE("AMotionEvent_getX", sensor_enable);
    RESOLVE("AMotionEvent_getY", sensor_enable);
    RESOLVE("ASensorManager_getInstance", sensor_manager_instance);
    RESOLVE("ASensorManager_getDefaultSensor", sensor_default);
    RESOLVE("ASensorManager_getSensorList", sensor_list);
    RESOLVE("ASensorManager_createEventQueue", sensor_create_queue);
    RESOLVE("ASensorManager_destroyEventQueue", sensor_destroy_queue);
    RESOLVE("ASensorEventQueue_enableSensor", sensor_enable);
    RESOLVE("ASensorEventQueue_disableSensor", sensor_disable);
    RESOLVE("ASensorEventQueue_hasEvents", sensor_has_events);
    RESOLVE("ASensorEventQueue_getEvents", sensor_get_events);
    RESOLVE("ASensorEventQueue_setEventRate", sensor_set_rate);
    return 0U;
}

void *sg_android_dlsym(void *handle, const char *name)
{
    uintptr_t symbol;

    if (!sg_android_is_handle(handle))
        return NULL;
    symbol = sg_android_resolve(name);
    return symbol != 0U ? (void *)symbol : NULL;
}
