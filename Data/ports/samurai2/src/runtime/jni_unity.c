#include "jni_unity.h"
#include "android_native.h"
#include "crash_trace.h"
#include "opensl_bridge.h"
#include "platform_probe.h"
#include "screen_size.h"
#include "build_id.h"
#include "collider_debug.h"
#include "../nfsmw_frame.h"

/*
 * libunity / libil2cpp are Android armeabi-v7a (softfp). This runtime is
 * arm-linux-gnueabihf (hardfp). JNI float returns must use AAPCS so Unity
 * reads the value from r0, not s0. Integer JNI already matches. That is why
 * buttons worked and analog look never did.
 */
#if defined(__arm__)
#define SG_JNI_SOFTFP __attribute__((pcs("aapcs")))
#else
#define SG_JNI_SOFTFP
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    JNI_TABLE_SLOTS = 240,
    JVM_TABLE_SLOTS = 8,
    NATIVE_CAPACITY = 64,
    METHOD_CAPACITY = 4096,
    JNI_OK_VALUE = 0,
    JNI_VERSION_1_6_VALUE = 0x00010006,
    FAKE_MAGIC = 0x53474a4e,
    JNI_GET_VERSION = 4, JNI_FIND_CLASS = 6,
    JNI_FROM_REFLECTED_METHOD = 7, JNI_FROM_REFLECTED_FIELD = 8,
    JNI_TO_REFLECTED_METHOD = 9,
    JNI_GET_SUPERCLASS = 10, JNI_IS_ASSIGNABLE_FROM = 11,
    JNI_TO_REFLECTED_FIELD = 12,
    JNI_EXCEPTION_OCCURRED = 15, JNI_EXCEPTION_DESCRIBE = 16,
    JNI_EXCEPTION_CLEAR = 17, JNI_PUSH_LOCAL_FRAME = 19,
    JNI_POP_LOCAL_FRAME = 20, JNI_NEW_GLOBAL_REF = 21,
    JNI_DELETE_GLOBAL_REF = 22, JNI_DELETE_LOCAL_REF = 23,
    JNI_IS_SAME_OBJECT = 24, JNI_NEW_LOCAL_REF = 25,
    JNI_ENSURE_LOCAL_CAPACITY = 26, JNI_ALLOC_OBJECT = 27,
    JNI_NEW_OBJECT = 28, JNI_NEW_OBJECT_V = 29, JNI_NEW_OBJECT_A = 30,
    JNI_GET_OBJECT_CLASS = 31, JNI_IS_INSTANCE_OF = 32,
    JNI_GET_METHOD_ID = 33, JNI_CALL_OBJECT_METHOD = 34,
    JNI_CALL_OBJECT_METHOD_V = 35, JNI_CALL_OBJECT_METHOD_A = 36,
    JNI_CALL_BOOLEAN_METHOD = 37, JNI_CALL_BOOLEAN_METHOD_V = 38,
    JNI_CALL_BOOLEAN_METHOD_A = 39, JNI_CALL_INT_METHOD = 49,
    JNI_CALL_INT_METHOD_V = 50, JNI_CALL_INT_METHOD_A = 51,
    JNI_CALL_LONG_METHOD = 52, JNI_CALL_LONG_METHOD_V = 53,
    JNI_CALL_LONG_METHOD_A = 54, JNI_CALL_FLOAT_METHOD = 55,
    JNI_CALL_FLOAT_METHOD_V = 56, JNI_CALL_FLOAT_METHOD_A = 57,
    JNI_CALL_DOUBLE_METHOD = 58, JNI_CALL_DOUBLE_METHOD_V = 59,
    JNI_CALL_VOID_METHOD = 61, JNI_CALL_VOID_METHOD_V = 62,
    JNI_CALL_VOID_METHOD_A = 63, JNI_GET_FIELD_ID = 94,
    JNI_GET_OBJECT_FIELD = 95, JNI_GET_BOOLEAN_FIELD = 96,
    JNI_GET_CHAR_FIELD = 98,
    JNI_GET_INT_FIELD = 100, JNI_GET_LONG_FIELD = 101,
    JNI_GET_FLOAT_FIELD = 102, JNI_SET_OBJECT_FIELD = 104,
    JNI_SET_BOOLEAN_FIELD = 105, JNI_SET_INT_FIELD = 109,
    JNI_GET_STATIC_METHOD_ID = 113,
    JNI_CALL_STATIC_OBJECT_METHOD = 114,
    JNI_CALL_STATIC_OBJECT_METHOD_V = 115,
    JNI_CALL_STATIC_OBJECT_METHOD_A = 116,
    JNI_CALL_STATIC_BOOLEAN_METHOD = 117,
    JNI_CALL_STATIC_BOOLEAN_METHOD_V = 118,
    JNI_CALL_STATIC_INT_METHOD = 129,
    JNI_CALL_STATIC_INT_METHOD_V = 130,
    JNI_CALL_STATIC_VOID_METHOD = 141,
    JNI_CALL_STATIC_VOID_METHOD_V = 142,
    JNI_GET_STATIC_FIELD_ID = 144,
    JNI_GET_STATIC_OBJECT_FIELD = 145,
    JNI_GET_STATIC_BOOLEAN_FIELD = 146,
    JNI_GET_STATIC_INT_FIELD = 150,
    JNI_NEW_STRING = 163, JNI_GET_STRING_LENGTH = 164,
    JNI_GET_STRING_CHARS = 165, JNI_RELEASE_STRING_CHARS = 166,
    JNI_NEW_STRING_UTF = 167, JNI_GET_STRING_UTF_LENGTH = 168,
    JNI_GET_STRING_UTF_CHARS = 169, JNI_RELEASE_STRING_UTF_CHARS = 170,
    JNI_GET_ARRAY_LENGTH = 171, JNI_NEW_OBJECT_ARRAY = 172,
    JNI_GET_OBJECT_ARRAY_ELEMENT = 173, JNI_SET_OBJECT_ARRAY_ELEMENT = 174,
    JNI_NEW_BYTE_ARRAY = 176, JNI_NEW_INT_ARRAY = 179,
    JNI_GET_BYTE_ARRAY_ELEMENTS = 184, JNI_GET_INT_ARRAY_ELEMENTS = 187,
    JNI_RELEASE_BYTE_ARRAY_ELEMENTS = 192,
    JNI_RELEASE_INT_ARRAY_ELEMENTS = 195,
    JNI_REGISTER_NATIVES = 215, JNI_UNREGISTER_NATIVES = 216,
    JNI_MONITOR_ENTER = 217, JNI_MONITOR_EXIT = 218,
    JNI_GET_JAVA_VM = 219,
    JNI_GET_STRING_REGION = 220, JNI_GET_STRING_UTF_REGION = 221,
    JNI_EXCEPTION_CHECK = 228,
    JNI_NEW_DIRECT_BYTE_BUFFER = 229,
    JNI_GET_DIRECT_BUFFER_ADDRESS = 230,
    JNI_GET_DIRECT_BUFFER_CAPACITY = 231,
    JVM_GET_ENV = 6,
    PATH_CAPACITY = 4096,
    /* Android joystick input (SOURCE_JOYSTICK, MotionEvent.getAxisValue,
       InputDevice.getMotionRange) only exists from API 12, and Unity gates its
       axis code on SDK_INT: on API 8 it reads getSource(), sees a joystick that
       cannot exist there and recycles the event without touching the axes.
       Report Jelly Bean so the axis path runs; runtime permissions start at 23
       and stay off. The app keeps targeting API 8 for the OBB gating. */
    ANDROID_SDK_INT = 16,
    ANDROID_TARGET_SDK = 8,
    OBJECT_HASH_SLOTS = 4096
};

enum fake_kind {
    FAKE_GENERIC, FAKE_CLASS, FAKE_STRING, FAKE_FILE, FAKE_ACTIVITY,
    FAKE_SURFACE, FAKE_ARRAY, FAKE_BITMAP, FAKE_DIRECT
};

struct fake_jni_handle { uintptr_t *functions; };

struct fake_object {
    uint32_t magic;
    enum fake_kind kind;
    char *name;
    char *text;
    uint16_t *utf16;
    size_t utf16_length;
    void **elements;
    size_t length;
    int32_t ints[8];
    struct fake_object *hash_next;
};

struct native_method {
    char class_name[192];
    char name[96];
    char signature[96];
    void *fn;
};

struct method_id {
    char name[96];
    char signature[96];
};

static uintptr_t jni_table[JNI_TABLE_SLOTS];
static uintptr_t jvm_table[JVM_TABLE_SLOTS];
static struct fake_jni_handle jni_handle;
static struct fake_jni_handle jvm_handle;
static struct native_method natives[NATIVE_CAPACITY];
static size_t native_count;
static struct method_id methods[METHOD_CAPACITY];
static size_t method_count;
static struct fake_object *activity;
static struct fake_object *unity_player;
static struct fake_object *app_info;
static struct fake_object *surface;
static struct fake_object *object_hash[OBJECT_HASH_SLOTS];
static int tables_ready;
static int request_exit;
static uint32_t *pad_shm;
static struct fake_object *pad_device;
static struct fake_object *key_event;
static struct fake_object *motion_event;
static struct fake_object *touch_event;
static uint32_t prev_pad_buttons;
static int look_touch_down;
static float look_touch_x = 480.0f;
static float look_touch_y = 240.0f;
static float pad_axis[6];
static int stick_center_bias;
static volatile int fmod_running;

enum {
    AKEYCODE_BACK = 4,
    AKEYCODE_ESCAPE = 111,
    AKEYCODE_DPAD_UP = 19,
    AKEYCODE_DPAD_DOWN = 20,
    AKEYCODE_DPAD_LEFT = 21,
    AKEYCODE_DPAD_RIGHT = 22,
    AKEYCODE_BUTTON_A = 96,
    AKEYCODE_BUTTON_B = 97,
    AKEYCODE_BUTTON_X = 99,
    AKEYCODE_BUTTON_Y = 100,
    AKEYCODE_BUTTON_L1 = 102,
    AKEYCODE_BUTTON_R1 = 103,
    AKEYCODE_BUTTON_L2 = 104,
    AKEYCODE_BUTTON_R2 = 105,
    AKEYCODE_BUTTON_THUMBL = 106,
    AKEYCODE_BUTTON_THUMBR = 107,
    AKEYCODE_BUTTON_START = 108,
    AKEYCODE_BUTTON_SELECT = 109,
    /* Virtual keycodes for analog stick directions */
    AKEYCODE_BUTTON_THUMBL_UP = 222,
    AKEYCODE_BUTTON_THUMBL_DOWN = 223,
    AKEYCODE_BUTTON_THUMBL_LEFT = 224,
    AKEYCODE_BUTTON_THUMBL_RIGHT = 225,
    AKEYCODE_BUTTON_THUMBR_UP = 226,
    AKEYCODE_BUTTON_THUMBR_DOWN = 227,
    AKEYCODE_BUTTON_THUMBR_LEFT = 228,
    AKEYCODE_BUTTON_THUMBR_RIGHT = 229,
    ASOURCE_DPAD = 0x00000201,
    ASOURCE_GAMEPAD = 0x00000401,
    ASOURCE_JOYSTICK = 0x01000010,
    ASOURCE_TOUCHSCREEN = 0x00001002,
    ASOURCE_XPERIA_PAD = 0x01000611,
    AMOTION_AXIS_X = 0,
    AMOTION_AXIS_Y = 1,
    AMOTION_AXIS_Z = 11,
    AMOTION_AXIS_RX = 12,
    AMOTION_AXIS_RY = 13,
    AMOTION_AXIS_RZ = 14,
    AMOTION_AXIS_HAT_X = 15,
    AMOTION_AXIS_HAT_Y = 16,
    AMOTION_AXIS_LTRIGGER = 17,
    AMOTION_AXIS_RTRIGGER = 18
};

/* Indexed by SDL button number. R1 reports L2's keycode so that reload, which
   the profile keeps on the L2 button, sits on the shoulder instead. */
static int32_t xperia_keycodes[15] = {
    AKEYCODE_BUTTON_B, AKEYCODE_BUTTON_A, AKEYCODE_BUTTON_X, AKEYCODE_BUTTON_Y,
    AKEYCODE_ESCAPE, 0, AKEYCODE_BUTTON_START, AKEYCODE_BUTTON_THUMBL,
    AKEYCODE_BUTTON_THUMBR, AKEYCODE_BUTTON_L1, AKEYCODE_BUTTON_L2,
    AKEYCODE_DPAD_UP, AKEYCODE_DPAD_DOWN, AKEYCODE_DPAD_LEFT,
    AKEYCODE_DPAD_RIGHT
};

#define PAD_SLOTS 8U

enum pad_source {
    PAD_SRC_NONE = 0,
    PAD_SRC_LX,
    PAD_SRC_LY,
    PAD_SRC_RX,
    PAD_SRC_RY,
    PAD_SRC_LT,
    PAD_SRC_RT,
    PAD_SRC_COUNT,
    /* "btn7" and friends: a held button reads as a full deflection, which is
       how an action bound to half an axis can be put on a shoulder button. */
    PAD_SRC_BUTTON = PAD_SRC_COUNT
};

/* The game shows its binds as "Axis 1".."Axis 8", and Unity fills those slots
   from the advertised axes in its own table order, so slot N is this axis. */
static const int32_t slot_axis[PAD_SLOTS] = {
    AMOTION_AXIS_X, AMOTION_AXIS_Y, AMOTION_AXIS_RX, AMOTION_AXIS_RY,
    AMOTION_AXIS_LTRIGGER, AMOTION_AXIS_RTRIGGER, AMOTION_AXIS_Z,
    AMOTION_AXIS_RZ
};

/* Which control feeds each slot decides which action a stick or trigger fires,
   and the profile's action order is not readable anywhere, so this is settable
   from controls.txt instead of being guessed in the binary. The defaults
   reproduce BUILD 71, the last layout confirmed good, except that slot 5 is
   inverted to reach "Axis 5 -" - the one bind nothing could ever press. */
static int8_t slot_source[PAD_SLOTS] = {
    PAD_SRC_LX, PAD_SRC_LY, PAD_SRC_RX, PAD_SRC_RY,
    PAD_SRC_BUTTON + 10, PAD_SRC_RT, PAD_SRC_RX, PAD_SRC_RY
};
static int8_t slot_negate[PAD_SLOTS] = { 0, 0, 0, 0, 0, 0, 0, 0 };

/* Optional second source driving a slot the other way, so both halves of one
   axis can be separate controls the way the game binds them. */
static int8_t slot_source_neg[PAD_SLOTS] = {
    PAD_SRC_NONE, PAD_SRC_NONE, PAD_SRC_NONE, PAD_SRC_NONE,
    PAD_SRC_LT, PAD_SRC_NONE, PAD_SRC_NONE, PAD_SRC_NONE
};

static const struct {
    const char *name;
    enum pad_source source;
} pad_source_names[] = {
    { "none", PAD_SRC_NONE }, { "lx", PAD_SRC_LX }, { "ly", PAD_SRC_LY },
    { "rx", PAD_SRC_RX }, { "ry", PAD_SRC_RY }, { "lt", PAD_SRC_LT },
    { "rt", PAD_SRC_RT }
};

static const struct {
    const char *name;
    int32_t keycode;
} pad_key_names[] = {
    { "none", 0 }, { "a", AKEYCODE_BUTTON_A }, { "b", AKEYCODE_BUTTON_B },
    { "x", AKEYCODE_BUTTON_X }, { "y", AKEYCODE_BUTTON_Y },
    { "l1", AKEYCODE_BUTTON_L1 }, { "r1", AKEYCODE_BUTTON_R1 },
    { "l2", AKEYCODE_BUTTON_L2 }, { "r2", AKEYCODE_BUTTON_R2 },
    { "start", AKEYCODE_BUTTON_START }, { "select", AKEYCODE_BUTTON_SELECT },
    { "escape", AKEYCODE_ESCAPE }, { "back", AKEYCODE_BACK },
    { "thumbl", AKEYCODE_BUTTON_THUMBL }, { "thumbr", AKEYCODE_BUTTON_THUMBR },
    { "up", AKEYCODE_DPAD_UP }, { "down", AKEYCODE_DPAD_DOWN },
    { "left", AKEYCODE_DPAD_LEFT }, { "right", AKEYCODE_DPAD_RIGHT }
};

/* The triggers are axes, not SDL buttons, so their optional keycodes are kept
   separately. L2 stays silent by default because R1 now carries reload. */
static int32_t trigger_l2_key = AKEYCODE_BUTTON_THUMBR;
static int32_t trigger_r2_key = AKEYCODE_BUTTON_R2;

static float pad_source_value(int8_t id, const float *axis_source)
{
    if (id >= (int8_t)PAD_SRC_BUTTON) {
        unsigned bit = (unsigned)(id - (int8_t)PAD_SRC_BUTTON);
        return (prev_pad_buttons & (1u << bit)) != 0 ? 1.0f : 0.0f;
    }
    return axis_source[id];
}

static void pad_source_name(int8_t id, char *out, size_t out_size)
{
    if (id >= (int8_t)PAD_SRC_BUTTON)
        snprintf(out, out_size, "btn%d", (int)(id - (int8_t)PAD_SRC_BUTTON));
    else
        snprintf(out, out_size, "%s", pad_source_names[id].name);
}

static void controls_apply(char *key, char *value)
{
    size_t index;

    if (strcmp(key, "l2key") == 0 || strcmp(key, "r2key") == 0) {
        for (index = 0U; index < sizeof(pad_key_names) /
                                 sizeof(pad_key_names[0]); ++index) {
            if (strcmp(value, pad_key_names[index].name) != 0)
                continue;
            if (key[0] == 'l')
                trigger_l2_key = pad_key_names[index].keycode;
            else
                trigger_r2_key = pad_key_names[index].keycode;
            printf("SG-CTL %s = %s\n", key, value);
            return;
        }
        printf("SG-CTL unknown key '%s'\n", value);
        return;
    }
    if (strncmp(key, "slot", 4) == 0) {
        int slot = atoi(key + 4);
        int negate = 0;
        int is_neg_side = strstr(key, "neg") != NULL;
        int8_t source = -1;
        if (slot < 1 || slot > (int)PAD_SLOTS)
            return;
        if (*value == '-' || *value == '+') {
            negate = (*value == '-');
            value += 1;
        }
        if (strncmp(value, "btn", 3) == 0) {
            int button = atoi(value + 3);
            if (button >= 0 && button <= 14)
                source = (int8_t)(PAD_SRC_BUTTON + button);
        } else {
            for (index = 0U; index < sizeof(pad_source_names) /
                                     sizeof(pad_source_names[0]); ++index) {
                if (strcmp(value, pad_source_names[index].name) != 0)
                    continue;
                source = (int8_t)pad_source_names[index].source;
                break;
            }
        }
        if (source < 0) {
            printf("SG-CTL unknown source '%s'\n", value);
            return;
        }
        if (is_neg_side) {
            slot_source_neg[slot - 1] = source;
        } else {
            slot_source[slot - 1] = source;
            slot_negate[slot - 1] = (int8_t)negate;
        }
        printf("SG-CTL slot%d%s = %s%s\n", slot, is_neg_side ? "neg" : "",
               negate ? "-" : "", value);
        return;
    }
    if (strncmp(key, "button", 6) == 0) {
        int button = atoi(key + 6);
        if (button < 0 || button > 14)
            return;
        for (index = 0U; index < sizeof(pad_key_names) /
                                 sizeof(pad_key_names[0]); ++index) {
            if (strcmp(value, pad_key_names[index].name) != 0)
                continue;
            xperia_keycodes[button] = pad_key_names[index].keycode;
            printf("SG-CTL button%d = %s\n", button, value);
            return;
        }
        printf("SG-CTL unknown key '%s'\n", value);
        return;
    }
    printf("SG-CTL unknown entry '%s'\n", key);
}

static const char *root_path(void);
static void join_path(char *out, size_t out_size, const char *base,
                      const char *leaf);

/* controls.txt: "slot4 = -ry" points the game's "Axis 4" bind at the right
   stick's vertical travel inverted, "button10 = r1" gives SDL button 10 the R1
   keycode. Lines may use spaces or tabs and # starts a comment. */
