#include "opensl_bridge.h"
#include "platform_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t sl_result;
typedef void **sl_interface;

enum { SL_SUCCESS = 0U, SL_PLAYSTATE_STOPPED = 1U };

/*
 * Khronos / Android SLInterfaceID is a pointer to a 16-byte UUID. FMOD
 * compares those UUIDs, not our token addresses. time_low uniquely identifies
 * the interfaces this title asks for.
 */
struct sl_iid {
    uint32_t time_low;
    uint16_t time_mid;
    uint16_t time_hi;
    uint8_t rest[8];
};

enum {
    IID_ENGINE_LOW = 0x8d779ea0U,
    IID_PLAY_LOW = 0xef6b7a22U,
    IID_BUFFERQUEUE_LOW = 0x2bc99cc0U,
    IID_ANDROIDQUEUE_LOW = 0x1b798b88U,
    IID_ANDROIDCONFIG_LOW = 0x89f6a7e0U,
    IID_RECORD_LOW = 0xc5657aa0U,
    IID_VOLUME_LOW = 0x09e8ede0U
};

struct pcm_format {
    uint32_t format_type;
    uint32_t channels;
    uint32_t samples_per_second;
    uint32_t bits_per_sample;
    uint32_t container_size;
    uint32_t channel_mask;
    uint32_t endianness;
};

struct data_source { void *locator; void *format; };

static unsigned char handle_token;
static const struct sl_iid iid_engine_value = {
    IID_ENGINE_LOW, 0x46bb, 0x11df, { 0xaa, 0x0a, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b }
};
static const struct sl_iid iid_play_value = {
    IID_PLAY_LOW, 0xd9c9, 0x11df, { 0x86, 0x00, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b }
};
static const struct sl_iid iid_queue_value = {
    IID_ANDROIDQUEUE_LOW, 0xd3fe, 0x47e5, { 0xa2, 0xbb, 0x49, 0x8b, 0x02, 0x7b, 0xde, 0x56 }
};
static const struct sl_iid iid_bufferqueue_value = {
    IID_BUFFERQUEUE_LOW, 0xddd4, 0x11db, { 0x8d, 0x82, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b }
};
static const struct sl_iid iid_config_value = {
    IID_ANDROIDCONFIG_LOW, 0xbeac, 0x11df, { 0x81, 0x40, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b }
};
static const struct sl_iid iid_record_value = {
    IID_RECORD_LOW, 0xd965, 0x11df, { 0xbe, 0x07, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b }
};
static const struct sl_iid iid_volume_value = {
    IID_VOLUME_LOW, 0xfdfd, 0x11db, { 0x36, 0xf0, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b }
};

/* Exported the same way libOpenSLES.so does: symbol is a pointer to the UUID. */
const struct sl_iid *SL_IID_ENGINE = &iid_engine_value;
const struct sl_iid *SL_IID_PLAY = &iid_play_value;
const struct sl_iid *SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &iid_queue_value;
const struct sl_iid *SL_IID_BUFFERQUEUE = &iid_bufferqueue_value;
const struct sl_iid *SL_IID_ANDROIDCONFIGURATION = &iid_config_value;
const struct sl_iid *SL_IID_RECORD = &iid_record_value;
const struct sl_iid *SL_IID_VOLUME = &iid_volume_value;

static void *iid_engine = &iid_engine_value;
static void *iid_play = &iid_play_value;
static void *iid_queue = &iid_queue_value;
static void *iid_config = &iid_config_value;
static void *iid_record = &iid_record_value;

