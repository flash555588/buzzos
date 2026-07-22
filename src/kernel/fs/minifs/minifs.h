#ifndef BUZZOS_MINIFS_H
#define BUZZOS_MINIFS_H

#include <stddef.h>
#include <stdint.h>
#include "vfs.h"

enum {
    MINIFS_LBA_START = 67584,
    MINIFS_SECTORS = 32768,
};

struct fs_info {
    uint32_t magic;
    uint32_t inode_count;
    uint32_t used_inodes;
    uint32_t dir_count;
    uint32_t file_count;
    uint32_t block_count;
    uint32_t used_blocks;
    uint32_t free_blocks;
    uint32_t data_lba;
    uint32_t max_file_size;
};

int minifs_mount(void);
int minifs_info(struct fs_info *out);
int minifs_open(const char *path, uint16_t *ino_out);
int minifs_create(const char *path);
int minifs_mkdir(const char *path);
int minifs_unlink(const char *path);
int minifs_rmdir(const char *path);
int minifs_rename(const char *old_path, const char *new_path);
int minifs_truncate(const char *path);
int minifs_size_ino(uint16_t ino, size_t *size_out);
int minifs_stat(const char *path, struct stat *st);
int minifs_read(uint16_t ino, size_t *pos, void *buf, size_t count);
int minifs_write(uint16_t ino, size_t *pos, const void *buf, size_t count);
int minifs_getdents(uint16_t ino, size_t *pos, struct dirent *ents, size_t count);
int minifs_is_dir_path(const char *path);
int minifs_is_dir_ino(uint16_t ino);

#endif