static void controls_init(void)
{
    static int loaded;
    char path[PATH_CAPACITY];
    char line[256];
    FILE *file;
    size_t slot;

    if (loaded)
        return;
    loaded = 1;
    join_path(path, sizeof(path), root_path(), "controls.txt");
    file = fopen(path, "r");
    if (file != NULL) {
        while (fgets(line, sizeof(line), file) != NULL) {
            char *key = line;
            char *value = strchr(line, '=');
            char *comment = strchr(line, '#');
            char *end;
            if (comment != NULL)
                *comment = '\0';
            if (value == NULL)
                continue;
            *value++ = '\0';
            while (*key == ' ' || *key == '\t')
                ++key;
            for (end = key + strlen(key); end > key &&
                 (end[-1] == ' ' || end[-1] == '\t'); --end)
                end[-1] = '\0';
            while (*value == ' ' || *value == '\t')
                ++value;
            for (end = value + strlen(value); end > value &&
                 (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                  end[-1] == '\r'); --end)
                end[-1] = '\0';
            if (*key != '\0')
                controls_apply(key, value);
        }
        fclose(file);
        printf("SG-CTL loaded %s\n", path);
    }
    for (slot = 0U; slot < PAD_SLOTS; ++slot) {
        char positive[16];
        char negative[16];
        pad_source_name(slot_source[slot], positive, sizeof(positive));
        pad_source_name(slot_source_neg[slot], negative, sizeof(negative));
        printf("SG-CTL map slot%u = %s%s minus %s\n", (unsigned)(slot + 1U),
               slot_negate[slot] != 0 ? "-" : "", positive, negative);
    }
}

#define FUNCTION_VALUE(fn) ((uintptr_t)(fn))

static const char *env_or(const char *key, const char *fallback)
{
    const char *value = getenv(key);
    return (value != NULL && value[0] != '\0') ? value : fallback;
}

static const char *package_name(void)
{
    return env_or("SG_PACKAGE", "com.madfingergames.SamuraiIIAll");
}

static const char *apk_path(void)
{
    return env_or("SG_APK", "gamedata/samurai2-1.1.4.apk");
}

static const char *obb_path(void)
{
    return env_or("SG_OBB",
                  "Android/obb/com.madfingergames.shadowgun/"
                  "main.170300014.com.madfingergames.shadowgun.obb");
}

static const char *root_path(void)
{
    return env_or("SG_ROOT", ".");
}

static char *dup_string(const char *text)
{
    size_t length = text != NULL ? strlen(text) : 0U;
    char *copy = calloc(1U, length + 1U);
    if (copy == NULL)
        abort();
    if (text != NULL)
        memcpy(copy, text, length);
    return copy;
}

static void join_path(char *out, size_t out_size, const char *base,
                      const char *leaf)
{
    if (leaf != NULL && leaf[0] == '/') {
        snprintf(out, out_size, "%s", leaf);
        return;
    }
    snprintf(out, out_size, "%s/%s", base != NULL ? base : ".",
             leaf != NULL ? leaf : "");
}

static struct fake_object *as_object(void *value)
{
    struct fake_object *object = (struct fake_object *)value;
    struct fake_object *found;
    unsigned slot;
    if (object == NULL || (uintptr_t)object < 0x10000U)
        return NULL;
    slot = ((unsigned)((uintptr_t)object >> 4)) & (OBJECT_HASH_SLOTS - 1);
    for (found = object_hash[slot]; found != NULL; found = found->hash_next) {
        if (found == object)
            return found->magic == FAKE_MAGIC ? found : NULL;
    }
    return NULL;
}

static int ctor_takes_string(void *method)
{
    struct method_id *id = (struct method_id *)method;
    return id != NULL && strstr(id->signature, "Ljava/lang/String;") != NULL;
}

static struct fake_object *new_object(enum fake_kind kind, const char *name)
{
    struct fake_object *object = calloc(1U, sizeof(*object));
    unsigned slot;
    if (object == NULL)
        abort();
    object->magic = FAKE_MAGIC;
    object->kind = kind;
    object->name = dup_string(name != NULL ? name : "java/lang/Object");
    slot = ((unsigned)((uintptr_t)object >> 4)) & (OBJECT_HASH_SLOTS - 1);
    object->hash_next = object_hash[slot];
    object_hash[slot] = object;
    return object;
}

static struct fake_object *new_string(const char *text)
{
    struct fake_object *object = new_object(FAKE_STRING, "java/lang/String");
    object->text = dup_string(text != NULL ? text : "");
    return object;
}

static struct fake_object *new_file_abs(const char *path)
{
    struct fake_object *object = new_object(FAKE_FILE, "java/io/File");
    object->text = dup_string(path != NULL ? path : root_path());
    return object;
}

static struct fake_object *new_file(const char *path)
{
    char full[PATH_CAPACITY];
    join_path(full, sizeof(full), root_path(), path);
    return new_file_abs(full);
}

/* Shadowgun keeps a table of controllers it already knows how to map and falls
   back to the "configure your new gamepad" screen for anything else, so the
   name has to match one of its entries byte for byte. */
#define PAD_DEVICE_NAME "Microsoft X-Box 360 pad"

static struct fake_object *input_device_object(int32_t device_id)
{
    if (pad_device == NULL) {
        pad_device = new_object(FAKE_GENERIC, "android/view/InputDevice");
        pad_device->text = dup_string(PAD_DEVICE_NAME);
    }
    pad_device->ints[0] = device_id;
    return pad_device;
}

static int object_is_named(const struct fake_object *object, const char *part)
{
    return object != NULL && object->name != NULL &&
           strstr(object->name, part) != NULL;
}

enum { MOTION_RANGE_MAGIC = 0x4d524e47 };

static struct fake_object *motion_range_object(int32_t axis)
{
    struct fake_object *range =
        new_object(FAKE_GENERIC, "android/view/InputDevice$MotionRange");
    range->ints[0] = axis;
    return range;
}

/* Unity ignores this order and assigns its joystick slots 1..n by its own axis
   table, which the log shows as X, Y, LTRIGGER, RTRIGGER, Z, RZ, RX, RY. Only
   advertised axes get a slot, so the six-axis list left the profile's view
   binds on slots 7/8 pointing at nothing; RX/RY fill those and carry the right
   stick as well. The hat stays absent - the D-pad already arrives as key events
   and Unity would synthesize a second set from a hat axis. */
static const int32_t pad_motion_axes[] = {
    AMOTION_AXIS_X, AMOTION_AXIS_Y, AMOTION_AXIS_Z, AMOTION_AXIS_RZ,
    AMOTION_AXIS_LTRIGGER, AMOTION_AXIS_RTRIGGER,
    AMOTION_AXIS_RX, AMOTION_AXIS_RY
};

/* InputDevice.getMotionRanges(): Unity walks this list to learn which axes the
   joystick has, and a device with no axes gets its motion events dropped. */
static struct fake_object *motion_range_list(void)
{
    const size_t count = sizeof(pad_motion_axes) / sizeof(pad_motion_axes[0]);
    struct fake_object *list = new_object(FAKE_ARRAY, "java/util/ArrayList");
    size_t index;

    list->elements = calloc(count, sizeof(*list->elements));
    if (list->elements == NULL)
        abort();
    for (index = 0U; index < count; ++index)
        list->elements[index] = motion_range_object(pad_motion_axes[index]);
    list->length = count;
    list->ints[1] = MOTION_RANGE_MAGIC;
    printf("SG-JNI getMotionRanges -> %u axes\n", (unsigned)count);
    return list;
}

enum { FMOD_DEVICE_MAGIC = 0x464d4f44 };

static int is_fmod_device(const struct fake_object *object)
{
    if (object == NULL)
        return 0;
    if (object->ints[1] == FMOD_DEVICE_MAGIC)
        return 1;
    return object_is_named(object, "FMODAudioDevice");
}

static void fmod_audio_device_start(struct fake_object *device);
static void fmod_audio_device_stop(void);

static void fill_obb_path(char *out, size_t out_size)
{
    join_path(out, out_size, root_path(), obb_path());
}

static void fill_expansion_path(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/Android/obb/%s", root_path(), package_name());
}

static const char *file_path_text(const struct fake_object *object)
{
    if (object != NULL && object->text != NULL && object->text[0] != '\0')
        return object->text;
    return root_path();
}

static void *file_parent_string(const struct fake_object *object)
{
    const char *path_text = file_path_text(object);
    const char *slash = strrchr(path_text, '/');
    char parent[PATH_CAPACITY];
    size_t length;

    if (slash == NULL)
        return new_string("");
    if (slash == path_text)
        return new_string("/");
    length = (size_t)(slash - path_text);
    if (length >= sizeof(parent))
        length = sizeof(parent) - 1U;
    memcpy(parent, path_text, length);
    parent[length] = '\0';
    return new_string(parent);
}

static const char *method_name(void *method)
{
    struct method_id *id = (struct method_id *)method;
    if (id == NULL)
        return "";
    return id->name;
}

static const char *method_signature(void *method)
{
    struct method_id *id = (struct method_id *)method;
    if (id == NULL || id->signature[0] == '\0')
        return "";
    return id->signature;
}

static int is_www_ctor(void *method)
{
    return strcmp(method_signature(method),
                  "(ILjava/lang/String;[BLjava/util/Map;)V") == 0;
}

static void *intern_method(const char *name, const char *signature)
{
    size_t index;
    if (name == NULL)
        name = "";
    if (signature == NULL)
        signature = "";
    for (index = 0U; index < method_count; ++index) {
        if (strcmp(methods[index].name, name) == 0 &&
            strcmp(methods[index].signature, signature) == 0)
            return &methods[index];
    }
    if (method_count >= METHOD_CAPACITY) {
        printf("SG-JNI intern FULL %zu %s %s\n", method_count, name, signature);
        return &methods[0];
    }
    snprintf(methods[method_count].name, sizeof(methods[method_count].name),
             "%s", name);
    snprintf(methods[method_count].signature,
             sizeof(methods[method_count].signature), "%s", signature);
    method_count += 1U;
    if (method_count == 64U || method_count == 256U || method_count == 1024U ||
        method_count == 2048U)
        printf("SG-JNI interned %zu\n", method_count);
    return &methods[method_count - 1U];
}

static void slash_to_dot(char *text)
{
    if (text == NULL)
        return;
    while (*text != '\0') {
        if (*text == '/')
            *text = '.';
        text += 1;
    }
}

static void dot_to_slash(char *text)
{
    if (text == NULL)
        return;
    while (*text != '\0') {
        if (*text == '.')
            *text = '/';
        text += 1;
    }
}

static void *class_for_name(void *name_value)
{
    struct fake_object *str = as_object(name_value);
    char slash[192];
    const char *dotted = str != NULL && str->text != NULL ?
                         str->text : "java.lang.Object";
    printf("SG-JNI forName '%s'\n", dotted);
    snprintf(slash, sizeof(slash), "%s", dotted);
    dot_to_slash(slash);
    return new_object(FAKE_CLASS, slash);
}

static void *dotted_class_string(const struct fake_object *object)
{
    char copy[192];
    const char *raw = object != NULL && object->name != NULL ?
                      object->name : "java.lang.Object";
    snprintf(copy, sizeof(copy), "%s", raw);
    slash_to_dot(copy);
    return new_string(copy);
}

static void *simple_class_string(const struct fake_object *object)
{
    char copy[192];
    const char *raw = object != NULL && object->name != NULL ?
                      object->name : "Object";
    const char *slash;
    snprintf(copy, sizeof(copy), "%s", raw);
    slash_to_dot(copy);
    slash = strrchr(copy, '.');
    return new_string(slash != NULL && slash[1] != '\0' ? slash + 1 : copy);
}

static uint16_t *ensure_utf16(struct fake_object *object)
{
    const char *text;
    size_t length;
    size_t index;
    static uint16_t empty[1];

    if (object == NULL)
        return empty;
    if (object->utf16 != NULL)
        return object->utf16;
    text = object->text != NULL ? object->text : "";
    length = strlen(text);
    object->utf16 = calloc(length + 1U, sizeof(*object->utf16));
    if (object->utf16 == NULL)
        abort();
    for (index = 0U; index < length; ++index)
        object->utf16[index] = (uint8_t)text[index];
    object->utf16_length = length;
    return object->utf16;
}

static struct native_method *find_native(const char *class_name,
                                         const char *name)
{
    size_t index;
    for (index = 0U; index < native_count; ++index) {
        if (strcmp(natives[index].name, name) == 0 &&
            (class_name == NULL || class_name[0] == '\0' ||
             strstr(natives[index].class_name, class_name) != NULL))
            return &natives[index];
    }
    for (index = 0U; index < native_count; ++index) {
        if (strcmp(natives[index].name, name) == 0)
            return &natives[index];
    }
    return NULL;
}

static struct native_method *find_native_sig(const char *name, const char *sig)
{
    size_t index;
    for (index = 0U; index < native_count; ++index) {
        if (strcmp(natives[index].name, name) == 0 &&
            (sig == NULL || sig[0] == '\0' ||
             strcmp(natives[index].signature, sig) == 0))
            return &natives[index];
    }
    return find_native(NULL, name);
}

static void apply_www_ctor(struct fake_object *created, int32_t handle,
                           void *url_value)
{
    struct fake_object *url = as_object(url_value);
    if (created == NULL)
        return;
    created->ints[0] = handle;
    created->ints[1] = 1;
    created->ints[2] = 0;
    if (created->text != NULL)
        free(created->text);
    created->text = dup_string(url != NULL && url->text != NULL ? url->text : "");
    printf("SG-JNI WWW ctor handle=%d url='%s'\n", handle,
           created->text != NULL ? created->text : "");
}

/* Unity C++ env->NewObject uses the JNI table NewObjectV slot. On ARM the
   va_list is packed 4-byte slots: jint handle, jobject url, jbyteArray, Map.
   NewObjectA is 8-byte jvalue. Scan both for a String. */
static void apply_www_ctor_args(struct fake_object *created, void *args)
{
    void **slots = args;
    struct fake_object *url = NULL;
    int32_t handle;
    int i;

    if (created == NULL || args == NULL) {
        apply_www_ctor(created, 0, NULL);
        return;
    }
    handle = *(int32_t *)args;
    printf("SG-JNI WWW args %p %p %p %p\n", slots[0], slots[1], slots[2],
           slots[3]);
    for (i = 0; i < 6; i++) {
        struct fake_object *object = as_object(slots[i]);
        if (object == NULL || object->text == NULL || object->text[0] == '\0')
            continue;
        printf("SG-JNI WWW arg[%d] kind=%d '%s'\n", i, (int)object->kind,
               object->text);
        if (url == NULL || object->kind == FAKE_STRING)
            url = object;
    }
    apply_www_ctor(created, handle, url);
}

static const char *www_file_path(const char *url)
{
    if (url == NULL)
        return "";
    if (strncmp(url, "file://", 7) == 0) {
        const char *path = url + 7;
        return path[0] != '\0' ? path : "/";
    }
    if (strncmp(url, "file:", 5) == 0)
        return url + 5;
    return url;
}

static void complete_www(struct fake_object *www)
{
    struct native_method *header_native;
    struct native_method *read_native;
    struct native_method *done_native;
    struct native_method *error_native;
    const char *url;
    const char *path;
    char header[256];
    FILE *file;
    long size;
    int32_t handle;

    if (www == NULL || www->ints[1] == 0)
        return;
    handle = www->ints[0];
    url = www->text != NULL ? www->text : "";
    path = www_file_path(url);
    www->ints[2] = 1;

    header_native = find_native_sig("headerCallback", "(ILjava/lang/String;)Z");
    read_native = find_native_sig("readCallback", "(I[BI)Z");
    done_native = find_native_sig("doneCallback", "(I)V");
    error_native = find_native_sig("errorCallback", "(ILjava/lang/String;)V");

    printf("SG-JNI WWW start handle=%d url='%s' path='%s'\n", handle, url, path);

    if (strncmp(url, "file:", 5) != 0 && url[0] != '/') {
        const char *body = "{}";
        size_t body_len = 2U;
        char hdr[256];
        typedef int32_t (*header_fn)(void *, void *, int32_t, void *);
        typedef int32_t (*read_fn)(void *, void *, int32_t, void *, int32_t);
        typedef void (*done_fn)(void *, void *, int32_t);
        struct fake_object *bytes;

        /* Offline: never open a socket. Unity Analytics / config URLs are dead. */
        printf("SG-JNI WWW local stub (no network)\n");
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.0 200 OK\nContent-Type: application/json\n"
                 "Content-Length: %zu\n\n",
                 body_len);
        if (header_native != NULL && header_native->fn != NULL)
            (void)((header_fn)header_native->fn)(&jni_handle, www, handle,
                                                 new_string(hdr));
        bytes = new_object(FAKE_ARRAY, "[B");
        bytes->length = body_len;
        bytes->text = dup_string(body);
        if (read_native != NULL && read_native->fn != NULL)
            (void)((read_fn)read_native->fn)(&jni_handle, www, handle, bytes,
                                             (int32_t)body_len);
        if (done_native != NULL && done_native->fn != NULL)
            ((done_fn)done_native->fn)(&jni_handle, www, handle);
        www->ints[2] = 0;
        www->ints[1] = 2;
        printf("SG-JNI WWW done handle=%d size=%zu (stub)\n", handle, body_len);
        return;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        typedef void (*error_fn)(void *, void *, int32_t, void *);
        char message[512];
        snprintf(message, sizeof(message), "java.io.FileNotFoundException: %s",
                 path);
        printf("SG-JNI WWW fopen FAIL %s errno=%d\n", path, errno);
        if (error_native != NULL && error_native->fn != NULL)
            ((error_fn)error_native->fn)(&jni_handle, www, handle,
                                         new_string(message));
        www->ints[2] = 0;
        www->ints[1] = 2;
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0)
        size = 0;
    else
        size = ftell(file);
    if (size < 0)
        size = 0;
    rewind(file);
    snprintf(header, sizeof(header),
             "HTTP/1.0 200 OK\nContent-Type: application/octet-stream\n"
             "Content-Length: %ld\n\n",
             size);
    if (header_native != NULL && header_native->fn != NULL) {
        typedef int32_t (*header_fn)(void *, void *, int32_t, void *);
        (void)((header_fn)header_native->fn)(&jni_handle, www, handle,
                                             new_string(header));
    }
    if (size > 0 && read_native != NULL && read_native->fn != NULL) {
        typedef int32_t (*read_fn)(void *, void *, int32_t, void *, int32_t);
        char chunk[16384];
        size_t got;
        while ((got = fread(chunk, 1U, sizeof(chunk), file)) > 0U) {
            struct fake_object *bytes = new_object(FAKE_ARRAY, "[B");
            bytes->length = got;
            bytes->text = calloc(got, 1U);
            if (bytes->text == NULL)
                abort();
            memcpy(bytes->text, chunk, got);
            if (((read_fn)read_native->fn)(&jni_handle, www, handle, bytes,
                                           (int32_t)got) == 0)
                break;
        }
    }
    fclose(file);
    if (done_native != NULL && done_native->fn != NULL) {
        typedef void (*done_fn)(void *, void *, int32_t);
        ((done_fn)done_native->fn)(&jni_handle, www, handle);
    }
    www->ints[2] = 0;
    www->ints[1] = 2;
    printf("SG-JNI WWW done handle=%d size=%ld\n", handle, size);
}

static uintptr_t jni_unknown(void *environment, ...)
{
    static const char message[] = "SG-JNI UNKNOWN table call\n";
    (void)environment;
    (void)write(STDERR_FILENO, message, sizeof(message) - 1U);
    return 0U;
}

static int32_t jni_get_version(void *environment)
{
    (void)environment;
    return JNI_VERSION_1_6_VALUE;
}

static int32_t jni_int_ok(void *environment, ...)
{
    (void)environment;
    return JNI_OK_VALUE;
}

static void jni_void_noop(void *environment, ...)
{
    (void)environment;
}

static void *jni_no_exception(void *environment)
{
    (void)environment;
    return NULL;
}

static int32_t jni_exception_check(void *environment)
{
    (void)environment;
    return 0;
}

static int32_t jni_true(void *environment, ...)
{
    (void)environment;
    return 1;
}

static int32_t jni_is_instance_of(void *environment, void *object_value,
                                  void *class_value)
{
    struct fake_object *object = as_object(object_value);
    struct fake_object *class_object = as_object(class_value);
    const char *on;
    const char *cn;

    (void)environment;
    if (object == NULL || class_object == NULL)
        return 0;
    on = object->name != NULL ? object->name : "";
    cn = class_object->name != NULL ? class_object->name : "";
    if (strstr(cn, "java/lang/Object") != NULL ||
        strstr(cn, "java.lang.Object") != NULL)
        return 1;
    if (strstr(cn, "KeyEvent") != NULL)
        return strstr(on, "KeyEvent") != NULL;
    if (strstr(cn, "MotionEvent") != NULL)
        return strstr(on, "MotionEvent") != NULL;
    if (strstr(cn, "InputEvent") != NULL)
        return strstr(on, "KeyEvent") != NULL ||
               strstr(on, "MotionEvent") != NULL;
    if (strstr(cn, "InputDevice") != NULL)
        return strstr(on, "InputDevice") != NULL;
    return 1;
}

static void *jni_find_class(void *environment, const char *name)
{
    (void)environment;
    printf("SG-JNI FindClass %s\n", name != NULL ? name : "(null)");
    return new_object(FAKE_CLASS, name != NULL ? name : "java/lang/Object");
}