static uintptr_t object_vtable[10];
static uintptr_t engine_vtable[20];
static uintptr_t play_vtable[12];
static uintptr_t queue_vtable[8];
static uintptr_t config_vtable[4];
static uintptr_t dummy_vtable[20];
static uintptr_t *engine_object_functions = object_vtable;
static uintptr_t *mix_object_functions = object_vtable;
static uintptr_t *player_object_functions = object_vtable;
static uintptr_t *engine_functions = engine_vtable;
static uintptr_t *play_functions = play_vtable;
static uintptr_t *queue_functions = queue_vtable;
static uintptr_t *config_functions = config_vtable;
static uintptr_t *dummy_functions = dummy_vtable;
static sl_interface engine_object = (sl_interface)&engine_object_functions;
static sl_interface mix_object = (sl_interface)&mix_object_functions;
static sl_interface player_object = (sl_interface)&player_object_functions;
static sl_interface engine_interface = (sl_interface)&engine_functions;
static sl_interface play_interface = (sl_interface)&play_functions;
static sl_interface queue_interface = (sl_interface)&queue_functions;
static sl_interface config_interface = (sl_interface)&config_functions;
static sl_interface dummy_interface = (sl_interface)&dummy_functions;
static void (*queue_callback)(sl_interface caller, void *context);
static void *queue_context;
static uint32_t queue_buffer_size;
static uint32_t play_state = SL_PLAYSTATE_STOPPED;
static int initialized;
static int callback_active;
static int engine_started;

static uintptr_t function_value(const void *storage, size_t storage_size)
{
    uintptr_t value = 0U;
    if (storage_size == sizeof(value)) (void)memcpy(&value, storage, sizeof(value));
    return value;
}

#define FN_VALUE(function)                                                   \
    function_value(&(__typeof__(&(function))){ &(function) },                \
                   sizeof(&(function)))

static void initialize(void);

static uint32_t iid_time_low(const void *iid)
{
    uint32_t low = 0U;
    const void *uuid = iid;

    if (iid == NULL) return 0U;
    if (iid == iid_engine || iid == &iid_engine_value || iid == &SL_IID_ENGINE)
        return IID_ENGINE_LOW;
    if (iid == iid_play || iid == &iid_play_value || iid == &SL_IID_PLAY)
        return IID_PLAY_LOW;
    if (iid == iid_queue || iid == &iid_queue_value ||
        iid == &SL_IID_ANDROIDSIMPLEBUFFERQUEUE || iid == &SL_IID_BUFFERQUEUE)
        return IID_ANDROIDQUEUE_LOW;
    if (iid == iid_config || iid == &iid_config_value ||
        iid == &SL_IID_ANDROIDCONFIGURATION)
        return IID_ANDROIDCONFIG_LOW;
    if (iid == iid_record || iid == &iid_record_value || iid == &SL_IID_RECORD)
        return IID_RECORD_LOW;
    /*
     * FMOD sometimes passes SLInterfaceID (pointer to UUID) and sometimes the
     * address of the exported pointer from dlsym. Accept both.
     */
    if (*(const void *const *)iid == &iid_engine_value) return IID_ENGINE_LOW;
    if (*(const void *const *)iid == &iid_play_value) return IID_PLAY_LOW;
    if (*(const void *const *)iid == &iid_queue_value ||
        *(const void *const *)iid == &iid_bufferqueue_value)
        return IID_ANDROIDQUEUE_LOW;
    if (*(const void *const *)iid == &iid_config_value)
        return IID_ANDROIDCONFIG_LOW;
    memcpy(&low, uuid, sizeof(low));
    if (low != IID_ENGINE_LOW && low != IID_PLAY_LOW &&
        low != IID_BUFFERQUEUE_LOW && low != IID_ANDROIDQUEUE_LOW &&
        low != IID_ANDROIDCONFIG_LOW && low != IID_RECORD_LOW &&
        low != IID_VOLUME_LOW) {
        memcpy(&low, *(const void *const *)iid, sizeof(low));
    }
    return low;
}

static sl_result sl_ok(sl_interface self, ...)
{
    (void)self;
    return SL_SUCCESS;
}

static void sl_void(sl_interface self, ...)
{
    (void)self;
}

static sl_result object_get_state(sl_interface self, uint32_t *state)
{
    (void)self;
    if (state != NULL) *state = 2U;
    return SL_SUCCESS;
}

static sl_result object_get_interface(sl_interface self, const void *iid,
                                      void *result)
{
    uint32_t low = iid_time_low(iid);
    void *value = dummy_interface;

    if (low == IID_ENGINE_LOW) value = engine_interface;
    else if (low == IID_PLAY_LOW) value = play_interface;
    else if (low == IID_BUFFERQUEUE_LOW || low == IID_ANDROIDQUEUE_LOW)
        value = queue_interface;
    else if (low == IID_ANDROIDCONFIG_LOW) value = config_interface;
    if (result == NULL) return 12U;
    *(void **)result = value;
    (void)printf("G8-OPENSL GetInterface self=%p iid_low=0x%08x -> %p\n",
                 (void *)self, low, value);
    return SL_SUCCESS;
}

