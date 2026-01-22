#ifndef __THREADSAVE_H__
#define __THREADSAVE_H__

#include "server.h"

#define THREADSAVE_FILE_ITER_NAME "threadsave_file"

int threadsaveToDisk(const char *filename);
void threadsaveCancel(void);

#endif