static void *jni_get_superclass(void *environment, void *class_value)
{
    struct fake_object *class_object = as_object(class_value);
    const char *name = class_object != NULL ? class_object->name : "(null)";
    (void)environment;
    printf("SG-JNI GetSuperclass %s\n", name);
    if (class_object == NULL || class_object->name == NULL)
        return new_object(FAKE_CLASS, "java/lang/Object");
    if (strcmp(class_object->name, "java/lang/Object") == 0 ||
        strcmp(class_object->name, "java.lang.Object") == 0)
        return NULL;
    return new_object(FAKE_CLASS, "java/lang/Object");
}

static int32_t jni_is_assignable_from(void *environment, void *from, void *to)
{
    (void)environment;
    (void)from;
    (void)to;
    return 1;
}

static void *jni_new_global_ref(void *environment, void *value)
{
    (void)environment;
    return value;
}

static void *jni_pop_local_frame(void *environment, void *result)
{
    (void)environment;
    return result;
}

static int32_t jni_is_same_object(void *environment, void *a, void *b)
{
    (void)environment;
    return a == b;
}

static void *jni_alloc_object(void *environment, void *class_value)
{
    struct fake_object *class_object = as_object(class_value);
    const char *name = class_object != NULL ? class_object->name : NULL;
    struct fake_object *created;
    enum fake_kind kind = FAKE_GENERIC;
    (void)environment;
    if (name != NULL && strstr(name, "File") != NULL)
        kind = FAKE_FILE;
    else if (name != NULL && strstr(name, "String") != NULL)
        kind = FAKE_STRING;
    created = new_object(kind, name);
    if (name != NULL && strstr(name, "FMODAudioDevice") != NULL)
        created->ints[1] = FMOD_DEVICE_MAGIC;
    printf("SG-JNI AllocObject %s\n", created->name);
    return created;
}

static void apply_string_ctor(struct fake_object *created, void *method,
                              void *first_arg)
{
    struct method_id *id = (struct method_id *)method;
    struct fake_object *argument = as_object(first_arg);

    if (created == NULL || argument == NULL || argument->text == NULL)
        return;
    if (id != NULL && strstr(id->signature, "Ljava/lang/String;") == NULL)
        return;
    if (created->text != NULL)
        free(created->text);
    created->text = dup_string(argument->text);
    if (created->name != NULL && strstr(created->name, "File") != NULL)
        created->kind = FAKE_FILE;
}

static void *jni_new_object(void *environment, void *class_value, void *method,
                            ...)
{
    struct fake_object *created = jni_alloc_object(environment, class_value);
    printf("SG-JNI NewObject %s %s %s\n",
           created != NULL && created->name != NULL ? created->name : "?",
           method_name(method), method_signature(method));
    if (is_www_ctor(method)) {
        va_list arguments;
        int32_t handle;
        void *url;
        va_start(arguments, method);
        handle = va_arg(arguments, int32_t);
        url = va_arg(arguments, void *);
        va_end(arguments);
        apply_www_ctor(created, handle, url);
        return created;
    }
    if (ctor_takes_string(method)) {
        va_list arguments;
        void *first;
        va_start(arguments, method);
        first = va_arg(arguments, void *);
        va_end(arguments);
        apply_string_ctor(created, method, first);
    }
    return created;
}

static void *jni_new_object_v(void *environment, void *class_value,
                              void *method, void *args)
{
    struct fake_object *created = jni_alloc_object(environment, class_value);
    if (is_www_ctor(method) && args != NULL) {
        apply_www_ctor_args(created, args);
        return created;
    }
    if (ctor_takes_string(method) && args != NULL)
        apply_string_ctor(created, method, *(void **)args);
    return created;
}

static void *jni_new_object_a(void *environment, void *class_value,
                              void *method, void *args)
{
    return jni_new_object_v(environment, class_value, method, args);
}

static void *jni_get_object_class(void *environment, void *value)
{
    struct fake_object *object = as_object(value);
    (void)environment;
    printf("SG-JNI GetObjectClass %s\n",
           object != NULL && object->name != NULL ? object->name : "null");
    return new_object(FAKE_CLASS, object != NULL ? object->name : NULL);
}

static void *jni_get_method_id(void *environment, void *class_value,
                               const char *name, const char *signature)
{
    struct fake_object *class_object = as_object(class_value);
    (void)environment;
    printf("SG-JNI GetMethodID %s %s %s\n",
           class_object != NULL && class_object->name != NULL ?
           class_object->name : "?",
           name != NULL ? name : "",
           signature != NULL ? signature : "");
    return intern_method(name, signature);
}

static void *jni_get_static_method_id(void *environment, void *class_value,
                                      const char *name, const char *signature)
{
    return jni_get_method_id(environment, class_value, name, signature);
}

static void *jni_get_field_id(void *environment, void *class_value,
                              const char *name, const char *signature)
{
    return jni_get_method_id(environment, class_value, name, signature);
}

static void *jni_get_static_field_id(void *environment, void *class_value,
                                     const char *name, const char *signature)
{
    return jni_get_method_id(environment, class_value, name, signature);
}

/* SharedPreferences.putX() used to be dropped on the floor, so the sensitivity
   slider and any gamepad rebinding reverted to the built-in defaults on the
   next level load. Back the settings with a file instead; deleting prefs.txt
   restores the defaults. */
#define PREFS_CAPACITY 256

static struct pref_entry {
    char *key;
    char *value;
} prefs_entries[PREFS_CAPACITY];
static size_t prefs_entry_count;
static int prefs_loaded;
static int prefs_dirty;

/* Permission, OBB and plugin gating answers are the runtime's business, not
   user settings. The game writes a 0 for the Google Play Games plugin that can
   never report in here, and once that 0 was saved it outvoted the forced 1 and
   left the loading screen waiting for a plugin that does not exist. */
static int prefs_key_is_runtime_owned(const char *key)
{
    return strstr(key, "ermission") != NULL || strstr(key, "OBB") != NULL ||
           strstr(key, "PLUGIN") != NULL || strstr(key, "Init") != NULL ||
           strstr(key, "State") != NULL || strstr(key, "Granted") != NULL ||
           strcmp(key, "OptionsXperiaViewPad") == 0;
}

static void prefs_store_set(const char *key, const char *value)
{
    size_t index;

    if (key == NULL || key[0] == '\0' || value == NULL)
        return;
    if (prefs_key_is_runtime_owned(key))
        return;
    for (index = 0U; index < prefs_entry_count; ++index) {
        if (strcmp(prefs_entries[index].key, key) != 0)
            continue;
        free(prefs_entries[index].value);
        prefs_entries[index].value = dup_string(value);
        prefs_dirty = 1;
        return;
    }
    if (prefs_entry_count >= PREFS_CAPACITY)
        return;
    prefs_entries[prefs_entry_count].key = dup_string(key);
    prefs_entries[prefs_entry_count].value = dup_string(value);
    prefs_entry_count += 1U;
    prefs_dirty = 1;
}

/* Gamepad profiles are multi-line strings, so the separators are escaped on
   disk and put back in place here. */
static void prefs_unescape(char *text)
{
    char *out = text;
    const char *in = text;

    for (; *in != '\0'; ++in) {
        if (*in != '\\') {
            *out++ = *in;
            continue;
        }
        switch (*++in) {
        case 'n': *out++ = '\n'; break;
        case 'r': *out++ = '\r'; break;
        case 't': *out++ = '\t'; break;
        case '\0': *out++ = '\\'; --in; break;
        default: *out++ = *in; break;
        }
    }
    *out = '\0';
}

static void prefs_file_path(char *out, size_t out_size)
{
    join_path(out, out_size, root_path(), "prefs.txt");
}

static void prefs_load(void)
{
    char path[PATH_CAPACITY];
    char line[1024];
    FILE *file;

    if (prefs_loaded)
        return;
    prefs_loaded = 1;
    prefs_file_path(path, sizeof(path));
    file = fopen(path, "r");
    if (file == NULL)
        return;
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        char *split;
        while (length > 0U &&
               (line[length - 1U] == '\n' || line[length - 1U] == '\r'))
            line[--length] = '\0';
        split = strchr(line, '\t');
        if (split == NULL)
            continue;
        *split = '\0';
        prefs_unescape(split + 1);
        prefs_store_set(line, split + 1);
    }
    fclose(file);
    prefs_dirty = 0;
    printf("SG-PREFS loaded %u keys from %s\n",
           (unsigned)prefs_entry_count, path);
}

static void prefs_save(void)
{
    char path[PATH_CAPACITY];
    FILE *file;
    size_t index;

    if (!prefs_dirty)
        return;
    prefs_file_path(path, sizeof(path));
    file = fopen(path, "w");
    if (file == NULL) {
        printf("SG-PREFS save failed %s\n", path);
        prefs_dirty = 0;
        return;
    }
    for (index = 0U; index < prefs_entry_count; ++index) {
        const char *value = prefs_entries[index].value;
        fprintf(file, "%s\t", prefs_entries[index].key);
        for (; *value != '\0'; ++value) {
            switch (*value) {
            case '\\': fputs("\\\\", file); break;
            case '\n': fputs("\\n", file); break;
            case '\r': fputs("\\r", file); break;
            case '\t': fputs("\\t", file); break;
            default: fputc(*value, file); break;
            }
        }
        fputc('\n', file);
    }
    fclose(file);
    prefs_dirty = 0;
}

static const char *prefs_store_get(const char *key)
{
    size_t index;

    prefs_load();
    if (key == NULL)
        return NULL;
    for (index = 0U; index < prefs_entry_count; ++index) {
        if (strcmp(prefs_entries[index].key, key) == 0)
            return prefs_entries[index].value;
    }
    return NULL;
}

/* A NULL key drops every entry, which is what Editor.clear() asks for. */
static void prefs_store_remove(const char *key)
{
    size_t index;

    prefs_load();
    for (index = prefs_entry_count; index > 0U; --index) {
        struct pref_entry *entry = &prefs_entries[index - 1U];
        if (key != NULL && strcmp(entry->key, key) != 0)
            continue;
        free(entry->key);
        free(entry->value);
        *entry = prefs_entries[prefs_entry_count - 1U];
        prefs_entry_count -= 1U;
        prefs_dirty = 1;
        if (key != NULL)
            return;
    }
}

static void prefs_put_text(const char *key, const char *value)
{
    prefs_load();
    prefs_store_set(key, value);
    prefs_save();
    printf("SG-PREFS put '%s'='%s'\n", key != NULL ? key : "",
           value != NULL ? value : "");
}

static int prefs_is_put(const char *name)
{
    return strcmp(name, "putInt") == 0 || strcmp(name, "putLong") == 0 ||
           strcmp(name, "putFloat") == 0 || strcmp(name, "putBoolean") == 0 ||
           strcmp(name, "putString") == 0;
}

static const char *prefs_key_text(void *key_value)
{
    struct fake_object *key = as_object(key_value);
    return key != NULL && key->text != NULL ? key->text : "";
}

/* Varargs path: the slots are packed 4-byte, and a jfloat argument has been
   promoted to jdouble by the caller's compiler. */
static void prefs_put_va(const char *name, va_list args)
{
    const char *key = prefs_key_text(va_arg(args, void *));
    char text[64];

    if (strcmp(name, "putString") == 0) {
        prefs_put_text(key, prefs_key_text(va_arg(args, void *)));
        return;
    }
    if (strcmp(name, "putFloat") == 0)
        snprintf(text, sizeof(text), "%.6g", va_arg(args, double));
    else if (strcmp(name, "putLong") == 0)
        snprintf(text, sizeof(text), "%lld", (long long)va_arg(args, int64_t));
    else
        snprintf(text, sizeof(text), "%d", va_arg(args, int32_t));
    prefs_put_text(key, text);
}

/* jvalue path: every argument occupies an 8-byte union, so the value starts one
   union past the key. */
static void prefs_put_jvalue(const char *name, void *args)
{
    void **slots = args;
    const char *key = prefs_key_text(slots[0]);
    char text[64];

    if (strcmp(name, "putString") == 0) {
        prefs_put_text(key, prefs_key_text(slots[2]));
        return;
    }
    if (strcmp(name, "putFloat") == 0)
        snprintf(text, sizeof(text), "%.6g", (double)*(float *)(slots + 2));
    else if (strcmp(name, "putLong") == 0)
        snprintf(text, sizeof(text), "%lld",
                 (long long)*(int64_t *)(slots + 2));
    else
        snprintf(text, sizeof(text), "%d", *(int32_t *)(slots + 2));
    prefs_put_text(key, text);
}

static void *call_object(void *object_value, void *method, const char *static_class)
{
    const char *name = method_name(method);
    struct fake_object *object = as_object(object_value);
    char path[PATH_CAPACITY];
    int noisy;

    noisy = strcmp(name, "getDeviceIds") == 0 || strcmp(name, "getName") == 0 ||
            strcmp(name, "obtain") == 0;
    if (!noisy) {
        printf("SG-JNI CallObject %s obj=%s text=%s\n", name,
               object != NULL && object->name != NULL ? object->name : "null",
               object != NULL && object->text != NULL ? object->text : "");
    } else {
        static unsigned noisy_logs;
        if (noisy_logs < 8U) {
            printf("SG-JNI CallObject %s obj=%s text=%s\n", name,
                   object != NULL && object->name != NULL ? object->name : "null",
                   object != NULL && object->text != NULL ? object->text : "");
            noisy_logs += 1U;
        }
    }

    if (strcmp(name, "getPackageName") == 0)
        return new_string(package_name());
    if (strcmp(name, "getPackageCodePath") == 0 ||
        strcmp(name, "getPackageResourcePath") == 0) {
        join_path(path, sizeof(path), root_path(), apk_path());
        return new_string(path);
    }
    if (strcmp(name, "getApplicationInfo") == 0)
        return app_info;
    if (strcmp(name, "getApplicationContext") == 0 ||
        strcmp(name, "getApplication") == 0 ||
        strcmp(name, "getBaseContext") == 0)
        return activity;
    if (strcmp(name, "getFilesDir") == 0)
        return new_file("files");
    if (strcmp(name, "getCacheDir") == 0)
        return new_file("cache");
    if (strcmp(name, "getObbDir") == 0) {
        snprintf(path, sizeof(path), "Android/obb/%s", package_name());
        return new_file(path);
    }
    if (strcmp(name, "getExternalFilesDir") == 0 ||
        strcmp(name, "getExternalCacheDir") == 0)
        return new_file("files");
    if (strcmp(name, "getAbsolutePath") == 0 || strcmp(name, "getPath") == 0 ||
        strcmp(name, "getCanonicalPath") == 0)
        return new_string(file_path_text(object));
    if (strcmp(name, "getParent") == 0)
        return file_parent_string(object);
    if (strcmp(name, "getParentFile") == 0) {
        struct fake_object *parent_string = file_parent_string(object);
        struct fake_object *file = new_object(FAKE_FILE, "java/io/File");
        file->text = dup_string(parent_string != NULL &&
                                parent_string->text != NULL ?
                                parent_string->text : "/");
        return file;
    }
    if (strcmp(name, "getAbsoluteFile") == 0 ||
        strcmp(name, "getCanonicalFile") == 0) {
        struct fake_object *file = new_object(FAKE_FILE, "java/io/File");
        file->text = dup_string(file_path_text(object));
        return file;
    }
    if (strcmp(name, "getAssets") == 0)
        return new_object(FAKE_GENERIC, "android/content/res/AssetManager");
    if (strcmp(name, "getSystemService") == 0)
        return new_object(FAKE_GENERIC, "android/view/WindowManager");
    if (strcmp(name, "getActiveNetworkInfo") == 0 ||
        strcmp(name, "getNetworkInfo") == 0)
        return new_object(FAKE_GENERIC, "android/net/NetworkInfo");
    if (strcmp(name, "GetExpansionFilePath") == 0 ||
        strcmp(name, "getExpansionFilePath") == 0) {
        fill_expansion_path(path, sizeof(path));
        printf("SG-JNI GetExpansionFilePath %s\n", path);
        return new_string(path);
    }
    if (strcmp(name, "getWindowManager") == 0)
        return new_object(FAKE_GENERIC, "android/view/WindowManager");
    if (strcmp(name, "getDefaultDisplay") == 0)
        return new_object(FAKE_GENERIC, "android/view/Display");
    if (strcmp(name, "getHolder") == 0)
        return new_object(FAKE_GENERIC, "android/view/SurfaceHolder");
    if (strcmp(name, "getSurface") == 0)
        return surface;
    if (strcmp(name, "getWindow") == 0)
        return new_object(FAKE_GENERIC, "android/view/Window");
    if (strcmp(name, "getDecorView") == 0)
        return new_object(FAKE_GENERIC, "android/view/View");
    if (strcmp(name, "getResources") == 0)
        return new_object(FAKE_GENERIC, "android/content/res/Resources");
    if (strcmp(name, "getDisplayMetrics") == 0)
        return new_object(FAKE_GENERIC, "android/util/DisplayMetrics");
    if (strcmp(name, "getConfiguration") == 0)
        return new_object(FAKE_GENERIC, "android/content/res/Configuration");
    if (strcmp(name, "getClass") == 0)
        return jni_get_object_class(NULL, object_value);
    if (strcmp(name, "getDescriptor") == 0 &&
        object_is_named(object, "InputDevice"))
        return new_string(object->text != NULL ? object->text : "Sony Ericsson");
    if (strcmp(name, "getName") == 0) {
        if (object_is_named(object, "InputDevice") &&
            !object_is_named(object, "MotionRange"))
            return new_string(object->text != NULL ? object->text
                                                   : PAD_DEVICE_NAME);
        if (object != NULL && object->kind == FAKE_FILE) {
            const char *path_text = file_path_text(object);
            const char *slash = strrchr(path_text, '/');
            return new_string(slash != NULL && slash[1] != '\0' ?
                              slash + 1 : path_text);
        }
        if (object != NULL && object->kind == FAKE_CLASS)
            return dotted_class_string(object);
        return new_string(object != NULL ? object->name : "java.lang.Object");
    }
    if (strcmp(name, "getCanonicalName") == 0 ||
        strcmp(name, "getTypeName") == 0)
        return dotted_class_string(object);
    if (strcmp(name, "getSimpleName") == 0)
        return simple_class_string(object);
    if (strcmp(name, "toString") == 0) {
        if (object != NULL && object->text != NULL)
            return new_string(object->text);
        if (object != NULL && object->name != NULL &&
            strstr(object->name, "Integer") != NULL) {
            char number[16];
            snprintf(number, sizeof(number), "%d",
                     object->ints[0] != 0 ? object->ints[0] : 1);
            return new_string(number);
        }
        return new_string("sg-jni");
    }
    if (strcmp(name, "getSharedPreferences") == 0 ||
        strcmp(name, "getDefaultSharedPreferences") == 0)
        return new_object(FAKE_GENERIC, "android/content/SharedPreferences");
    if (strcmp(name, "getAll") == 0) {
        struct fake_object *map = new_object(FAKE_GENERIC, "java/util/HashMap");
        map->ints[0] = 4;
        map->ints[1] = 0x5046;
        return map;
    }
    if (strcmp(name, "entrySet") == 0 || strcmp(name, "keySet") == 0 ||
        strcmp(name, "values") == 0) {
        struct fake_object *set = new_object(FAKE_GENERIC, "java/util/Set");
        if (object != NULL) {
            set->ints[0] = object->ints[0];
            set->ints[1] = object->ints[1];
        }
        return set;
    }
    if (strcmp(name, "iterator") == 0) {
        struct fake_object *it = new_object(FAKE_GENERIC, "java/util/Iterator");
        if (object != NULL) {
            it->ints[0] = object->ints[0];
            it->ints[1] = object->ints[1];
            if (object->ints[1] == MOTION_RANGE_MAGIC) {
                it->elements = object->elements;
                it->length = object->length;
                it->ints[0] = 0;
            }
        }
        return it;
    }
    if (strcmp(name, "next") == 0) {
        static const char *keys[] = {
            "OBB_PERMISSIONS", "LastRequestState",
            "AndroidPermissions", "PLUGIN.AndroidPermissions.INIT"
        };
        struct fake_object *entry;
        int index;
        if (object != NULL && object->ints[1] == MOTION_RANGE_MAGIC) {
            size_t cursor = (size_t)object->ints[0];
            if (object->elements == NULL || cursor >= object->length)
                return NULL;
            object->ints[0] = (int32_t)(cursor + 1U);
            return object->elements[cursor];
        }
        if (object == NULL || object->ints[1] != 0x5046 || object->ints[0] <= 0)
            return NULL;
        index = 4 - object->ints[0];
        object->ints[0] -= 1;
        if (index < 0)
            index = 0;
        if (index > 3)
            index = 3;
        entry = new_object(FAKE_GENERIC, "java/util/Map$Entry");
        entry->text = dup_string(keys[index]);
        entry->ints[0] = 1;
        printf("SG-JNI prefs next '%s'=1\n", keys[index]);
        return entry;
    }
    if (strcmp(name, "getKey") == 0)
        return new_string(object != NULL && object->text != NULL ?
                          object->text : "");
    if (strcmp(name, "getValue") == 0) {
        /* Unity 5.3 PlayerPrefs upgrade does GetStringUTFChars(getValue()),
           not Integer.intValue. Empty string → LastRequestState stays Pending. */
        char number[16];
        int value = object != NULL && object->ints[0] != 0 ? object->ints[0] : 1;
        snprintf(number, sizeof(number), "%d", value);
        printf("SG-JNI prefs getValue '%s'='%s'\n",
               object != NULL && object->text != NULL ? object->text : "",
               number);
        return new_string(number);
    }
    if (strcmp(name, "edit") == 0)
        return new_object(FAKE_GENERIC,
                          "android/content/SharedPreferences$Editor");
    if (strcmp(name, "putInt") == 0 || strcmp(name, "putLong") == 0 ||
        strcmp(name, "putFloat") == 0 || strcmp(name, "putBoolean") == 0 ||
        strcmp(name, "putString") == 0 || strcmp(name, "putStringSet") == 0 ||
        strcmp(name, "remove") == 0 || strcmp(name, "clear") == 0) {
        if (strcmp(name, "clear") == 0 && object != NULL &&
            object->name != NULL &&
            strstr(object->name, "SharedPreferences") != NULL) {
            prefs_store_remove(NULL);
            prefs_save();
            printf("SG-PREFS cleared\n");
        }
        return object_value != NULL ? object_value :
            new_object(FAKE_GENERIC, "android/content/SharedPreferences$Editor");
    }
    if (strcmp(name, "newInterfaceProxy") == 0)
        return new_object(FAKE_GENERIC, "java/lang/Object");
    if (strcmp(name, "getString") == 0) {
        if (object != NULL && object->name != NULL &&
            strstr(object->name, "SharedPreferences") != NULL)
            return new_string("1");
        return new_string("");
    }
    if (strcmp(name, "getIntent") == 0)
        return new_object(FAKE_GENERIC, "android/content/Intent");
    if (strcmp(name, "getExtras") == 0)
        return new_object(FAKE_GENERIC, "android/os/Bundle");
    if (strcmp(name, "getMainOBBPath") == 0 ||
        strcmp(name, "GetMainOBBPath") == 0) {
        fill_obb_path(path, sizeof(path));
        printf("SG-JNI GetMainOBBPath %s\n", path);
        return new_string(path);
    }
    if (strcmp(name, "getPatchOBBPath") == 0 ||
        strcmp(name, "GetPatchOBBPath") == 0)
        return NULL;
    if (strcmp(name, "getExternalStorageDirectory") == 0)
        return new_file_abs(root_path());
    if (strcmp(name, "getExternalStorageState") == 0)
        return new_string("mounted");
    if (strcmp(name, "getDataDirectory") == 0)
        return new_file("files");
    if (strcmp(name, "getDir") == 0)
        return new_file("files");
    if (strcmp(name, "getClassLoader") == 0)
        return new_object(FAKE_GENERIC, "java/lang/ClassLoader");
    if (strcmp(name, "getDeviceIds") == 0) {
        struct fake_object *ids = new_object(FAKE_ARRAY, "[I");
        ids->length = 1U;
        ids->ints[0] = 1;
        return ids;
    }
    if (strcmp(name, "getMotionRanges") == 0)
        return motion_range_list();
    if (strcmp(name, "obtain") == 0 && object_is_named(object, "MotionEvent")) {
        /* Unity queues a copy of every injected MotionEvent and later checks
           IsInstanceOf(copy, MotionEvent) before reading its axes, so the copy
           must stay a MotionEvent instead of a generic fallback object. */
        if (motion_event == NULL)
            motion_event = new_object(FAKE_GENERIC, "android/view/MotionEvent");
        return motion_event;
    }
    if (strcmp(name, "getPackageManager") == 0)
        return new_object(FAKE_GENERIC, "android/content/pm/PackageManager");
    if (strcmp(name, "getPackageInfo") == 0)
        return new_object(FAKE_GENERIC, "android/content/pm/PackageInfo");
    if (strcmp(name, "getContentResolver") == 0)
        return new_object(FAKE_GENERIC, "android/content/ContentResolver");
    if (strcmp(name, "getConstructor") == 0 ||
        strcmp(name, "getDeclaredConstructor") == 0)
        return new_object(FAKE_GENERIC, "java/lang/reflect/Constructor");
    if (strcmp(name, "getMethod") == 0 ||
        strcmp(name, "getDeclaredMethod") == 0)
        return new_object(FAKE_GENERIC, "java/lang/reflect/Method");
    if (strcmp(name, "getField") == 0 ||
        strcmp(name, "getDeclaredField") == 0)
        return new_object(FAKE_GENERIC, "java/lang/reflect/Field");
    if (strcmp(name, "newInstance") == 0) {
        if (object != NULL && object->name != NULL &&
            strstr(object->name, "File") != NULL)
            return new_file("files");
        return new_object(FAKE_GENERIC, "java/lang/Object");
    }
    if (strcmp(name, "list") == 0 || strcmp(name, "listFiles") == 0) {
        struct fake_object *array = new_object(FAKE_ARRAY, "[Ljava/io/File;");
        array->length = 0U;
        return array;
    }
    if (strcmp(name, "getBytes") == 0)
        return new_object(FAKE_ARRAY, "[B");
    if (strcmp(name, "checkPermissions") == 0 ||
        strcmp(name, "getGrantedResults") == 0) {
        struct fake_object *ids = new_object(FAKE_ARRAY, "[I");
        ids->length = 4U;
        printf("SG-JNI %s granted n=4\n", name);
        return ids;
    }
    if (strcmp(name, "findLibrary") == 0)
        return NULL;
    if (strcmp(name, "mapLibraryName") == 0)
        return new_string("libunity.so");
    if (strcmp(name, "registerReceiver") == 0)
        return NULL;
    if (strcmp(name, "getErrorReportReceiver") == 0)
        return NULL;
    (void)static_class;
    {
        const char *sig = method_signature(method);
        const char *ret = strrchr(sig, ')');
        printf("SG-JNI CallObject %s %s\n", name, sig);
        if (ret != NULL) {
            ret += 1;
            if (ret[0] == '[' )
                return new_object(FAKE_ARRAY, ret);
            if (strstr(ret, "java/lang/String;") != NULL)
                return new_string("");
            if (strstr(ret, "java/io/File;") != NULL)
                return new_file("files");
            if (ret[0] == 'L')
                return new_object(FAKE_GENERIC, name);
        }
        return object_value != NULL ? object_value :
            new_object(FAKE_GENERIC, name);
    }
}

