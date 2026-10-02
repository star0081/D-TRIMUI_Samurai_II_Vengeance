#ifndef SG_ANDROID_NATIVE_H
#define SG_ANDROID_NATIVE_H

#include <stdint.h>

void sg_android_init(int width, int height);
uintptr_t sg_android_resolve(const char *name);
void *sg_android_dlopen(const char *name);
void *sg_android_dlsym(void *handle, const char *name);
int sg_android_is_handle(void *handle);
void *sg_android_window(void);

#endif
