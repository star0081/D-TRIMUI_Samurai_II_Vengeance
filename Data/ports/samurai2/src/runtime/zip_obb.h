#ifndef SG_ZIP_OBB_H
#define SG_ZIP_OBB_H

#include <stdio.h>
#include <sys/stat.h>

/* Serve Unity paths of the form "<apk|obb>/assets/..." from the zip. */
int zip_obb_stat(const char *path, struct stat *out);
int zip_obb_access(const char *path);
FILE *zip_obb_fopen(const char *path);

/*
 * Unity Audio/FMOD opens sharedassetsN.resource via SplitFile when
 * name.split0 exists, but only probes leftover split0+split1. Hide every
 * .resource.splitN (Exists = false) so Unity opens the unsplit name as a
 * regular file, then serve the concatenated blob once.
 *
 * Returns:
 *  -1  not a managed .resource path (caller continues)
 *   0  success (*out filled / FILE* set)
 *   1  managed but missing (errno = ENOENT)
 */
int zip_obb_resource_stat(const char *path, struct stat *out);
int zip_obb_resource_fopen(const char *path, FILE **out);

#endif