static int is_reflect_lookup(void *method)
{
    const char *name = method_name(method);
    return strcmp(name, "getMethodID") == 0 ||
           strcmp(name, "getFieldID") == 0 ||
           strcmp(name, "getConstructorID") == 0;
}

static void *make_reflected(const char *kind, void *name_obj, void *sig_obj)
{
    struct fake_object *ns = as_object(name_obj);
    struct fake_object *ss = as_object(sig_obj);
    const char *n = ns != NULL && ns->text != NULL ? ns->text : "";
    const char *s = ss != NULL && ss->text != NULL ? ss->text : "";
    void *id = intern_method(n, s);
    struct fake_object *reflected = new_object(FAKE_GENERIC, kind);
    reflected->elements = calloc(1U, sizeof(*reflected->elements));
    if (reflected->elements == NULL)
        abort();
    reflected->elements[0] = id;
    reflected->length = 1U;
    reflected->text = dup_string(n);
    printf("SG-JNI reflect %s %s %s -> %p\n", kind, n, s, id);
    return reflected;
}

static void *reflect_lookup_va(void *method, va_list args)
{
    const char *name = method_name(method);
    void *clazz = va_arg(args, void *);
    (void)clazz;
    if (strcmp(name, "getConstructorID") == 0) {
        void *jsig = va_arg(args, void *);
        return make_reflected("java/lang/reflect/Constructor",
                              new_string("<init>"), jsig);
    }
    {
        void *jname = va_arg(args, void *);
        void *jsig = va_arg(args, void *);
        if (strcmp(name, "getFieldID") == 0)
            return make_reflected("java/lang/reflect/Field", jname, jsig);
        return make_reflected("java/lang/reflect/Method", jname, jsig);
    }
}

