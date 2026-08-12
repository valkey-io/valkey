#ifndef __FORKLESS_H__
#define __FORKLESS_H__

#include "server.h"

#define FORKLESS_SAVE_FILE_ITER_NAME "forkless_save_file"

int forklessSaveToDisk(const char *filename);
void forklessSaveCancel(void);

#endif
