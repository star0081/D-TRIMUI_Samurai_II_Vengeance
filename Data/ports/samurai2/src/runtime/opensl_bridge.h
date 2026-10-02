#ifndef NFSMW_OPENSL_BRIDGE_H
#define NFSMW_OPENSL_BRIDGE_H

void *nfsmw_opensl_handle(void);
int nfsmw_opensl_is_handle(void *handle);
int nfsmw_opensl_is_name(const char *name);
void *nfsmw_opensl_dlsym(const char *name);
int nfsmw_opensl_started(void);
void nfsmw_opensl_pump(void);

#endif