static void *jni_from_reflected_method(void *environment, void *method_obj)
{
    struct fake_object *object = as_object(method_obj);
    (void)environment;
    if (object != NULL && object->elements != NULL && object->length > 0U &&
        object->elements[0] != NULL) {
        printf("SG-JNI FromReflectedMethod %s\n",
               method_name(object->elements[0]));
        return object->elements[0];
    }
    printf("SG-JNI FromReflectedMethod fallback obj=%p\n", method_obj);
    return intern_method("invoke",
                         "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
}

static void *jni_from_reflected_field(void *environment, void *field_obj)
{
    return jni_from_reflected_method(environment, field_obj);
}

static void *jni_to_reflected_method(void *environment, void *clazz,
                                     void *method_id, uint8_t is_static)
{
    struct fake_object *reflected =
        new_object(FAKE_GENERIC, "java/lang/reflect/Method");
    (void)environment;
    (void)clazz;
    (void)is_static;
    reflected->elements = calloc(1U, sizeof(*reflected->elements));
    if (reflected->elements == NULL)
        abort();
    reflected->elements[0] = method_id;
    reflected->length = 1U;
    reflected->text = dup_string(method_name(method_id));
    return reflected;
}

static void *jni_to_reflected_field(void *environment, void *clazz,
                                    void *field_id, uint8_t is_static)
{
    struct fake_object *reflected =
        new_object(FAKE_GENERIC, "java/lang/reflect/Field");
    (void)environment;
    (void)clazz;
    (void)is_static;
    reflected->elements = calloc(1U, sizeof(*reflected->elements));
    if (reflected->elements == NULL)
        abort();
    reflected->elements[0] = field_id;
    reflected->length = 1U;
    reflected->text = dup_string(method_name(field_id));
    return reflected;
}

static void *system_service_for_name(const char *text)
{
    if (text == NULL)
        text = "";
    printf("SG-JNI getSystemService %s\n", text);
    if (strcmp(text, "connectivity") == 0)
        return new_object(FAKE_GENERIC, "android/net/ConnectivityManager");
    if (strcmp(text, "phone") == 0)
        return new_object(FAKE_GENERIC, "android/telephony/TelephonyManager");
    if (strcmp(text, "audio") == 0)
        return new_object(FAKE_GENERIC, "android/media/AudioManager");
    if (strcmp(text, "input") == 0)
        return new_object(FAKE_GENERIC, "android/hardware/input/InputManager");
    if (strcmp(text, "sensor") == 0)
        return new_object(FAKE_GENERIC, "android/hardware/SensorManager");
    return new_object(FAKE_GENERIC, "android/view/WindowManager");
}

static void *uri_passthrough(const char *name, void *arg)
{
    struct fake_object *text = as_object(arg);
    const char *value = text != NULL && text->text != NULL ? text->text : "";
    printf("SG-JNI Uri.%s '%s'\n", name, value);
    return new_string(value);
}

static void *call_object_with_args(void *object, void *method, va_list args)
{
    const char *name = method_name(method);
    if (is_reflect_lookup(method))
        return reflect_lookup_va(method, args);
    if (strcmp(name, "getSystemService") == 0) {
        struct fake_object *key = as_object(va_arg(args, void *));
        return system_service_for_name(key != NULL ? key->text : NULL);
    }
    if (strcmp(name, "forName") == 0)
        return class_for_name(va_arg(args, void *));
    if (strcmp(name, "encode") == 0 || strcmp(name, "decode") == 0)
        return uri_passthrough(name, va_arg(args, void *));
    if (strcmp(name, "getDevice") == 0)
        return input_device_object(va_arg(args, int32_t));
    if (prefs_is_put(name)) {
        prefs_put_va(name, args);
        return call_object(object, method, NULL);
    }
    if (strcmp(name, "remove") == 0) {
        prefs_store_remove(prefs_key_text(va_arg(args, void *)));
        prefs_save();
        return call_object(object, method, NULL);
    }
    if (strcmp(name, "getString") == 0) {
        struct fake_object *key = as_object(va_arg(args, void *));
        const char *key_text = (key != NULL && key->text != NULL) ?
            key->text : "";
        const char *saved = prefs_store_get(key_text);
        printf("SG-JNI getString_va key='%s'\n", key_text);
        if (saved != NULL)
            return new_string(saved);
        /* With nothing saved, report every gamepad slot as empty so the game
           applies its built-in defaults for PAD_DEVICE_NAME. A hand-written
           profile here only trips its own validation ("Load failed - lengt of
           parsed strings do not match"). */
        if (strncmp(key_text, "Gamepad", 7) == 0) {
            printf("SG-JNI gamepad slot empty (va): %s\n", key_text);
            return new_string("");
        }
    }
    if (strcmp(name, "getMotionRange") == 0)
        return motion_range_object(va_arg(args, int32_t));
    return call_object(object, method, NULL);
}

static void *jni_call_object_method(void *environment, void *object,
                                    void *method, ...)
{
    va_list args;
    void *result;
    (void)environment;
    va_start(args, method);
    result = call_object_with_args(object, method, args);
    va_end(args);
    return result;
}

static void *jni_call_object_method_v(void *environment, void *object,
                                      void *method, va_list args)
{
    (void)environment;
    return call_object_with_args(object, method, args);
}

static void *jni_call_object_method_a(void *environment, void *object,
                                      void *method, void *args)
{
    const char *name = method_name(method);
    struct {
        union {
            void *l;
            int32_t i;
            uint8_t z;
        } u;
        void *pad;
    } *jv = args;
    (void)environment;
    if (args != NULL && prefs_is_put(name)) {
        prefs_put_jvalue(name, args);
        return call_object(object, method, NULL);
    }
    if (args != NULL && strcmp(name, "remove") == 0) {
        prefs_store_remove(prefs_key_text(jv[0].u.l));
        prefs_save();
        return call_object(object, method, NULL);
    }
    if (args != NULL && strcmp(name, "getString") == 0) {
        struct fake_object *owner = as_object(object);
        if (owner != NULL && owner->name != NULL &&
            strstr(owner->name, "SharedPreferences") != NULL) {
            struct fake_object *key = as_object(jv[0].u.l);
            const char *key_text = (key != NULL && key->text != NULL) ?
                key->text : "";
            const char *saved = prefs_store_get(key_text);
            printf("SG-JNI getString key='%s'\n", key_text);
            if (saved != NULL)
                return new_string(saved);
            if (strncmp(key_text, "Gamepad", 7) == 0) {
                printf("SG-JNI gamepad slot empty: %s\n", key_text);
                return new_string("");
            }
            return new_string("1");
        }
        return new_string("");
    }
    if (args != NULL && strcmp(name, "getSystemService") == 0) {
        struct fake_object *key = as_object(jv[0].u.l);
        return system_service_for_name(key != NULL ? key->text : NULL);
    }
    if (args != NULL && strcmp(name, "forName") == 0)
        return class_for_name(jv[0].u.l);
    if (args != NULL && is_reflect_lookup(method)) {
        if (strcmp(name, "getConstructorID") == 0)
            return make_reflected("java/lang/reflect/Constructor",
                                  new_string("<init>"), jv[1].u.l);
        if (strcmp(name, "getFieldID") == 0)
            return make_reflected("java/lang/reflect/Field", jv[1].u.l,
                                  jv[2].u.l);
        return make_reflected("java/lang/reflect/Method", jv[1].u.l, jv[2].u.l);
    }
    if (args != NULL &&
        (strcmp(name, "encode") == 0 || strcmp(name, "decode") == 0))
        return uri_passthrough(name, jv[0].u.l);
    if (args != NULL && strcmp(name, "getDevice") == 0)
        return input_device_object(jv[0].u.i);
    if (args != NULL && strcmp(name, "getMotionRange") == 0)
        return motion_range_object(jv[0].u.i);
    return call_object(object, method, NULL);
}

static int32_t call_int(void *object_value, void *method)
{
    const char *name = method_name(method);
    struct fake_object *object = as_object(object_value);
    /* settings.xml splash_mode=1: scale the splash to the panel.
       Falling through to 0 leaves it at a tiny origin rect. */
    if (strcmp(name, "getSplashMode") == 0)
        return 1;
    if (strcmp(name, "getWidth") == 0 || strcmp(name, "getRawWidth") == 0)
        return nfsmw_screen_width();
    if (strcmp(name, "getHeight") == 0 || strcmp(name, "getRawHeight") == 0)
        return nfsmw_screen_height();
    if (strcmp(name, "getRotation") == 0)
        return 0;
    if (strcmp(name, "getAction") == 0) {
        if (object_is_named(object, "MotionEvent"))
            return object != NULL ? object->ints[0] : 0;
        return object != NULL ? object->ints[1] : 0;
    }
    if (strcmp(name, "getKeyCode") == 0 || strcmp(name, "getScanCode") == 0)
        return object != NULL ? object->ints[0] : 0;
    if (strcmp(name, "getDeviceId") == 0 || strcmp(name, "getDisplayId") == 0)
        return object != NULL && object->ints[2] != 0 ? object->ints[2] : 1;
    if (strcmp(name, "getSource") == 0) {
        /* A gamepad stick range belongs to SOURCE_JOYSTICK alone; the combined
           device sources would fail Unity's exact source-class match. */
        if (object_is_named(object, "MotionRange"))
            return ASOURCE_JOYSTICK;
        if (object_is_named(object, "MotionEvent") && object->ints[3] != 0) {
            static unsigned motion_source_logs;
            if (motion_source_logs < 4U) {
                printf("SG-JNI MotionEvent getSource -> 0x%08x action=%d\n",
                       (unsigned)object->ints[3], (int)object->ints[0]);
                motion_source_logs += 1U;
            }
            return object->ints[3];
        }
        if (object_is_named(object, "KeyEvent") && object->ints[3] != 0)
            return object->ints[3];
        if (object_is_named(object, "InputDevice"))
            return ASOURCE_XPERIA_PAD;
        return ASOURCE_XPERIA_PAD;
    }
    if (strcmp(name, "getSources") == 0)
        return ASOURCE_XPERIA_PAD;
    if (strcmp(name, "getPointerCount") == 0)
        return 1;
    if (strcmp(name, "getPointerId") == 0 || strcmp(name, "getMetaState") == 0 ||
        strcmp(name, "getFlags") == 0 || strcmp(name, "getEdgeFlags") == 0 ||
        strcmp(name, "getRepeatCount") == 0 ||
        strcmp(name, "getButtonState") == 0 ||
        strcmp(name, "getHistorySize") == 0 ||
        strcmp(name, "getToolType") == 0)
        return 0;
    if (strcmp(name, "getActionMasked") == 0)
        return object != NULL ? (object_is_named(object, "MotionEvent") ?
                                 object->ints[0] : object->ints[1]) & 0xff : 0;
    if (strcmp(name, "getActionIndex") == 0)
        return 0;
    if (strcmp(name, "getAxis") == 0)
        return object != NULL ? object->ints[0] : 0;
    if (strcmp(name, "getVendorId") == 0)
        return 0x054c;
    if (strcmp(name, "getProductId") == 0)
        return 0x04ee;
    if (strcmp(name, "getControllerNumber") == 0)
        return 1;
    if (strcmp(name, "getType") == 0)
        return 1;
    if (strcmp(name, "getSdkVersion") == 0)
        return ANDROID_SDK_INT;
    if (strcmp(name, "checkCallingOrSelfPermission") == 0 ||
        strcmp(name, "checkSelfPermission") == 0 ||
        strcmp(name, "checkPermission") == 0)
        return 0;
    if (strcmp(name, "mkdirs") == 0 || strcmp(name, "mkdir") == 0)
        return 1;
    if (strcmp(name, "exists") == 0)
        return 1;
    if (strcmp(name, "canRead") == 0 || strcmp(name, "canWrite") == 0)
        return 1;
    if (strcmp(name, "isDirectory") == 0)
        return 1;
    if (strcmp(name, "isFile") == 0 || strcmp(name, "isAbsolute") == 0)
        return 1;
    if (strcmp(name, "getInt") == 0)
        return 0;
    if (strcmp(name, "intValue") == 0)
        return object != NULL && object->ints[0] != 0 ? object->ints[0] : 1;
    if (strcmp(name, "length") == 0) {
        if (object != NULL && object->kind == FAKE_ARRAY)
            return (int32_t)object->length;
        if (object != NULL && object->kind == FAKE_DIRECT)
            return (int32_t)object->length;
        if (object != NULL && object->text != NULL)
            return (int32_t)strlen(object->text);
        return 0;
    }
    if (strcmp(name, "capacity") == 0 || strcmp(name, "remaining") == 0 ||
        strcmp(name, "limit") == 0) {
        if (object != NULL && object->kind == FAKE_DIRECT)
            return (int32_t)object->length;
        return 0;
    }
    if (strcmp(name, "position") == 0)
        return 0;
    if (strcmp(name, "size") == 0) {
        if (object != NULL && object->ints[1] == MOTION_RANGE_MAGIC)
            return (int32_t)object->length;
        return 0;
    }
    if (strcmp(name, "hashCode") == 0)
        return 1;
    if (strcmp(name, "available") == 0)
        return 0;
    printf("SG-JNI CallInt %s -> 0\n", name);
    return 0;
}

static int is_prefs_object(void *object_value)
{
    struct fake_object *owner = as_object(object_value);
    return owner != NULL && owner->name != NULL &&
           strstr(owner->name, "SharedPreferences") != NULL;
}

static int32_t prefs_get_int(void *key_value, int32_t fallback)
{
    struct fake_object *key = as_object(key_value);
    const char *text = key != NULL && key->text != NULL ? key->text : "";
    const char *saved;
    int32_t value = fallback;

    if (strstr(text, "ermission") != NULL || strstr(text, "OBB") != NULL ||
        strstr(text, "PLUGIN") != NULL || strstr(text, "Init") != NULL ||
        strstr(text, "State") != NULL || strstr(text, "Granted") != NULL) {
        printf("SG-JNI getInt '%s' forced -> 1\n", text);
        return 1;
    }
    /* Defaults to 1 in game, which sends the camera to the Xperia Play
       touchpad instead of the gamepad stick. */
    if (strcmp(text, "OptionsXperiaViewPad") == 0) {
        printf("SG-JNI getInt '%s' forced -> 0\n", text);
        return 0;
    }
    saved = prefs_store_get(text);
    if (saved != NULL) {
        value = (int32_t)strtol(saved, NULL, 10);
        printf("SG-JNI getInt '%s' saved -> %d\n", text, value);
        return value;
    }
    printf("SG-JNI getInt '%s' def=%d -> %d\n", text, fallback, value);
    return value;
}

/* SharedPreferences.getFloat(): returning 0 for everything left the camera
   sensitivity slider at zero, so the stick moved nothing even once the axes
   arrived. Only the key is inspected; the caller's default is a float argument
   whose position differs between the varargs and jvalue paths. */
static float prefs_get_float(void *key_value)
{
    struct fake_object *key = as_object(key_value);
    const char *text = key != NULL && key->text != NULL ? key->text : "";
    const char *saved = prefs_store_get(text);
    float value = 0.0f;

    if (saved != NULL) {
        value = (float)strtod(saved, NULL);
        printf("SG-JNI getFloat '%s' saved -> %.3f\n", text, (double)value);
        return value;
    }
    /* Only the starting point now that the slider persists: the stick felt
       slow at 0.6 and every level load fell back to this value. */
    if (strcmp(text, "OptionsSensitivity") == 0)
        value = 1.0f;
    else if (strcmp(text, "OptionsMusicVolume") == 0)
        value = 0.6f;
    printf("SG-JNI getFloat '%s' -> %.3f\n", text, (double)value);
    return value;
}

static int32_t jni_call_int_method(void *environment, void *object,
                                   void *method, ...)
{
    const char *name = method_name(method);
    (void)environment;
    if (strcmp(name, "getInt") == 0) {
        struct fake_object *owner = as_object(object);
        int prefs = owner != NULL && owner->name != NULL &&
                    strstr(owner->name, "SharedPreferences") != NULL;
        if (prefs) {
            va_list arguments;
            void *key;
            int32_t fallback;
            va_start(arguments, method);
            key = va_arg(arguments, void *);
            fallback = va_arg(arguments, int32_t);
            va_end(arguments);
            return prefs_get_int(key, fallback);
        }
    }
    return call_int(object, method);
}

static int32_t jni_call_int_method_v(void *environment, void *object,
                                     void *method, void *args)
{
    const char *name = method_name(method);
    if (strcmp(name, "getInt") == 0 && args != NULL) {
        struct fake_object *owner = as_object(object);
        if (owner != NULL && owner->name != NULL &&
            strstr(owner->name, "SharedPreferences") != NULL) {
            void **slots = args;
            struct fake_object *packed = as_object(slots[0]);
            union {
                int32_t i;
                void *l;
                int64_t j;
                double d;
            } *jv = args;
            if (packed != NULL)
                return prefs_get_int(packed, *(int32_t *)(slots + 1));
            return prefs_get_int(jv[0].l, jv[1].i);
        }
    }
    (void)environment;
    return call_int(object, method);
}

static int32_t jni_call_int_method_a(void *environment, void *object,
                                     void *method, void *args)
{
    return jni_call_int_method_v(environment, object, method, args);
}

static int32_t call_boolean(void *object_value, void *method)
{
    const char *name = method_name(method);
    struct fake_object *object = as_object(object_value);
    if (strcmp(name, "isFinishing") == 0 || strcmp(name, "isDestroyed") == 0)
        return 0;
    if (strcmp(name, "equals") == 0)
        return 1;
    if (strcmp(name, "exists") == 0) {
        struct stat info;
        const char *path = file_path_text(object);
        int ok = stat(path, &info) == 0;
        printf("SG-JNI File.exists %s -> %d\n", path, ok);
        return ok ? 1 : 0;
    }
    if (strcmp(name, "mkdirs") == 0 || strcmp(name, "mkdir") == 0)
        return 1;
    if (strcmp(name, "isDirectory") == 0) {
        struct stat info;
        const char *path = file_path_text(object);
        return stat(path, &info) == 0 && S_ISDIR(info.st_mode) ? 1 : 0;
    }
    if (strcmp(name, "isFile") == 0) {
        struct stat info;
        const char *path = file_path_text(object);
        return stat(path, &info) == 0 && S_ISREG(info.st_mode) ? 1 : 0;
    }
    if (strcmp(name, "isAbsolute") == 0)
        return 1;
    if (strcmp(name, "getBoolean") == 0)
        return 1;
    if (strcmp(name, "commit") == 0) {
        prefs_save();
        return 1;
    }
    if (strcmp(name, "hasNext") == 0 || strcmp(name, "hasPrevious") == 0) {
        if (object != NULL && object->ints[1] == MOTION_RANGE_MAGIC)
            return (size_t)object->ints[0] < object->length ? 1 : 0;
        if (object != NULL && object->ints[1] == 0x5046)
            return object->ints[0] > 0 ? 1 : 0;
        return 0;
    }
    if (strcmp(name, "isEmpty") == 0)
        return 1;
    if (strcmp(name, "contains") == 0 || strcmp(name, "containsKey") == 0 ||
        strcmp(name, "containsValue") == 0)
        return 1;
    if (strcmp(name, "hasSystemFeature") == 0)
        return 1;
    if (strcmp(name, "isRequestReady") == 0)
        return 1;
    if (strcmp(name, "isConnected") == 0 || strcmp(name, "isAvailable") == 0 ||
        strcmp(name, "isConnectedOrConnecting") == 0) {
        if (object != NULL && object->name != NULL &&
            strstr(object->name, "googleplay") != NULL)
            return 0;
        return 1;
    }
    if (strcmp(name, "CheckIfLicensed") == 0 || strcmp(name, "getLicensed") == 0)
        return 1;
    if (strcmp(name, "isAlive") == 0) {
        if (is_fmod_device(object))
            return fmod_running;
        if (object != NULL && object->ints[1] != 0)
            return object->ints[2];
        return 0;
    }
    if (strcmp(name, "isRunning") == 0)
        return fmod_running ? 1 : 0;
    if (strcmp(name, "loadLibrary") == 0 || strcmp(name, "load") == 0 ||
        strcmp(name, "load0") == 0)
        return 1;
    printf("SG-JNI CallBoolean %s -> 0\n", name);
    return 0;
}

static int32_t jni_call_boolean_method(void *environment, void *object,
                                       void *method, ...)
{
    (void)environment;
    return call_boolean(object, method);
}

static int32_t jni_call_boolean_method_v(void *environment, void *object,
                                         void *method, void *args)
{
    (void)args;
    return jni_call_boolean_method(environment, object, method);
}

static int32_t jni_call_boolean_method_a(void *environment, void *object,
                                         void *method, void *args)
{
    (void)args;
    return jni_call_boolean_method(environment, object, method);
}

static int64_t jni_call_long_method(void *environment, void *object,
                                    void *method, ...)
{
    const char *name = method_name(method);
    struct fake_object *file = as_object(object);
    const char *path;
    struct stat info;

    (void)environment;
    if (strcmp(name, "length") == 0) {
        path = file_path_text(file);
        if (stat(path, &info) == 0) {
            printf("SG-JNI CallLong length %s -> %lld\n", path,
                   (long long)info.st_size);
            return (int64_t)info.st_size;
        }
        printf("SG-JNI CallLong length %s FAIL\n", path);
        return 0;
    }
    if (strcmp(name, "lastModified") == 0)
        return 1;
    if (strcmp(name, "currentTimeMillis") == 0)
        return (int64_t)time(NULL) * 1000;
    if (strcmp(name, "getEventTime") == 0 || strcmp(name, "getDownTime") == 0 ||
        strcmp(name, "getHistoricalEventTime") == 0) {
        /* Unity drops events whose timestamps do not advance, so report a
           millisecond monotonic clock rather than one-second time(). */
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
            return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        return (int64_t)time(NULL) * 1000;
    }
    printf("SG-JNI CallLong %s -> 0\n", name);
    return 0;
}

static int64_t jni_call_long_method_v(void *environment, void *object,
                                      void *method, void *args)
{
    (void)args;
    return jni_call_long_method(environment, object, method);
}

static int64_t jni_call_long_method_a(void *environment, void *object,
                                      void *method, void *args)
{
    (void)args;
    return jni_call_long_method(environment, object, method);
}

static float axis_dead(float value)
{
    return (value > -0.08f && value < 0.08f) ? 0.0f : value;
}

/* Reports a steady deflection on the view axis while the stick is centred, so a
   camera that turns on its own means the bind is live. Left in place but off by
   default now that it has shown slot 5 is not the view axis; set
   SG_AXIS_PROBE=1 to run it again. */
static int axis_probe_enabled(void)
{
    static int state = -1;
    if (state < 0) {
        const char *value = getenv("SG_AXIS_PROBE");
        state = (value != NULL && value[0] == '1') ? 1 : 0;
        printf("SG-PAD view axis probe %s\n", state != 0 ? "ON" : "off");
    }
    return state;
}

static float axis_value(int32_t axis)
{
    float source[PAD_SRC_COUNT];
    size_t slot;

    controls_init();
    source[PAD_SRC_NONE] = 0.0f;
    /* D-pad left/right on the presenter writes NFSMW_HDR_STICK_BIAS.
       0 is the original stick. The offset is applied after the noise
       deadzone so a small bias is not wiped back to zero. */
    {
        float shift = (float)stick_center_bias / 32767.0f;
        source[PAD_SRC_LX] = axis_dead(pad_axis[0] / 32767.0f) - shift;
        source[PAD_SRC_LY] = axis_dead(pad_axis[1] / 32767.0f);
    }
    source[PAD_SRC_RX] = axis_dead(pad_axis[2] / 32767.0f);
    source[PAD_SRC_RY] = axis_dead(pad_axis[3] / 32767.0f);
    /* SDL reports triggers as 0..32767, so the old -32768 bias made a released
       trigger read 0.5 and the game saw it half pressed all the time. */
    source[PAD_SRC_LT] = pad_axis[4] > 0.0f ? pad_axis[4] / 32767.0f : 0.0f;
    source[PAD_SRC_RT] = pad_axis[5] > 0.0f ? pad_axis[5] / 32767.0f : 0.0f;

    for (slot = 0U; slot < PAD_SLOTS; ++slot) {
        float value;
        if (slot_axis[slot] != axis)
            continue;
        value = pad_source_value(slot_source[slot], source);
        if (slot_negate[slot] != 0)
            value = -value;
        value -= pad_source_value(slot_source_neg[slot], source);
        if (slot == 6U && value == 0.0f && axis_probe_enabled() != 0)
            value = 0.35f;
        /* Exact 0 is read as full deflection: X becomes right, Y becomes
           up. A tiny nonzero stays analog and does not walk. */
        if ((axis == AMOTION_AXIS_X || axis == AMOTION_AXIS_Y) &&
            value == 0.0f)
            return 1.0e-20f;
        return value;
    }
    if (axis == AMOTION_AXIS_HAT_X) {
        if (prev_pad_buttons & (1u << 13))
            return -1.0f;
        if (prev_pad_buttons & (1u << 14))
            return 1.0f;
        return 0.0f;
    }
    if (axis == AMOTION_AXIS_HAT_Y) {
        if (prev_pad_buttons & (1u << 11))
            return -1.0f;
        if (prev_pad_buttons & (1u << 12))
            return 1.0f;
        return 0.0f;
    }
    return 0.0f;
}

static int motion_is_touch(const struct fake_object *object)
{
    return object != NULL && object_is_named(object, "MotionEvent") &&
           object->ints[3] == ASOURCE_TOUCHSCREEN;
}

static float motion_get_x(const struct fake_object *object)
{
    if (motion_is_touch(object))
        return look_touch_x;
    return axis_value(AMOTION_AXIS_X);
}

static float motion_get_y(const struct fake_object *object)
{
    if (motion_is_touch(object))
        return look_touch_y;
    return axis_value(AMOTION_AXIS_Y);
}

static float float_from_name(void *object, void *method, int32_t axis)
{
    const char *name = method_name(method);
    struct fake_object *event = as_object(object);
    static unsigned axis_logs;

    if (strcmp(name, "getAxisValue") == 0) {
        float value = axis_value(axis);
        static unsigned axis_hits;
        axis_logs += 1U;
        if (axis_logs <= 12U || axis_logs % 4000U == 0U) {
            printf("SG-JNI getAxisValue #%u axis=%d -> %.3f\n", axis_logs, axis,
                   value);
        }
        /* Budgeted per slot: a single shared cap kept getting spent on the
           first control touched, which repeatedly hid whether the others
           delivered anything at all. */
        if (value != 0.0f) {
            static unsigned char slot_hits[PAD_SLOTS];
            size_t slot;
            for (slot = 0U; slot < PAD_SLOTS; ++slot) {
                if (slot_axis[slot] != axis || slot_hits[slot] >= 6U)
                    continue;
                slot_hits[slot] += 1U;
                printf("SG-JNI getAxisValue LIVE slot%u axis=%d -> %.3f\n",
                       (unsigned)(slot + 1U), axis, (double)value);
                break;
            }
        }
        return value;
    }
    if (strcmp(name, "getX") == 0 || strcmp(name, "getRawX") == 0)
        return motion_get_x(event);
    if (strcmp(name, "getY") == 0 || strcmp(name, "getRawY") == 0)
        return motion_get_y(event);
    if (strcmp(name, "getMin") == 0)
        return -1.0f;
    if (strcmp(name, "getMax") == 0)
        return 1.0f;
    if (strcmp(name, "getRange") == 0)
        return 2.0f;
    if (strcmp(name, "getFlat") == 0)
        return 0.08f;
    if (strcmp(name, "getFuzz") == 0)
        return 0.0f;
    if (strcmp(name, "getPressure") == 0 || strcmp(name, "getSize") == 0 ||
        strcmp(name, "getToolMajor") == 0 || strcmp(name, "getTouchMajor") == 0)
        return 1.0f;
    return 0.0f;
}

/* Softfp puts the float in r0. A constant 0 is "mov r0, #0" and leaves s0
   holding the previous return (getMax is 1, full right), so a centred stick
   sprints. ±1 is computed in s0, which is why it only creeps. Copy r0 into
   s0 and return with both registers holding the same bits. */
static SG_JNI_SOFTFP float __attribute__((noinline, naked)) jni_return_float(float value)
{
    __asm__ __volatile__(
        "vmov s0, r0\n\t"
        "bx lr\n\t");
}

static SG_JNI_SOFTFP float jni_call_float_method(void *environment, void *object,
                                                 void *method, ...)
{
    va_list args;
    int32_t axis = 0;
    const char *name = method_name(method);

    (void)environment;
    if (strcmp(name, "getFloat") == 0 && is_prefs_object(object)) {
        void *key;
        va_start(args, method);
        key = va_arg(args, void *);
        va_end(args);
        return jni_return_float(prefs_get_float(key));
    }
    if (strcmp(name, "getAxisValue") == 0) {
        va_start(args, method);
        axis = va_arg(args, int32_t);
        va_end(args);
    }
    return jni_return_float(float_from_name(object, method, axis));
}

static SG_JNI_SOFTFP float jni_call_float_method_v(void *environment, void *object,
                                                   void *method, void *args)
{
    int32_t axis = 0;
    const char *name = method_name(method);

    (void)environment;
    if (strcmp(name, "getFloat") == 0 && is_prefs_object(object))
        return jni_return_float(prefs_get_float(args != NULL ? ((void **)args)[0] : NULL));
    if (strcmp(name, "getAxisValue") == 0 && args != NULL)
        axis = *(int32_t *)args;
    return jni_return_float(float_from_name(object, method, axis));
}

static SG_JNI_SOFTFP float jni_call_float_method_a(void *environment, void *object,
                                                   void *method, void *args)
{
    return jni_call_float_method_v(environment, object, method, args);
}

static int call_native(const char *name, char *error, size_t error_size);

static void jni_call_void_method(void *environment, void *object, void *method,
                                 ...)
{
    const char *name = method_name(method);
    struct fake_object *target = as_object(object);
    if (strcmp(name, "recycle") != 0 && strcmp(name, "setScreenSize") != 0) {
        printf("SG-JNI CallVoid %s\n", name);
    } else if (strcmp(name, "setScreenSize") == 0) {
        va_list args;
        int32_t sw, sh, fs;
        va_start(args, method);
        sw = va_arg(args, int32_t);
        sh = va_arg(args, int32_t);
        fs = va_arg(args, int32_t);
        va_end(args);
        printf("SG-JNI setScreenSize %d %d fs=%d\n", sw, sh, fs);
        if (sw < 640 || sh < 360) {
            static int pushed;
            if (pushed == 0) {
                char error[128];
                pushed = 1;
                printf("SG-JNI setScreenSize reject %dx%d, resize to panel\n",
                       sw, sh);
                if (call_native("nativeResize", error, sizeof error) != 0)
                    printf("SG-JNI %s\n", error);
            }
        }
    } else {
        static unsigned recycle_logs;
        if (recycle_logs < 4U) {
            printf("SG-JNI CallVoid recycle\n");
            recycle_logs += 1U;
        }
    }
    if (strcmp(name, "<init>") == 0 && is_www_ctor(method)) {
        va_list args;
        int32_t handle;
        void *url;
        va_start(args, method);
        handle = va_arg(args, int32_t);
        url = va_arg(args, void *);
        va_end(args);
        apply_www_ctor(target, handle, url);
        return;
    }
    if (strcmp(name, "start") == 0) {
        if (target != NULL && target->ints[1] == 1) {
            complete_www(target);
            return;
        }
        if (is_fmod_device(target) ||
            (object_is_named(target, "Thread") &&
             find_native(NULL, "fmodGetInfo") != NULL && fmod_running == 0)) {
            fmod_audio_device_start(is_fmod_device(target) ? target : NULL);
            return;
        }
    }
    if ((strcmp(name, "stop") == 0 || strcmp(name, "close") == 0) &&
        is_fmod_device(target)) {
        fmod_audio_device_stop();
        return;
    }
    if (strcmp(name, "join") == 0 && target != NULL && target->ints[1] != 0)
        return;
    if (strcmp(name, "runOnUiThread") == 0) {
        va_list args;
        void *runnable;
        struct fake_object *job;

        va_start(args, method);
        runnable = va_arg(args, void *);
        va_end(args);
        job = as_object(runnable);
        printf("SG-JNI runOnUiThread runnable=%p job=%s\n", runnable,
               job != NULL && job->name != NULL ? job->name : "skip");
        if (job != NULL)
            jni_call_void_method(environment, job, intern_method("run", "()V"));
        return;
    }
    if (strcmp(name, "apply") == 0 ||
        strcmp(name, "startActivity") == 0 || strcmp(name, "load") == 0 ||
        strcmp(name, "loadLibrary") == 0 || strcmp(name, "load0") == 0)
        return;
    (void)environment;
}

static void jni_call_void_method_v(void *environment, void *object,
                                   void *method, void *args)
{
    const char *name = method_name(method);
    if (is_www_ctor(method) && args != NULL) {
        apply_www_ctor_args(as_object(object), args);
        return;
    }
    if (strcmp(name, "runOnUiThread") == 0 && args != NULL) {
        union {
            int32_t i;
            void *l;
            int64_t j;
            double d;
        } *jv = args;
        struct fake_object *job = as_object(jv[0].l);
        printf("SG-JNI runOnUiThreadV runnable=%p job=%s\n", jv[0].l,
               job != NULL && job->name != NULL ? job->name : "skip");
        if (job != NULL)
            jni_call_void_method(environment, job, intern_method("run", "()V"));
        return;
    }
    jni_call_void_method(environment, object, method);
}

static void jni_call_void_method_a(void *environment, void *object,
                                   void *method, void *args)
{
    jni_call_void_method_v(environment, object, method, args);
}

static void *jni_get_object_field(void *environment, void *object,
                                  void *field)
{
    const char *name = method_name(field);
    char path[PATH_CAPACITY];
    (void)environment;
    (void)object;
    printf("SG-JNI GetObjectField %s\n", name);
    if (strcmp(name, "currentActivity") == 0)
        return activity;
    if (strcmp(name, "sourceDir") == 0 || strcmp(name, "publicSourceDir") == 0) {
        join_path(path, sizeof(path), root_path(), apk_path());
        return new_string(path);
    }
    if (strcmp(name, "dataDir") == 0)
        return new_string(root_path());
    if (strcmp(name, "nativeLibraryDir") == 0) {
        join_path(path, sizeof(path), root_path(), "gamefiles/android-libs");
        return new_string(path);
    }
    if (strcmp(name, "packageName") == 0)
        return new_string(package_name());
    /* Never report R800i / Sony Ericsson here: DetectXperiaPlayModel() would
       set IsXperiaPlay and route the camera through UpdateXperiaView, which
       reads the Xperia Play touchpad. Stick axes are ignored in that mode. */
    if (strcmp(name, "MODEL") == 0 || strcmp(name, "DEVICE") == 0 ||
        strcmp(name, "PRODUCT") == 0)
        return new_string("TrimUI Smart Pro");
    if (strcmp(name, "MANUFACTURER") == 0 || strcmp(name, "BRAND") == 0)
        return new_string("trimui");
    if (strcmp(name, "HARDWARE") == 0)
        return new_string("sun50iw10");
    if (strcmp(name, "CPU_ABI") == 0)
        return new_string("armeabi-v7a");
    if (strcmp(name, "FINGERPRINT") == 0)
        return new_string("trimui/tsp/tsp:4.1.2/JZO54K/1:user/release-keys");
    if (strcmp(name, "RELEASE") == 0)
        return new_string("4.1.2");
    if (strcmp(name, "INCREMENTAL") == 0)
        return new_string("1");
    if (strcmp(name, "CODENAME") == 0)
        return new_string("REL");
    if (strcmp(name, "ID") == 0)
        return new_string("FRG83");
    if (strcmp(name, "SDK") == 0)
        return new_string("8");
    if (strcmp(name, "versionName") == 0)
        return new_string(env_or("SG_VERSION_NAME", "1.1.4"));
    if (strcmp(name, "BASE64_PUBLIC_KEY") == 0)
        return new_string(
            "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAq3SHmGpt8uCOQC6vuCJz"
            "75BUMqCA+snV/JtEMchE88IGGnTdy3Ve3KZhiTtpf9gQdvHQuz0XAgCpmxO/aA8m"
            "IUCRgOIiJWULakK8AfsGxHnGmyG2y05SnwI6vFx9zEK3cm+lIhQUTxmsCFnnSoGt"
            "ZiGSgGjRbXteusf0dw5iS3UK4DngU7khB18/e4VCOPCy76JVYwhgFbH6Sef2CkEG"
            "xBLCdqOGGqm+8JOuSBoQqfmuhBU0Ei65jl69JJ9ho3OAqGkTv9DnkLqunaO41OkA"
            "9AR3eHqbOREbpaanD6wg2Gu0YwWq6bZfTrEFjKqtnxbj/U9lLGehU6CixAzIFxJf"
            "yQIDAQAB");
    if (strcmp(name, "SALT") == 0) {
        static const unsigned char salt_bytes[20] = {
            1, 42, 244, 255, 54, 98, 156, 244, 43, 2,
            248, 252, 9, 5, 150, 149, 223, 45, 255, 84
        };
        struct fake_object *salt = new_object(FAKE_ARRAY, "[B");
        salt->length = 20U;
        salt->text = calloc(20U, 1U);
        if (salt->text == NULL)
            abort();
        memcpy(salt->text, salt_bytes, 20U);
        return salt;
    }
    if (strcmp(name, "TAGS") == 0)
        return new_string("release-keys");
    if (strcmp(name, "SCREEN_OFF_TIMEOUT") == 0)
        return new_string("screen_off_timeout");
    if (strcmp(name, "BOARD") == 0)
        return new_string("sun50iw10");
    if (strcmp(name, "MEDIA_MOUNTED") == 0)
        return new_string("mounted");
    if (strcmp(name, "MEDIA_MOUNTED_READ_ONLY") == 0)
        return new_string("mounted_ro");
    if (strcmp(name, "MEDIA_REMOVED") == 0)
        return new_string("removed");
    if (strcmp(name, "MEDIA_UNMOUNTED") == 0)
        return new_string("unmounted");
    if (strcmp(name, "MEDIA_BAD_REMOVAL") == 0)
        return new_string("bad_removal");
    if (strcmp(name, "MEDIA_CHECKING") == 0)
        return new_string("checking");
    if (strcmp(name, "MEDIA_EJECTING") == 0)
        return new_string("ejecting");
    if (strcmp(name, "MEDIA_NOFS") == 0)
        return new_string("nofs");
    if (strcmp(name, "MEDIA_SHARED") == 0)
        return new_string("shared");
    if (strcmp(name, "MEDIA_UNKNOWN") == 0)
        return new_string("unknown");
    if (strcmp(name, "WRITE_EXTERNAL_STORAGE") == 0)
        return new_string("android.permission.WRITE_EXTERNAL_STORAGE");
    if (strcmp(name, "READ_EXTERNAL_STORAGE") == 0)
        return new_string("android.permission.READ_EXTERNAL_STORAGE");
    if (strcmp(name, "READ_PHONE_STATE") == 0)
        return new_string("android.permission.READ_PHONE_STATE");
    if (strcmp(name, "GET_ACCOUNTS") == 0)
        return new_string("android.permission.GET_ACCOUNTS");
    if (strcmp(name, "INTERNET") == 0)
        return new_string("android.permission.INTERNET");
    if (strcmp(name, "ACCESS_NETWORK_STATE") == 0)
        return new_string("android.permission.ACCESS_NETWORK_STATE");
    if (strcmp(name, "WAKE_LOCK") == 0)
        return new_string("android.permission.WAKE_LOCK");
    if (strcmp(name, "TELEPHONY_SERVICE") == 0)
        return new_string("phone");
    if (strcmp(name, "DISPLAY_SERVICE") == 0)
        return new_string("display");
    if (strcmp(name, "CONNECTIVITY_SERVICE") == 0)
        return new_string("connectivity");
    if (strcmp(name, "WINDOW_SERVICE") == 0)
        return new_string("window");
    if (strcmp(name, "AUDIO_SERVICE") == 0)
        return new_string("audio");
    if (strcmp(name, "INPUT_SERVICE") == 0)
        return new_string("input");
    if (strcmp(name, "STORAGE_SERVICE") == 0)
        return new_string("storage");
    if (strcmp(name, "SENSOR_SERVICE") == 0)
        return new_string("sensor");
    if (strcmp(name, "ACTIVITY_SERVICE") == 0)
        return new_string("activity");
    if (strcmp(name, "WIFI_SERVICE") == 0)
        return new_string("wifi");
    if (strcmp(name, "LOCATION_SERVICE") == 0)
        return new_string("location");
    if (strcmp(name, "NOTIFICATION_SERVICE") == 0)
        return new_string("notification");
    if (strcmp(name, "INPUT_METHOD_SERVICE") == 0)
        return new_string("input_method");
    if (strcmp(name, "DOWNLOAD_SERVICE") == 0)
        return new_string("download");
    if (strcmp(name, "FEATURE_AUDIO_LOW_LATENCY") == 0)
        return new_string("android.hardware.audio.low_latency");
    if (strncmp(name, "DIRECTORY_", 10) == 0)
        return new_string(name + 10);
    if (strcmp(name, "separator") == 0 || strcmp(name, "pathSeparator") == 0)
        return new_string("/");
    printf("SG-JNI GetObjectField %s -> %s\n", name,
           strcmp(name, "MEDIA_MOUNTED") == 0 ? "mounted" : "");
    return new_string("");
}

static int32_t jni_get_int_field(void *environment, void *object, void *field)
{
    const char *name = method_name(field);
    (void)environment;
    (void)object;
    printf("SG-JNI GetIntField %s\n", name);
    if (strcmp(name, "SDK_INT") == 0) {
        printf("SG-JNI SDK_INT -> %d\n", ANDROID_SDK_INT);
        return ANDROID_SDK_INT;
    }
    if (strcmp(name, "widthPixels") == 0 || strcmp(name, "width") == 0)
        return nfsmw_screen_width();
    if (strcmp(name, "heightPixels") == 0 || strcmp(name, "height") == 0)
        return nfsmw_screen_height();
    if (strcmp(name, "densityDpi") == 0)
        return 240;
    if (strcmp(name, "orientation") == 0)
        return 2;
    if (strcmp(name, "flags") == 0)
        return 0;
    if (strcmp(name, "BIND_AUTO_CREATE") == 0)
        return 1;
    if (strcmp(name, "PERMISSION_GRANTED") == 0)
        return 0;
    if (strcmp(name, "PERMISSION_DENIED") == 0)
        return -1;
    if (strcmp(name, "GET_META_DATA") == 0)
        return 128;
    if (strcmp(name, "SCREEN_ORIENTATION_LANDSCAPE") == 0)
        return 0;
    if (strcmp(name, "SCREEN_ORIENTATION_PORTRAIT") == 0)
        return 1;
    if (strcmp(name, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE") == 0)
        return 8;
    if (strcmp(name, "SCREEN_ORIENTATION_REVERSE_PORTRAIT") == 0)
        return 9;
    if (strcmp(name, "SCREEN_ORIENTATION_SENSOR") == 0)
        return 4;
    if (strcmp(name, "SCREEN_ORIENTATION_SENSOR_LANDSCAPE") == 0)
        return 6;
    if (strcmp(name, "SCREEN_ORIENTATION_SENSOR_PORTRAIT") == 0)
        return 7;
    if (strcmp(name, "SCREEN_ORIENTATION_FULL_SENSOR") == 0)
        return 10;
    if (strcmp(name, "SCREEN_ORIENTATION_UNSPECIFIED") == 0)
        return -1;
    if (strcmp(name, "MODE_PRIVATE") == 0)
        return 0;
    if (strcmp(name, "targetSdkVersion") == 0)
        return ANDROID_TARGET_SDK;
    if (strcmp(name, "versionCode") == 0) {
        const char *code = getenv("SG_VERSION_CODE");
        if (code != NULL && code[0] != '\0')
            return (int32_t)strtol(code, NULL, 10);
        return 101040;
    }
    return 0;
}

static int32_t jni_get_boolean_field(void *environment, void *object,
                                     void *field)
{
    const char *name = method_name(field);
    (void)environment;
    (void)object;
    printf("SG-JNI GetBooleanField %s -> 1\n", name);
    return 1;
}

static int64_t jni_get_long_field(void *environment, void *object, void *field)
{
    (void)environment;
    (void)object;
    (void)field;
    return 0;
}

static SG_JNI_SOFTFP float jni_get_float_field(void *environment, void *object, void *field)
{
    const char *name = method_name(field);
    (void)environment;
    (void)object;
    if (strcmp(name, "density") == 0 || strcmp(name, "scaledDensity") == 0)
        return jni_return_float(1.5f);
    if (strcmp(name, "xdpi") == 0 || strcmp(name, "ydpi") == 0)
        return jni_return_float(240.0f);
    return jni_return_float(0.0f);
}

static int32_t jni_get_char_field(void *environment, void *object, void *field)
{
    const char *name = method_name(field);
    (void)environment;
    (void)object;
    if (strcmp(name, "separatorChar") == 0 ||
        strcmp(name, "pathSeparatorChar") == 0)
        return (int32_t)'/';
    return 0;
}

static void *jni_new_string_utf(void *environment, const char *text)
{
    (void)environment;
    if (text != NULL && (strstr(text, "://") != NULL || strstr(text, ".tmp") != NULL ||
                         strstr(text, ".obb") != NULL || text[0] == '/'))
        printf("SG-JNI NewStringUTF '%s'\n", text);
    return new_string(text);
}

static void *jni_new_string(void *environment, const uint16_t *chars,
                            int32_t length)
{
    char *utf8;
    int32_t index;
    void *result;
    (void)environment;
    if (length < 0)
        length = 0;
    utf8 = calloc((size_t)length + 1U, 1U);
    if (utf8 == NULL)
        abort();
    if (chars != NULL) {
        for (index = 0; index < length; ++index)
            utf8[index] = chars[index] < 128U ? (char)chars[index] : '?';
    }
    result = new_string(utf8);
    free(utf8);
    return result;
}

static int32_t jni_get_string_length(void *environment, void *value)
{
    struct fake_object *object = as_object(value);
    (void)environment;
    if (object != NULL && object->utf16 != NULL)
        return (int32_t)object->utf16_length;
    return object != NULL && object->text != NULL ?
           (int32_t)strlen(object->text) : 0;
}

static const uint16_t *jni_get_string_chars(void *environment, void *value,
                                            unsigned char *is_copy)
{
    struct fake_object *object = as_object(value);
    (void)environment;
    if (is_copy != NULL)
        *is_copy = 1;
    printf("SG-JNI GetStringChars len=%d\n",
           object != NULL && object->text != NULL ?
           (int)strlen(object->text) : 0);
    return ensure_utf16(object);
}

static int32_t jni_get_string_utf_length(void *environment, void *value)
{
    struct fake_object *object = as_object(value);
    (void)environment;
    return object != NULL && object->text != NULL ?
           (int32_t)strlen(object->text) : 0;
}

static const char *jni_get_string_utf_chars(void *environment, void *value,
                                            unsigned char *is_copy)
{
    struct fake_object *object = as_object(value);
    const char *text;
    (void)environment;
    if (is_copy != NULL)
        *is_copy = 0;
    text = object != NULL && object->text != NULL ? object->text : "";
    printf("SG-JNI GetStringUTFChars '%s'\n", text);
    return text;
}

static void jni_get_string_region(void *environment, void *value, int32_t start,
                                  int32_t length, uint16_t *buffer)
{
    const uint16_t *chars = ensure_utf16(as_object(value));
    struct fake_object *object = as_object(value);
    size_t available = object != NULL ? object->utf16_length : 0U;
    (void)environment;
    if (buffer == NULL || start < 0 || length <= 0)
        return;
    if ((size_t)start >= available)
        return;
    if ((size_t)start + (size_t)length > available)
        length = (int32_t)(available - (size_t)start);
    memcpy(buffer, chars + start, (size_t)length * sizeof(*buffer));
}

static void jni_get_string_utf_region(void *environment, void *value,
                                      int32_t start, int32_t length,
                                      char *buffer)
{
    struct fake_object *object = as_object(value);
    const char *text = object != NULL && object->text != NULL ? object->text : "";
    size_t available = strlen(text);
    (void)environment;
    if (buffer == NULL || start < 0 || length <= 0)
        return;
    if ((size_t)start >= available) {
        buffer[0] = '\0';
        return;
    }
    if ((size_t)start + (size_t)length > available)
        length = (int32_t)(available - (size_t)start);
    memcpy(buffer, text + start, (size_t)length);
    buffer[length] = '\0';
}

static int32_t jni_get_array_length(void *environment, void *value)
{
    struct fake_object *object = as_object(value);
    (void)environment;
    return object != NULL ? (int32_t)object->length : 0;
}

static void *jni_new_object_array(void *environment, int32_t length,
                                  void *element_class, void *initial)
{
    struct fake_object *array = new_object(FAKE_ARRAY, "java/lang/Object");
    (void)environment;
    (void)element_class;
    if (length < 0)
        length = 0;
    array->length = (size_t)length;
    if (length > 0) {
        array->elements = calloc((size_t)length, sizeof(*array->elements));
        if (array->elements == NULL)
            abort();
        if (initial != NULL) {
            int32_t index;
            for (index = 0; index < length; ++index)
                array->elements[index] = initial;
        }
    }
    return array;
}

static void *jni_get_object_array_element(void *environment, void *value,
                                          int32_t index)
{
    struct fake_object *array = as_object(value);
    (void)environment;
    if (array == NULL || array->elements == NULL || index < 0 ||
        (size_t)index >= array->length)
        return NULL;
    return array->elements[index];
}

static void jni_set_object_array_element(void *environment, void *value,
                                         int32_t index, void *element)
{
    struct fake_object *array = as_object(value);
    (void)environment;
    if (array == NULL || array->elements == NULL || index < 0 ||
        (size_t)index >= array->length)
        return;
    array->elements[index] = element;
}

static void *jni_new_byte_array(void *environment, int32_t length)
{
    struct fake_object *array = new_object(FAKE_ARRAY, "[B");
    (void)environment;
    if (length < 0)
        length = 0;
    array->length = (size_t)length;
    return array;
}

static void *jni_new_int_array(void *environment, int32_t length)
{
    struct fake_object *array = new_object(FAKE_ARRAY, "[I");
    (void)environment;
    if (length < 0)
        length = 0;
    array->length = (size_t)length;
    return array;
}

static uint8_t dummy_bytes[64];

static void *jni_get_byte_array_elements(void *environment, void *array,
                                         uint8_t *is_copy)
{
    struct fake_object *object = as_object(array);
    (void)environment;
    if (is_copy != NULL)
        *is_copy = 0;
    if (object != NULL && object->text != NULL)
        return object->text;
    return dummy_bytes;
}

static void jni_get_byte_array_region(void *environment, void *array,
                                      int32_t start, int32_t length,
                                      void *buffer)
{
    struct fake_object *object = as_object(array);
    const uint8_t *src = dummy_bytes;
    size_t avail = sizeof(dummy_bytes);
    (void)environment;
    if (buffer == NULL || length <= 0)
        return;
    if (object != NULL && object->text != NULL) {
        src = (const uint8_t *)object->text;
        avail = object->length > 0U ? object->length : strlen(object->text);
    }
    if (start < 0)
        start = 0;
    if ((size_t)start >= avail)
        return;
    if ((size_t)start + (size_t)length > avail)
        length = (int32_t)(avail - (size_t)start);
    memcpy(buffer, src + start, (size_t)length);
}

static void jni_set_byte_array_region(void *environment, void *array,
                                      int32_t start, int32_t length,
                                      void *buffer)
{
    (void)environment;
    (void)array;
    (void)start;
    (void)length;
    (void)buffer;
}

static void *jni_get_int_array_elements(void *environment, void *array,
                                        uint8_t *is_copy)
{
    struct fake_object *object = as_object(array);
    (void)environment;
    if (is_copy != NULL)
        *is_copy = 0;
    if (object != NULL)
        return object->ints;
    return dummy_bytes;
}

static void jni_release_array_elements(void *environment, void *array,
                                       void *elements, int32_t mode)
{
    (void)environment;
    (void)array;
    (void)elements;
    (void)mode;
}

static int32_t jni_register_natives(void *environment, void *class_value,
                                    void *methods_value, int32_t count)
{
    struct fake_object *class_object = as_object(class_value);
    const char *class_name =
        class_object != NULL ? class_object->name : "unknown";
    struct {
        const char *name;
        const char *signature;
        void *fn;
    } *items = methods_value;
    int32_t index;

    (void)environment;
    printf("SG-JNI RegisterNatives %s count=%d\n", class_name, count);
    if (items == NULL || count <= 0)
        return JNI_OK_VALUE;
    for (index = 0; index < count && native_count < NATIVE_CAPACITY; ++index) {
        snprintf(natives[native_count].class_name,
                 sizeof(natives[native_count].class_name), "%s", class_name);
        snprintf(natives[native_count].name, sizeof(natives[native_count].name),
                 "%s", items[index].name != NULL ? items[index].name : "");
        snprintf(natives[native_count].signature,
                 sizeof(natives[native_count].signature), "%s",
                 items[index].signature != NULL ? items[index].signature : "");
        natives[native_count].fn = items[index].fn;
        printf("SG-JNI native %s %s %s\n", class_name,
               natives[native_count].name, natives[native_count].signature);
        native_count += 1U;
    }
    return JNI_OK_VALUE;
}

static int32_t jni_get_java_vm(void *environment, void **vm)
{
    (void)environment;
    if (vm == NULL)
        return -1;
    *vm = &jvm_handle;
    return JNI_OK_VALUE;
}

static void *jni_new_direct_byte_buffer(void *environment, void *address,
                                        int64_t capacity)
{
    struct fake_object *buffer = new_object(FAKE_DIRECT, "java/nio/ByteBuffer");
    (void)environment;
    buffer->text = (char *)address;
    buffer->length = capacity > 0 ? (size_t)capacity : 0U;
    return buffer;
}

static void *jni_get_direct_buffer_address(void *environment, void *value)
{
    struct fake_object *buffer = as_object(value);
    (void)environment;
    return buffer != NULL ? buffer->text : NULL;
}

static int64_t jni_get_direct_buffer_capacity(void *environment, void *value)
{
    struct fake_object *buffer = as_object(value);
    (void)environment;
    return buffer != NULL ? (int64_t)buffer->length : -1;
}

/*
 * Unity 5.3 org.fmod.FMODAudioDevice.run(): fmodGetInfo indices are
 * 0=sampleRate, 1=dspBufferLength, 2=dspNumBuffers, 3=mixerRunning.
 * Direct ByteBuffer is dspLen * 2 * 2 bytes (16-bit stereo). We replace
 * AudioTrack.write with SDL_QueueAudio in this process (no GLES window).
 */
enum {
    FMOD_INFO_SAMPLERATE = 0,
    FMOD_INFO_DSPBUFFERLENGTH = 1,
    FMOD_INFO_DSPNUMBUFFERS = 2,
    FMOD_INFO_MIXERRUNNING = 3,
    FMOD_NUMCHANNELS = 2
};

static int32_t fmod_get_info(void *device, int32_t index)
{
    struct native_method *native = find_native("FMODAudioDevice", "fmodGetInfo");
    typedef int32_t (*fn)(void *, void *, int32_t);

    if (native == NULL)
        native = find_native(NULL, "fmodGetInfo");
    if (native == NULL || native->fn == NULL)
        return 0;
    return ((fn)native->fn)(&jni_handle, device, index);
}

static int32_t fmod_process(void *device, void *buffer)
{
    struct native_method *native = find_native("FMODAudioDevice", "fmodProcess");
    typedef int32_t (*fn)(void *, void *, void *);

    if (native == NULL)
        native = find_native(NULL, "fmodProcess");
    if (native == NULL || native->fn == NULL)
        return 0;
    return ((fn)native->fn)(&jni_handle, device, buffer);
}

static void *fmod_audio_thread(void *arg)
{
    struct fake_object *device = arg;
    int rate = 0;
    int dsp_len = 0;
    int dsp_bufs = 0;
    unsigned bytes;
    void *pcm;
    struct fake_object *buffer;
    unsigned peak_logs = 0;

    printf("SG-FMOD audio thread start\n");
    {
        struct native_method *init_jni =
            find_native("FMODAudioDevice", "fmodInitJni");
        typedef int32_t (*init_fn)(void *, void *);

        if (init_jni == NULL)
            init_jni = find_native(NULL, "fmodInitJni");
        if (init_jni != NULL && init_jni->fn != NULL) {
            int32_t rc = ((init_fn)init_jni->fn)(&jni_handle, device);
            printf("SG-FMOD fmodInitJni -> %d\n", rc);
        }
    }
    while (fmod_running && request_exit == 0) {
        rate = fmod_get_info(device, FMOD_INFO_SAMPLERATE);
        if (rate > 0)
            break;
        usleep(100000);
    }
    dsp_len = fmod_get_info(device, FMOD_INFO_DSPBUFFERLENGTH);
    dsp_bufs = fmod_get_info(device, FMOD_INFO_DSPNUMBUFFERS);
    if (dsp_len <= 0)
        dsp_len = 1024;
    if (dsp_bufs <= 0)
        dsp_bufs = 4;
    printf("SG-FMOD info rate=%d dspLen=%d dspBufs=%d mixer=%d\n", rate, dsp_len,
           dsp_bufs, fmod_get_info(device, FMOD_INFO_MIXERRUNNING));
    if (rate < 8000 || rate > 96000)
        rate = 44100;
    bytes = (unsigned)dsp_len * 2U * (unsigned)FMOD_NUMCHANNELS;
    if (bytes < 256U || bytes > 65536U)
        bytes = 4096U;
    pcm = calloc(1, bytes);
    if (pcm == NULL) {
        printf("SG-FMOD FAIL pcm alloc\n");
        fmod_running = 0;
        return NULL;
    }
    buffer = jni_new_direct_byte_buffer(&jni_handle, pcm, (int64_t)bytes);
    if (nfsmw_platform_audio_ensure(rate, FMOD_NUMCHANNELS) != 0 &&
        nfsmw_platform_audio_ensure(44100, FMOD_NUMCHANNELS) != 0) {
        printf("SG-FMOD FAIL SDL audio\n");
        fmod_running = 0;
        return NULL;
    }
    while (fmod_running && request_exit == 0) {
        int mixer = fmod_get_info(device, FMOD_INFO_MIXERRUNNING);
        unsigned queued;
        unsigned limit;

        if (mixer != 1) {
            usleep(10000);
            continue;
        }
        queued = nfsmw_platform_runtime_audio_queued();
        limit = (unsigned)rate * (unsigned)FMOD_NUMCHANNELS * 2U / 4U;
        if (queued > limit) {
            usleep(4000);
            continue;
        }
        fmod_process(device, buffer);
        if (peak_logs < 8U) {
            const int16_t *samples = pcm;
            unsigned i;
            int peak = 0;

            for (i = 0; i < bytes / 2U; i++) {
                int v = samples[i] < 0 ? -samples[i] : samples[i];
                if (v > peak)
                    peak = v;
            }
            printf("SG-FMOD process peak=%d queued=%u\n", peak, queued);
            peak_logs += 1U;
        }
        (void)nfsmw_platform_runtime_audio_queue(pcm, bytes);
    }
    printf("SG-FMOD audio thread stop\n");
    return NULL;
}

static void fmod_audio_device_start(struct fake_object *device)
{
    pthread_t thread;

    if (fmod_running)
        return;
    fmod_running = 1;
    if (device != NULL)
        device->ints[2] = 1;
    if (pthread_create(&thread, NULL, fmod_audio_thread, device) != 0) {
        fmod_running = 0;
        printf("SG-FMOD FAIL pthread_create\n");
        return;
    }
    pthread_detach(thread);
    printf("SG-FMOD AudioDevice.start\n");
}

static void fmod_audio_device_stop(void)
{
    fmod_running = 0;
}

static int32_t jvm_attach(void *vm, void **environment, void *arguments)
{
    (void)vm;
    (void)arguments;
    if (environment == NULL)
        return -1;
    *environment = &jni_handle;
    return JNI_OK_VALUE;
}

static int32_t jvm_detach(void *vm)
{
    (void)vm;
    return JNI_OK_VALUE;
}

static int32_t jvm_get_env(void *vm, void **environment, int32_t version)
{
    (void)vm;
    (void)version;
    if (environment == NULL)
        return -1;
    *environment = &jni_handle;
    return JNI_OK_VALUE;
}

static void initialize_tables(void)
{
    size_t index;
    uintptr_t unknown = FUNCTION_VALUE(jni_unknown);
#define JNI_SET(slot, function) jni_table[(slot)] = FUNCTION_VALUE(function)
    if (tables_ready)
        return;
    for (index = 0U; index < JNI_TABLE_SLOTS; ++index)
        jni_table[index] = unknown;
    for (index = 0U; index < JVM_TABLE_SLOTS; ++index)
        jvm_table[index] = unknown;
    JNI_SET(JNI_GET_VERSION, jni_get_version);
    JNI_SET(JNI_FIND_CLASS, jni_find_class);
    JNI_SET(JNI_FROM_REFLECTED_METHOD, jni_from_reflected_method);
    JNI_SET(JNI_FROM_REFLECTED_FIELD, jni_from_reflected_field);
    JNI_SET(JNI_TO_REFLECTED_METHOD, jni_to_reflected_method);
    JNI_SET(JNI_TO_REFLECTED_FIELD, jni_to_reflected_field);
    JNI_SET(JNI_GET_SUPERCLASS, jni_get_superclass);
    JNI_SET(JNI_IS_ASSIGNABLE_FROM, jni_is_assignable_from);
    JNI_SET(JNI_EXCEPTION_OCCURRED, jni_no_exception);
    JNI_SET(JNI_EXCEPTION_DESCRIBE, jni_void_noop);
    JNI_SET(JNI_EXCEPTION_CLEAR, jni_void_noop);
    JNI_SET(JNI_PUSH_LOCAL_FRAME, jni_int_ok);
    JNI_SET(JNI_POP_LOCAL_FRAME, jni_pop_local_frame);
    JNI_SET(JNI_NEW_GLOBAL_REF, jni_new_global_ref);
    JNI_SET(JNI_DELETE_GLOBAL_REF, jni_void_noop);
    JNI_SET(JNI_DELETE_LOCAL_REF, jni_void_noop);
    JNI_SET(JNI_IS_SAME_OBJECT, jni_is_same_object);
    JNI_SET(JNI_NEW_LOCAL_REF, jni_new_global_ref);
    JNI_SET(JNI_ENSURE_LOCAL_CAPACITY, jni_int_ok);
    JNI_SET(JNI_ALLOC_OBJECT, jni_alloc_object);
    JNI_SET(JNI_NEW_OBJECT, jni_new_object);
    JNI_SET(JNI_NEW_OBJECT_V, jni_new_object_v);
    JNI_SET(JNI_NEW_OBJECT_A, jni_new_object_a);
    JNI_SET(JNI_GET_OBJECT_CLASS, jni_get_object_class);
    JNI_SET(JNI_IS_INSTANCE_OF, jni_is_instance_of);
    JNI_SET(JNI_GET_METHOD_ID, jni_get_method_id);
    JNI_SET(JNI_CALL_OBJECT_METHOD, jni_call_object_method);
    JNI_SET(JNI_CALL_OBJECT_METHOD_V, jni_call_object_method_v);
    JNI_SET(JNI_CALL_OBJECT_METHOD_A, jni_call_object_method_a);
    JNI_SET(JNI_CALL_BOOLEAN_METHOD, jni_call_boolean_method);
    JNI_SET(JNI_CALL_BOOLEAN_METHOD_V, jni_call_boolean_method_v);
    JNI_SET(JNI_CALL_BOOLEAN_METHOD_A, jni_call_boolean_method_a);
    JNI_SET(JNI_CALL_INT_METHOD, jni_call_int_method);
    JNI_SET(JNI_CALL_INT_METHOD_V, jni_call_int_method_v);
    JNI_SET(JNI_CALL_INT_METHOD_A, jni_call_int_method_a);
    JNI_SET(JNI_CALL_LONG_METHOD, jni_call_long_method);
    JNI_SET(JNI_CALL_LONG_METHOD_V, jni_call_long_method_v);
    JNI_SET(JNI_CALL_LONG_METHOD_A, jni_call_long_method_a);
    JNI_SET(JNI_CALL_FLOAT_METHOD, jni_call_float_method);
    JNI_SET(JNI_CALL_FLOAT_METHOD_V, jni_call_float_method_v);
    JNI_SET(JNI_CALL_FLOAT_METHOD_A, jni_call_float_method_a);
    JNI_SET(JNI_CALL_DOUBLE_METHOD, jni_call_float_method);
    JNI_SET(JNI_CALL_DOUBLE_METHOD_V, jni_call_float_method_v);
    JNI_SET(40, jni_call_int_method);
    JNI_SET(41, jni_call_int_method_v);
    JNI_SET(43, jni_call_int_method);
    JNI_SET(44, jni_call_int_method_v);
    JNI_SET(46, jni_call_int_method);
    JNI_SET(47, jni_call_int_method_v);
    JNI_SET(JNI_CALL_VOID_METHOD, jni_call_void_method);
    JNI_SET(JNI_CALL_VOID_METHOD_V, jni_call_void_method_v);
    JNI_SET(JNI_CALL_VOID_METHOD_A, jni_call_void_method_a);
    /* CallNonvirtual* 64-93: same handlers as Call*. */
    JNI_SET(64, jni_call_object_method);
    JNI_SET(65, jni_call_object_method_v);
    JNI_SET(66, jni_call_object_method_a);
    JNI_SET(67, jni_call_boolean_method);
    JNI_SET(68, jni_call_boolean_method_v);
    JNI_SET(69, jni_call_boolean_method_a);
    JNI_SET(70, jni_call_int_method);
    JNI_SET(71, jni_call_int_method_v);
    JNI_SET(73, jni_call_int_method);
    JNI_SET(74, jni_call_int_method_v);
    JNI_SET(76, jni_call_int_method);
    JNI_SET(77, jni_call_int_method_v);
    JNI_SET(79, jni_call_int_method);
    JNI_SET(80, jni_call_int_method_v);
    JNI_SET(82, jni_call_long_method);
    JNI_SET(83, jni_call_long_method_v);
    JNI_SET(85, jni_call_float_method);
    JNI_SET(86, jni_call_float_method_v);
    JNI_SET(87, jni_call_float_method_a);
    JNI_SET(88, jni_call_float_method);
    JNI_SET(89, jni_call_float_method_v);
    JNI_SET(91, jni_call_void_method);
    JNI_SET(92, jni_call_void_method_v);
    JNI_SET(93, jni_call_void_method_a);
    JNI_SET(JNI_GET_FIELD_ID, jni_get_field_id);
    JNI_SET(JNI_GET_OBJECT_FIELD, jni_get_object_field);
    JNI_SET(JNI_GET_BOOLEAN_FIELD, jni_get_boolean_field);
    JNI_SET(97, jni_get_int_field);
    JNI_SET(JNI_GET_CHAR_FIELD, jni_get_char_field);
    JNI_SET(99, jni_get_int_field);
    JNI_SET(JNI_GET_INT_FIELD, jni_get_int_field);
    JNI_SET(JNI_GET_LONG_FIELD, jni_get_long_field);
    JNI_SET(JNI_GET_FLOAT_FIELD, jni_get_float_field);
    JNI_SET(103, jni_get_float_field);
    JNI_SET(JNI_SET_OBJECT_FIELD, jni_void_noop);
    JNI_SET(JNI_SET_BOOLEAN_FIELD, jni_void_noop);
    JNI_SET(JNI_SET_INT_FIELD, jni_void_noop);
    JNI_SET(JNI_GET_STATIC_METHOD_ID, jni_get_static_method_id);
    JNI_SET(JNI_CALL_STATIC_OBJECT_METHOD, jni_call_object_method);
    JNI_SET(JNI_CALL_STATIC_OBJECT_METHOD_V, jni_call_object_method_v);
    JNI_SET(JNI_CALL_STATIC_OBJECT_METHOD_A, jni_call_object_method_a);
    JNI_SET(JNI_CALL_STATIC_BOOLEAN_METHOD, jni_call_boolean_method);
    JNI_SET(JNI_CALL_STATIC_BOOLEAN_METHOD_V, jni_call_boolean_method_v);
    JNI_SET(JNI_CALL_STATIC_INT_METHOD, jni_call_int_method);
    JNI_SET(JNI_CALL_STATIC_INT_METHOD_V, jni_call_int_method_v);
    JNI_SET(JNI_CALL_STATIC_VOID_METHOD, jni_call_void_method);
    JNI_SET(JNI_CALL_STATIC_VOID_METHOD_V, jni_call_void_method_v);
    JNI_SET(143, jni_call_void_method_a);
    JNI_SET(JNI_GET_STATIC_FIELD_ID, jni_get_static_field_id);
    JNI_SET(JNI_GET_STATIC_OBJECT_FIELD, jni_get_object_field);
    JNI_SET(JNI_GET_STATIC_BOOLEAN_FIELD, jni_get_boolean_field);
    JNI_SET(147, jni_get_int_field);
    JNI_SET(148, jni_get_char_field);
    JNI_SET(149, jni_get_int_field);
    JNI_SET(JNI_GET_STATIC_INT_FIELD, jni_get_int_field);
    JNI_SET(151, jni_get_long_field);
    JNI_SET(152, jni_get_float_field);
    JNI_SET(153, jni_get_float_field);
    JNI_SET(JNI_NEW_STRING, jni_new_string);
    JNI_SET(JNI_GET_STRING_LENGTH, jni_get_string_length);
    JNI_SET(JNI_GET_STRING_CHARS, jni_get_string_chars);
    JNI_SET(JNI_RELEASE_STRING_CHARS, jni_void_noop);
    JNI_SET(JNI_NEW_STRING_UTF, jni_new_string_utf);
    JNI_SET(JNI_GET_STRING_UTF_LENGTH, jni_get_string_utf_length);
    JNI_SET(JNI_GET_STRING_UTF_CHARS, jni_get_string_utf_chars);
    JNI_SET(JNI_RELEASE_STRING_UTF_CHARS, jni_void_noop);
    JNI_SET(JNI_GET_ARRAY_LENGTH, jni_get_array_length);
    JNI_SET(JNI_NEW_OBJECT_ARRAY, jni_new_object_array);
    JNI_SET(JNI_GET_OBJECT_ARRAY_ELEMENT, jni_get_object_array_element);
    JNI_SET(JNI_SET_OBJECT_ARRAY_ELEMENT, jni_set_object_array_element);
    JNI_SET(JNI_NEW_BYTE_ARRAY, jni_new_byte_array);
    JNI_SET(JNI_NEW_INT_ARRAY, jni_new_int_array);
    JNI_SET(183, jni_get_int_array_elements);
    JNI_SET(JNI_GET_BYTE_ARRAY_ELEMENTS, jni_get_byte_array_elements);
    JNI_SET(185, jni_get_int_array_elements);
    JNI_SET(186, jni_get_int_array_elements);
    JNI_SET(JNI_GET_INT_ARRAY_ELEMENTS, jni_get_int_array_elements);
    JNI_SET(188, jni_get_int_array_elements);
    JNI_SET(189, jni_get_int_array_elements);
    JNI_SET(190, jni_get_int_array_elements);
    JNI_SET(191, jni_release_array_elements);
    JNI_SET(JNI_RELEASE_BYTE_ARRAY_ELEMENTS, jni_release_array_elements);
    JNI_SET(193, jni_release_array_elements);
    JNI_SET(194, jni_release_array_elements);
    JNI_SET(JNI_RELEASE_INT_ARRAY_ELEMENTS, jni_release_array_elements);
    JNI_SET(196, jni_release_array_elements);
    JNI_SET(197, jni_release_array_elements);
    JNI_SET(198, jni_release_array_elements);
    JNI_SET(199, jni_get_byte_array_elements);
    JNI_SET(200, jni_get_byte_array_region);
    JNI_SET(201, jni_get_byte_array_region);
    JNI_SET(202, jni_get_byte_array_region);
    JNI_SET(203, jni_get_byte_array_region);
    JNI_SET(204, jni_get_byte_array_region);
    JNI_SET(205, jni_get_byte_array_region);
    JNI_SET(206, jni_get_byte_array_region);
    JNI_SET(207, jni_get_byte_array_region);
    JNI_SET(208, jni_set_byte_array_region);
    JNI_SET(209, jni_set_byte_array_region);
    JNI_SET(210, jni_set_byte_array_region);
    JNI_SET(211, jni_set_byte_array_region);
    JNI_SET(212, jni_set_byte_array_region);
    JNI_SET(213, jni_set_byte_array_region);
    JNI_SET(214, jni_set_byte_array_region);
    JNI_SET(JNI_REGISTER_NATIVES, jni_register_natives);
    JNI_SET(JNI_UNREGISTER_NATIVES, jni_int_ok);
    JNI_SET(JNI_MONITOR_ENTER, jni_int_ok);
    JNI_SET(JNI_MONITOR_EXIT, jni_int_ok);
    JNI_SET(JNI_GET_JAVA_VM, jni_get_java_vm);
    JNI_SET(JNI_GET_STRING_REGION, jni_get_string_region);
    JNI_SET(JNI_GET_STRING_UTF_REGION, jni_get_string_utf_region);
    JNI_SET(JNI_EXCEPTION_CHECK, jni_exception_check);
    JNI_SET(JNI_NEW_DIRECT_BYTE_BUFFER, jni_new_direct_byte_buffer);
    JNI_SET(JNI_GET_DIRECT_BUFFER_ADDRESS, jni_get_direct_buffer_address);
    JNI_SET(JNI_GET_DIRECT_BUFFER_CAPACITY, jni_get_direct_buffer_capacity);
    jvm_table[4] = FUNCTION_VALUE(jvm_attach);
    jvm_table[5] = FUNCTION_VALUE(jvm_detach);
    jvm_table[JVM_GET_ENV] = FUNCTION_VALUE(jvm_get_env);
    jvm_table[7] = FUNCTION_VALUE(jvm_attach);
    jni_handle.functions = jni_table;
    jvm_handle.functions = jvm_table;
    tables_ready = 1;
#undef JNI_SET
}

static int call_jni_onload(const struct elf32_image *image, const char *label,
                           char *error, size_t error_size)
{
    typedef int32_t (*onload_fn)(void *vm, void *reserved);
    uintptr_t address = elf32_find_export(image, "JNI_OnLoad");
    onload_fn onload;
    int32_t version;

    if (address == 0U) {
        printf("SG-JNI %s has no JNI_OnLoad\n", label);
        return 0;
    }
    onload = (onload_fn)address;
    printf("SG-JNI %s JNI_OnLoad\n", label);
    version = onload(&jvm_handle, NULL);
    printf("SG-JNI %s JNI_OnLoad -> 0x%08x natives=%zu\n", label,
           (unsigned)version, native_count);
    if (version < 0x00010002) {
        snprintf(error, error_size, "%s JNI_OnLoad returned 0x%08x", label,
                 (unsigned)version);
        return -1;
    }
    return 0;
}

static int call_native(const char *name, char *error, size_t error_size)
{
    struct native_method *native = find_native("UnityPlayer", name);
    if (native == NULL)
        native = find_native(NULL, name);
    if (native == NULL || native->fn == NULL) {
        snprintf(error, error_size, "missing native %s (registered=%zu)", name,
                 native_count);
        return -1;
    }
    if (strcmp(name, "initJni") == 0) {
        typedef void (*fn)(void *, void *, void *);
        ((fn)native->fn)(&jni_handle, unity_player, activity);
        return 0;
    }
    if (strcmp(name, "nativeFile") == 0) {
        typedef void (*fn)(void *, void *, void *);
        char path[PATH_CAPACITY];
        fn file = (fn)native->fn;

        /* UnityPlayer: nativeFile(apk) then nativeFile(obb) when useObb. */
        join_path(path, sizeof(path), root_path(), apk_path());
        printf("SG-JNI nativeFile apk %s access=%d\n", path, access(path, R_OK));
        file(&jni_handle, unity_player, new_string(path));
        fill_obb_path(path, sizeof(path));
        printf("SG-JNI nativeFile obb %s access=%d\n", path, access(path, R_OK));
        /* Samurai II 1.1.4 sets useObb=false. A missing OBB must not be
           reported as a second data file. */
        if (access(path, R_OK) == 0)
            file(&jni_handle, unity_player, new_string(path));
        return 0;
    }
    if (strcmp(name, "nativeInitWWW") == 0 ||
        strcmp(name, "nativeInitWebRequest") == 0) {
        typedef void (*fn)(void *, void *, void *);
        ((fn)native->fn)(&jni_handle, unity_player,
                         new_object(FAKE_CLASS, strcmp(name, "nativeInitWWW") == 0
                                                    ? "com/unity3d/player/WWW"
                                                    : "com/unity3d/player/UnityWebRequest"));
        return 0;
    }
    if (strcmp(name, "nativeRecreateGfxState") == 0) {
        /* Unity 5 passes (int, Surface). Unity 4.6 is (Surface) only.
           A leading 0 makes 4.6 treat the window as null and never draw. */
        if (strstr(native->signature, "(Landroid/view/Surface;") ==
            native->signature) {
            typedef void (*fn)(void *, void *, void *);
            printf("SG-JNI nativeRecreateGfxState surface-only\n");
            ((fn)native->fn)(&jni_handle, unity_player, surface);
        } else {
            typedef void (*fn)(void *, void *, int32_t, void *);
            printf("SG-JNI nativeRecreateGfxState index+surface\n");
            ((fn)native->fn)(&jni_handle, unity_player, 0, surface);
        }
        return 0;
    }
    if (strcmp(name, "nativeResize") == 0) {
        typedef void (*fn)(void *, void *, int32_t, int32_t, int32_t, int32_t);
        int32_t w = nfsmw_screen_width();
        int32_t h = nfsmw_screen_height();
        /* (width, height, texWidth, texHeight). A leading 0,0 is read
           as a zero-sized view and Unity stays on its 128x128 default. */
        printf("SG-JNI nativeResize %d %d\n", w, h);
        ((fn)native->fn)(&jni_handle, unity_player, w, h, w, h);
        return 0;
    }
    if (strcmp(name, "nativeResume") == 0) {
        typedef void (*fn)(void *, void *);
        ((fn)native->fn)(&jni_handle, unity_player);
        return 0;
    }
    if (strcmp(name, "nativeFocusChanged") == 0) {
        typedef void (*fn)(void *, void *, int32_t);
        ((fn)native->fn)(&jni_handle, unity_player, 1);
        return 0;
    }
    if (strcmp(name, "nativePause") == 0) {
        typedef int32_t (*fn)(void *, void *);
        (void)((fn)native->fn)(&jni_handle, unity_player);
        return 0;
    }
    if (strcmp(name, "nativeDone") == 0) {
        typedef void (*fn)(void *, void *);
        ((fn)native->fn)(&jni_handle, unity_player);
        return 0;
    }
    snprintf(error, error_size, "unhandled native %s", name);
    return -1;
}

int nfsmw_jni_bitmap_info(void *bitmap, uint32_t information[5])
{
    (void)bitmap;
    if (information == NULL)
        return -1;
    information[0] = 1;
    information[1] = 1;
    information[2] = 4;
    information[3] = 1;
    information[4] = 0;
    return 0;
}

int nfsmw_jni_bitmap_lock(void *bitmap, void **pixels)
{
    static unsigned char dummy[4];
    (void)bitmap;
    if (pixels == NULL)
        return -1;
    *pixels = dummy;
    return 0;
}

int nfsmw_jni_bitmap_unlock(void *bitmap)
{
    (void)bitmap;
    return 0;
}

/* RVAs in this exact 1.7.0 PowerVR libil2cpp.so (PT_LOAD vaddr 0, ARM). */
enum {
    IL2CPP_GET_DATA_FILE_STATUS = 0x002298ec,
    IL2CPP_GET_WORST_STATE = 0x0022e600,
    IL2CPP_IS_OBB_FILE_ACCESSIBLE = 0x0022e844,
    IL2CPP_START_BIND_BEQ = 0x0022c0e8,
    IL2CPP_START_INVALID_MOV = 0x0022c188,
    IL2CPP_STMFD_IS_OBB = 0xe92d4ff0,
    IL2CPP_STMFD_STATUS = 0xe92d4c70,
    IL2CPP_STMFD_WORST = 0xe92d48f0,
    IL2CPP_BEQ_NOT_BOUND = 0x0a000023,
    IL2CPP_MOV_R1_INVALID = 0xe3a01003,
    IL2CPP_NOP = 0xe1a00000,
    /* B from 0x22c188 to DataDownloaded at 0x22c13c. */
    IL2CPP_B_DATADOWNLOADED = 0xeaffffeb
};

static int patch_arm_word(uintptr_t address, uint32_t expected, uint32_t word,
                          const char *label)
{
    uint32_t *code;
    uintptr_t page;
    const size_t page_size = 4096U;

    if (address < 0x1000U)
        return -1;
    code = (uint32_t *)address;
    if (code[0] != expected) {
        printf("SG-PATCH skip %s @%p first=%08x expected=%08x\n", label,
               (void *)address, code[0], expected);
        return -1;
    }
    page = address & ~(uintptr_t)(page_size - 1U);
    if (mprotect((void *)page, page_size * 2U, PROT_READ | PROT_WRITE) != 0) {
        printf("SG-PATCH mprotect %s: %s\n", label, strerror(errno));
        return -1;
    }
    code[0] = word;
    __builtin___clear_cache((char *)code, (char *)(code + 1));
    (void)mprotect((void *)page, page_size * 2U, PROT_READ | PROT_EXEC);
    printf("SG-PATCH %s %08x -> %08x\n", label, expected, word);
    return 0;
}

static int patch_arm_return(uintptr_t address, uint32_t expected_first,
                            uint32_t r0_value, const char *label)
{
    uint32_t *code;
    uintptr_t page;
    const size_t page_size = 4096U;

    if (address < 0x1000U)
        return -1;
    code = (uint32_t *)address;
    if (code[0] != expected_first) {
        printf("SG-PATCH skip %s @%p first=%08x expected=%08x\n", label,
               (void *)address, code[0], expected_first);
        return -1;
    }
    page = address & ~(uintptr_t)(page_size - 1U);
    if (mprotect((void *)page, page_size * 2U, PROT_READ | PROT_WRITE) != 0) {
        printf("SG-PATCH mprotect %s: %s\n", label, strerror(errno));
        return -1;
    }
    code[0] = 0xe3a00000u | (r0_value & 0xffu);
    code[1] = 0xe12fff1eu;
    __builtin___clear_cache((char *)code, (char *)(code + 2));
    (void)mprotect((void *)page, page_size * 2U, PROT_READ | PROT_EXEC);
    printf("SG-PATCH %s -> r0=%u\n", label, r0_value);
    return 0;
}

static void patch_obb_checks(const struct elf32_image *il2cpp_image)
{
    uintptr_t bias;

    if (il2cpp_image == NULL)
        return;
    bias = il2cpp_image->load_bias;
    (void)patch_arm_return(bias + IL2CPP_IS_OBB_FILE_ACCESSIBLE,
                           IL2CPP_STMFD_IS_OBB, 1U, "IsObbFileAccessible");
    (void)patch_arm_return(bias + IL2CPP_GET_WORST_STATE,
                           IL2CPP_STMFD_WORST, 1U, "GetWorstState");
    (void)patch_arm_return(bias + IL2CPP_GET_DATA_FILE_STATUS,
                           IL2CPP_STMFD_STATUS, 2U, "GetDataFileStatus");
    /* MenuLoader: dataBinded = dataPath.EndsWith(".obb"). Keep as a
       safety net if nativeFile(obb) does not switch dataPath. */
    (void)patch_arm_word(bias + IL2CPP_START_BIND_BEQ, IL2CPP_BEQ_NOT_BOUND,
                         IL2CPP_NOP, "Start !dataBinded beq");
    (void)patch_arm_word(bias + IL2CPP_START_INVALID_MOV, IL2CPP_MOV_R1_INVALID,
                         IL2CPP_B_DATADOWNLOADED, "Start InvalidDataError");
}

int sg_jni_startup(struct elf32_image *main_image,
                   struct elf32_image *unity_image,
                   struct elf32_image *il2cpp_image,
                   char *error, size_t error_size)
{
    char path[PATH_CAPACITY];

    initialize_tables();
    if (il2cpp_image != NULL &&
        elf32_find_export(il2cpp_image, "il2cpp_init") != 0U)
        patch_obb_checks(il2cpp_image);
    sg_android_init(nfsmw_screen_width(), nfsmw_screen_height());
    activity = new_object(FAKE_ACTIVITY,
                          "com/madfingergames/unity3d/player/MFUnityPlayerNativeActivity");
    unity_player = new_object(FAKE_GENERIC, "com/unity3d/player/UnityPlayer");
    app_info = new_object(FAKE_GENERIC, "android/content/pm/ApplicationInfo");
    surface = new_object(FAKE_SURFACE, "android/view/Surface");
    join_path(path, sizeof(path), root_path(), apk_path());
    printf("SG-JNI apk=%s\n", path);
    join_path(path, sizeof(path), root_path(), obb_path());
    printf("SG-JNI obb=%s\n", path);

    if (call_jni_onload(main_image, "libmain", error, error_size) != 0)
        return -1;
    if (call_jni_onload(unity_image, "libunity", error, error_size) != 0)
        return -1;
    if (call_jni_onload(il2cpp_image, "libil2cpp", error, error_size) != 0)
        return -1;
    (void)nfsmw_crash_trace_install();

    if (call_native("initJni", error, error_size) != 0) {
        printf("SG-JNI warn initJni: %s\n", error);
        error[0] = '\0';
    }
    if (call_native("nativeFile", error, error_size) != 0) {
        printf("SG-JNI warn nativeFile: %s\n", error);
        error[0] = '\0';
    }
    (void)call_native("nativeInitWWW", error, error_size);
    (void)call_native("nativeInitWebRequest", error, error_size);
    error[0] = '\0';
    (void)call_native("nativeResize", error, error_size);
    error[0] = '\0';
    if (call_native("nativeRecreateGfxState", error, error_size) != 0)
        return -1;
    if (call_native("nativeResume", error, error_size) != 0)
        return -1;
    if (call_native("nativeFocusChanged", error, error_size) != 0)
        return -1;
    printf("SG-JNI startup natives=%zu SG-BUILD %d\n", native_count,
           SG_RUNTIME_BUILD);
    return 0;
}

static uint32_t *pad_shm_map(void)
{
    int fd;

    if (pad_shm != NULL)
        return pad_shm;
    fd = open(NFSMW_FRAME_PATH, O_RDONLY);
    if (fd < 0)
        return NULL;
    pad_shm = mmap(NULL, NFSMW_FRAME_HDR, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (pad_shm == MAP_FAILED) {
        pad_shm = NULL;
        return NULL;
    }
    return pad_shm;
}

static void inject_event(void *event)
{
    struct native_method *native = find_native("UnityPlayer", "nativeInjectEvent");
    typedef int32_t (*fn)(void *, void *, void *);

    if (native == NULL)
        native = find_native(NULL, "nativeInjectEvent");
    if (native == NULL || native->fn == NULL)
        return;
    (void)((fn)native->fn)(&jni_handle, unity_player, event);
}

static void inject_key(int32_t keycode, int32_t action)
{
    if (keycode == 0)
        return;
    if (key_event == NULL)
        key_event = new_object(FAKE_GENERIC, "android/view/KeyEvent");
    key_event->ints[0] = keycode;
    key_event->ints[1] = action;
    key_event->ints[2] = 1;
    key_event->ints[3] = ASOURCE_XPERIA_PAD;
    inject_event(key_event);
}

static void inject_motion(int32_t action, int32_t source)
{
    if (motion_event == NULL)
        motion_event = new_object(FAKE_GENERIC, "android/view/MotionEvent");
    motion_event->ints[0] = action;
    motion_event->ints[1] = 1;
    motion_event->ints[2] = 1;
    motion_event->ints[3] = source;
    inject_event(motion_event);
}

static int pump_xperia_pad(void)
{
    uint32_t *hdr = pad_shm_map();
    uint32_t buttons = 0;
    uint32_t changed;
    size_t index;
    int l2, r2;
    static int prev_l2, prev_r2;
    static int logged;

    if (hdr == NULL || hdr[NFSMW_HDR_MAGIC] != NFSMW_FRAME_MAGIC)
        return 0;
    buttons = hdr[NFSMW_HDR_BUTTONS];
    stick_center_bias = (int)(int32_t)hdr[NFSMW_HDR_STICK_BIAS];
    for (index = 0; index < 6U; ++index)
        pad_axis[index] = (float)(int16_t)hdr[NFSMW_HDR_AXIS0 + index];

    changed = buttons ^ prev_pad_buttons;
    {
        /* Select is SDL 4. Menu is Start (6) or Guide (5) on TrimUI. */
        uint32_t menu = (1u << 5) | (1u << 6);
        int combo = (buttons & (1u << 4)) != 0 && (buttons & menu) != 0;
        int prev_combo = (prev_pad_buttons & (1u << 4)) != 0 &&
                         (prev_pad_buttons & menu) != 0;
        if (combo && !prev_combo)
            collider_debug_next();
        for (index = 0; index < 15U; ++index) {
            if ((changed & (1u << index)) == 0)
                continue;
            if ((combo || prev_combo) &&
                (index == 4U || index == 5U || index == 6U))
                continue;
            inject_key(xperia_keycodes[index],
                       (buttons & (1u << index)) != 0 ? 0 : 1);
        }
    }
    l2 = pad_axis[4] > 12000.0f;
    r2 = pad_axis[5] > 12000.0f;
    if (l2 != prev_l2 && trigger_l2_key != 0)
        inject_key(trigger_l2_key, l2 ? 0 : 1);
    if (r2 != prev_r2 && trigger_r2_key != 0)
        inject_key(trigger_r2_key, r2 ? 0 : 1);
    prev_l2 = l2;
    prev_r2 = r2;
    prev_pad_buttons = buttons;

    {
        /* Tells us whether the right stick and triggers reach the runtime at
           all, which the buttons=0x... line from the presenter cannot show. */
        static int raw_logs;
        static float last_rx, last_ry, last_lt, last_rt;
        if (raw_logs < 200 &&
            (fabsf(pad_axis[2] - last_rx) > 3000.0f ||
             fabsf(pad_axis[3] - last_ry) > 3000.0f ||
             fabsf(pad_axis[4] - last_lt) > 3000.0f ||
             fabsf(pad_axis[5] - last_rt) > 3000.0f)) {
            printf("SG-PAD raw lx=%.0f ly=%.0f rx=%.0f ry=%.0f lt=%.0f rt=%.0f\n",
                   pad_axis[0], pad_axis[1], pad_axis[2], pad_axis[3],
                   pad_axis[4], pad_axis[5]);
            last_rx = pad_axis[2];
            last_ry = pad_axis[3];
            last_lt = pad_axis[4];
            last_rt = pad_axis[5];
            raw_logs += 1;
        }
    }

    /* Joystick MOVE every frame so Unity reads getAxisValue (right stick look).
       Source must be plain SOURCE_JOYSTICK: real hardware never mixes in the
       GAMEPAD bits here, and Unity matches SOURCE_CLASS_MASK exactly. */
    inject_motion(2, ASOURCE_JOYSTICK);

    if (logged == 0) {
        printf("SG-PAD inject nativeInjectEvent=%s shm=%p softfp-float=1\n",
               find_native("UnityPlayer", "nativeInjectEvent") != NULL ||
                       find_native(NULL, "nativeInjectEvent") != NULL
                   ? "yes"
                   : "NO",
               (void *)hdr);
        logged = 1;
    }
    return (buttons & (1u << 4)) != 0 && (buttons & (1u << 6)) != 0;
}

static volatile unsigned int watchdog_renders;

static void watch_tasks(void)
{
    DIR *dir = opendir("/proc/self/task");
    struct dirent *ent;

    if (dir == NULL)
        return;
    while ((ent = readdir(dir)) != NULL) {
        char path[128];
        char buf[256];
        FILE *file;
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
            continue;
        snprintf(path, sizeof(path), "/proc/self/task/%s/syscall", ent->d_name);
        file = fopen(path, "r");
        if (file != NULL) {
            if (fgets(buf, sizeof(buf), file) != NULL)
                printf("SG-WATCH tid=%s syscall %s", ent->d_name, buf);
            fclose(file);
        }
        snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", ent->d_name);
        file = fopen(path, "r");
        if (file != NULL) {
            if (fgets(buf, sizeof(buf), file) != NULL)
                printf("SG-WATCH tid=%s wchan %s\n", ent->d_name, buf);
            fclose(file);
        }
    }
    closedir(dir);
}

static void *watchdog_thread(void *unused)
{
    unsigned int last = 0U;
    int dumped = 0;
    (void)unused;
    for (;;) {
        sleep(2);
        printf("SG-WATCH nativeRender returns=%u%s\n", watchdog_renders,
               watchdog_renders == last ? " (blocked)" : "");
        if (watchdog_renders == last && dumped < 2) {
            watch_tasks();
            dumped += 1;
        }
        last = watchdog_renders;
    }
    return NULL;
}

int sg_jni_run(char *error, size_t error_size)
{
    struct native_method *render = find_native("UnityPlayer", "nativeRender");
    typedef int32_t (*render_fn)(void *, void *);
    unsigned int frames = 0U;
    pthread_t watchdog;

    if (render == NULL)
        render = find_native(NULL, "nativeRender");
    if (render == NULL || render->fn == NULL) {
        snprintf(error, error_size, "nativeRender was not registered");
        return -1;
    }
    printf("SG-JNI entering nativeRender loop\n");
    (void)nfsmw_crash_trace_install();
    if (pthread_create(&watchdog, NULL, watchdog_thread, NULL) == 0)
        pthread_detach(watchdog);
    while (request_exit == 0) {
        int32_t keep_going;

        collider_noclip_arm();
        keep_going = ((render_fn)render->fn)(&jni_handle, unity_player);
        watchdog_renders += 1U;
        nfsmw_opensl_pump();
        /* Unity 4.6 registers the FMOD natives but never starts the Java
         * AudioTrack thread. OpenSL is dlopened and then abandoned. The
         * mixer thread itself waits until FMOD reports a sample rate. */
        if (frames == 0U && fmod_running == 0 &&
            nfsmw_opensl_started() == 0 &&
            find_native(NULL, "fmodGetInfo") != NULL) {
            printf("SG-FMOD start mixer (no Java thread, no OpenSL)\n");
            fmod_audio_device_start(NULL);
        }
        if (pump_xperia_pad() != 0)
            request_exit = 1;
        collider_noclip_move(pad_axis[0], pad_axis[1]);
        collider_first_lever();
        frames += 1U;
        if ((frames % 60U) == 0U)
            printf("SG-JNI frames=%u keep=%d\n", frames, keep_going);
        if (keep_going == 0)
            break;
    }
    printf("SG-JNI leave render frames=%u\n", frames);
    return 0;
}

void sg_jni_shutdown(void)
{
    char error[128];
    request_exit = 1;
    fmod_audio_device_stop();
    (void)call_native("nativePause", error, sizeof(error));
    (void)call_native("nativeDone", error, sizeof(error));
}