static void object_destroy(sl_interface self)
{
    if (self == player_object) {
        queue_callback = NULL;
        queue_context = NULL;
        queue_buffer_size = 0U;
    }
}

static sl_result engine_create_audio_player(sl_interface self,
                                            sl_interface *player,
                                            struct data_source *source,
                                            void *sink,
                                            uint32_t interface_count,
                                            const void *interface_ids,
                                            const uint32_t *required)
{
    const struct pcm_format *format = source != NULL ? source->format : NULL;
    int frequency = 44100;
    int channels = 2;
    (void)self; (void)sink; (void)interface_count;
    (void)interface_ids; (void)required;
    if (format != NULL) {
        if (format->samples_per_second >= 1000U)
            frequency = (int)(format->samples_per_second / 1000U);
        if (format->channels >= 1U && format->channels <= 2U)
            channels = (int)format->channels;
    }
    if (player == NULL ||
        nfsmw_platform_runtime_audio_start(frequency, channels) != 0) return 13U;
    *player = player_object;
    (void)printf("G8-OPENSL CreateAudioPlayer frequency=%d channels=%d\n",
                 frequency, channels);
    return SL_SUCCESS;
}

static sl_result engine_create_output_mix(sl_interface self,
                                          sl_interface *mix,
                                          uint32_t interface_count,
                                          const void *interface_ids,
                                          const uint32_t *required)
{
    (void)self; (void)interface_count; (void)interface_ids; (void)required;
    if (mix == NULL) return 13U;
    *mix = mix_object;
    (void)printf("G8-OPENSL CreateOutputMix\n");
    return SL_SUCCESS;
}

static sl_result play_set_state(sl_interface self, uint32_t state)
{
    (void)self;
    play_state = state;
    (void)printf("G8-OPENSL SetPlayState %u\n", state);
    return SL_SUCCESS;
}

static sl_result play_get_state(sl_interface self, uint32_t *state)
{
    (void)self;
    if (state != NULL) *state = play_state;
    return SL_SUCCESS;
}

static sl_result queue_enqueue(sl_interface self, const void *buffer,
                               uint32_t size)
{
    (void)self;
    if (nfsmw_platform_runtime_audio_queue(buffer, size) != 0) return 13U;
    queue_buffer_size = size;
    return SL_SUCCESS;
}

static sl_result queue_clear(sl_interface self)
{
    (void)self;
    return SL_SUCCESS;
}

static sl_result queue_get_state(sl_interface self, void *state)
{
    uint32_t *words = state;
    (void)self;
    if (words != NULL) {
        words[0] = nfsmw_platform_runtime_audio_queued() != 0U ? 1U : 0U;
        words[1] = 0U;
    }
    return SL_SUCCESS;
}

static sl_result queue_register_callback(
    sl_interface self, void (*callback)(sl_interface, void *), void *context)
{
    (void)self;
    queue_callback = callback;
    queue_context = context;
    return SL_SUCCESS;
}

static sl_result config_set(sl_interface self, const char *key,
                            const void *value, uint32_t size)
{
    (void)self; (void)key; (void)value; (void)size;
    return SL_SUCCESS;
}

static sl_result sl_create_engine(sl_interface *engine, uint32_t options,
                                  const void *option_array,
                                  uint32_t interface_count,
                                  const void *interface_ids,
                                  const uint32_t *required);

sl_result slCreateEngine(sl_interface *engine, uint32_t options,
                         const void *option_array, uint32_t interface_count,
                         const void *interface_ids, const uint32_t *required)
{
    return sl_create_engine(engine, options, option_array, interface_count,
                            interface_ids, required);
}

static sl_result sl_create_engine(sl_interface *engine, uint32_t options,
                                  const void *option_array,
                                  uint32_t interface_count,
                                  const void *interface_ids,
                                  const uint32_t *required)
{
    (void)options; (void)option_array; (void)interface_count;
    (void)interface_ids; (void)required;
    if (engine == NULL) return 13U;
    initialize();
    *engine = engine_object;
    engine_started = 1;
    (void)printf("G8-OPENSL slCreateEngine\n");
    return SL_SUCCESS;
}

