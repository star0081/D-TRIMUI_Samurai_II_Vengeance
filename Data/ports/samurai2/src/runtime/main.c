#include "elf32_loader.h"
#include "compat_bridge.h"
#include "initializer_trace.h"
#include "jni_unity.h"
#include "relocation_probe.h"
#include "symbol_probe.h"
#include "android_native.h"
#include "crash_trace.h"
#include "screen_size.h"
#include "build_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MODULE_COUNT = 3, PATH_CAPACITY = 4096 };

static const char *const module_names[MODULE_COUNT] = {
 "libmain.so",
 "libunity.so",
 "libmono.so",
};

int main(int argc, char **argv)
{
    struct elf32_image images[MODULE_COUNT];
    struct nfsmw_symbol_probe_stats symbol_stats;
    struct nfsmw_relocation_probe_stats relocation_stats;
    char error[ELF32_LOADER_ERROR_CAPACITY];
    char path[PATH_CAPACITY];
    const char *libdir;
    size_t mapped = 0U;
    size_t index;
    int result = 1;
    int constructors_started = 0;

    (void)setvbuf(stdout, NULL, _IONBF, 0U);
    (void)setvbuf(stderr, NULL, _IONBF, 0U);
    printf("=== samurai2_runtime enter S2-BUILD %d ===\n", SG_RUNTIME_BUILD);
    nfsmw_compat_init();
    (void)nfsmw_crash_trace_install();
    (void)memset(images, 0, sizeof(images));

    libdir = argc >= 2 ? argv[1] : "gamefiles/android-libs";
    printf("=== samurai2 unity 4.6.3p2 mono map ===\n");
    printf("libdir=%s w=%d h=%d\n", libdir, nfsmw_screen_width(),
           nfsmw_screen_height());
    sg_android_init(nfsmw_screen_width(), nfsmw_screen_height());

    for (index = 0U; index < MODULE_COUNT; ++index) {
        const int length = snprintf(path, sizeof(path), "%s/%s", libdir,
                                    module_names[index]);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            fprintf(stderr, "module path too long\n");
            goto done;
        }
        printf("mapping[%zu]=%s\n", index, path);
        if (elf32_map(&images[index], path, error, sizeof(error)) != 0) {
            fprintf(stderr, "FAIL map: %s\n", error);
            goto done;
        }
        mapped += 1U;
        (void)elf32_describe(&images[index]);
        nfsmw_compat_register_image(&images[index]);
    }

    if (nfsmw_probe_symbols(images, MODULE_COUNT, &symbol_stats, error,
                            sizeof(error)) != 0) {
        fprintf(stderr, "symbol census FAIL: %s\n", error);
        goto done;
    }
    if (nfsmw_relocation_probe(images, MODULE_COUNT, &relocation_stats, error,
                               sizeof(error)) != 0) {
        fprintf(stderr, "reloc FAIL: %s\n", error);
        goto done;
    }
    if (relocation_stats.unresolved_relocations != 0U) {
        fprintf(stderr, "reloc unresolved=%zu (continuing)\n",
                relocation_stats.unresolved_relocations);
    }

    if (nfsmw_compat_signal_selftest() != 0) {
        fprintf(stderr, "bionic signal self-test FAIL\n");
        goto done;
    }

    constructors_started = 1;
    {
        size_t initializer_count = 0U;
        for (index = 0U; index < MODULE_COUNT; ++index) {
            if (nfsmw_run_initializers(&images[index], &initializer_count,
                                       error, sizeof(error)) != 0) {
                fprintf(stderr, "ctor FAIL: %s\n", error);
                goto done;
            }
        }
        printf("ctors=%zu\n", initializer_count);
    }

    if (sg_jni_startup(&images[0], &images[1], &images[2], error,
                       sizeof(error)) != 0) {
        fprintf(stderr, "JNI startup FAIL: %s\n", error);
        goto done;
    }
    if (sg_jni_run(error, sizeof(error)) != 0) {
        fprintf(stderr, "JNI run FAIL: %s\n", error);
        goto done;
    }
    result = 0;

done:
    sg_jni_shutdown();
    if (constructors_started) {
        nfsmw_compat_finalize();
        fflush(NULL);
        _exit(result);
    }
    while (mapped != 0U) {
        mapped -= 1U;
        elf32_unmap(&images[mapped]);
    }
    nfsmw_relocation_release_hosts();
    return result;
}
