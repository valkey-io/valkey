#ifndef __FORKLESS_H__
#define __FORKLESS_H__

#include "server.h"

#define FORKLESS_SAVE_FILE_ITER_NAME "forkless_save_file"
#define FORKLESS_SOCKET_ITER_NAME "forkless_socket"

int forklessSaveToDisk(const char *filename);
int forklessSaveToSockets(void);
void forklessSaveCancel(void);
int isForklessSaveInProgress(void);
sds forkless_catInfo(sds info);
sds forkless_catDebugInfo(sds info);

#endif
