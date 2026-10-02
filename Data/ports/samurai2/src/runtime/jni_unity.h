#ifndef SG_JNI_UNITY_H
#define SG_JNI_UNITY_H

#include "elf32_loader.h"

#include <stddef.h>
#include <stdint.h>

int sg_jni_startup(struct elf32_image *main_image,
                   struct elf32_image *unity_image,
                   struct elf32_image *il2cpp_image,
                   char *error, size_t error_size);
int sg_jni_run(char *error, size_t error_size);
void sg_jni_shutdown(void);

int nfsmw_jni_bitmap_info(void *bitmap, uint32_t information[5]);
int nfsmw_jni_bitmap_lock(void *bitmap, void **pixels);
int nfsmw_jni_bitmap_unlock(void *bitmap);

#endif