static void initialize(void)
{
    size_t index;
    if (initialized != 0) return;
    for (index = 0U; index < 10U; ++index) object_vtable[index] = FN_VALUE(sl_ok);
    object_vtable[2] = FN_VALUE(object_get_state);
    object_vtable[3] = FN_VALUE(object_get_interface);
    object_vtable[5] = FN_VALUE(sl_void);
    object_vtable[6] = FN_VALUE(object_destroy);
    for (index = 0U; index < 20U; ++index) {
        engine_vtable[index] = FN_VALUE(sl_ok);
        dummy_vtable[index] = FN_VALUE(sl_ok);
    }
    engine_vtable[2] = FN_VALUE(engine_create_audio_player);
    engine_vtable[7] = FN_VALUE(engine_create_output_mix);
    for (index = 0U; index < 12U; ++index) play_vtable[index] = FN_VALUE(sl_ok);
    play_vtable[0] = FN_VALUE(play_set_state);
    play_vtable[1] = FN_VALUE(play_get_state);
    for (index = 0U; index < 8U; ++index) queue_vtable[index] = FN_VALUE(sl_ok);
    queue_vtable[0] = FN_VALUE(queue_enqueue);
    queue_vtable[1] = FN_VALUE(queue_clear);
    queue_vtable[2] = FN_VALUE(queue_get_state);
    queue_vtable[3] = FN_VALUE(queue_register_callback);
    config_vtable[0] = FN_VALUE(config_set);
    config_vtable[1] = FN_VALUE(sl_ok);
    config_vtable[2] = FN_VALUE(sl_ok);
    config_vtable[3] = FN_VALUE(sl_ok);
    initialized = 1;
}

void *nfsmw_opensl_handle(void)
{
    initialize();
    return &handle_token;
}

int nfsmw_opensl_is_handle(void *handle)
{
    return handle == &handle_token;
}

int nfsmw_opensl_is_name(const char *name)
{
    const char *base;

    if (name == NULL || name[0] == '\0') return 0;
    base = strrchr(name, '/');
    base = base != NULL ? base + 1 : name;
    return strcmp(base, "libOpenSLES.so") == 0 ||
           strcmp(base, "libOpenSLES.so.1") == 0 ||
           strcmp(base, "libOpenSLES.so.1.0") == 0;
}

static int opensl_symbol(const char *name)
{
    return name != NULL &&
           (strncmp(name, "sl", 2) == 0 || strncmp(name, "SL_", 3) == 0);
}

void *nfsmw_opensl_dlsym(const char *name)
{
    uintptr_t value = 0U;
    void *result = NULL;
    static unsigned logs;

    if (name == NULL) return NULL;
    initialize();
    if (strcmp(name, "slCreateEngine") == 0) value = FN_VALUE(slCreateEngine);
    else if (strcmp(name, "SL_IID_ENGINE") == 0) result = &SL_IID_ENGINE;
    else if (strcmp(name, "SL_IID_PLAY") == 0) result = &SL_IID_PLAY;
    else if (strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") == 0)
        result = &SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
    else if (strcmp(name, "SL_IID_BUFFERQUEUE") == 0)
        result = &SL_IID_BUFFERQUEUE;
    else if (strcmp(name, "SL_IID_ANDROIDCONFIGURATION") == 0)
        result = &SL_IID_ANDROIDCONFIGURATION;
    else if (strcmp(name, "SL_IID_RECORD") == 0) result = &SL_IID_RECORD;
    else if (strcmp(name, "SL_IID_VOLUME") == 0) result = &SL_IID_VOLUME;
    if (result == NULL)
        result = (void *)value;
    if (opensl_symbol(name) && logs < 32U) {
        (void)printf("G8-OPENSL dlsym %s -> %p\n", name, result);
        logs += 1U;
    }
    return result;
}

int nfsmw_opensl_started(void)
{
    return engine_started;
}

void nfsmw_opensl_pump(void)
{
    if (queue_callback != NULL && queue_buffer_size != 0U &&
        callback_active == 0 &&
        nfsmw_platform_runtime_audio_queued() <= queue_buffer_size) {
        callback_active = 1;
        queue_callback(queue_interface, queue_context);
        callback_active = 0;
    }
}
