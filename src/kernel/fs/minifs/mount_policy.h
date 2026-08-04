#ifndef BUZZOS_MINIFS_MOUNT_POLICY_H
#define BUZZOS_MINIFS_MOUNT_POLICY_H

#include <stddef.h>
#include <stdint.h>

enum minifs_superblock_action {
    MINIFS_SUPERBLOCK_FORMAT_BLANK = 0,
    MINIFS_SUPERBLOCK_MOUNT_CURRENT,
    MINIFS_SUPERBLOCK_MIGRATE_V1,
    MINIFS_SUPERBLOCK_REJECT_CORRUPT,
};

struct minifs_superblock_prefix {
    uint32_t magic;
    uint32_t inode_count;
    uint32_t block_count;
    uint32_t data_lba;
};

enum minifs_superblock_action minifs_classify_superblock(
    const uint8_t *sector, size_t sector_size, uint32_t magic,
    uint32_t current_inodes, uint32_t current_blocks,
    uint32_t current_data_lba, uint32_t v1_inodes);

#endif
