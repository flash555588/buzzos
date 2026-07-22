#ifndef BUZZOS_DIRENT_COMPAT_H
#define BUZZOS_DIRENT_COMPAT_H

#include "libc.h"

typedef struct buzz_dir DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *directory);
int closedir(DIR *directory);
int dirfd(DIR *directory);

#endif
