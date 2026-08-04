#include "mount_policy.h"

enum minifs_superblock_action minifs_classify_superblock(
    const uint8_t *sector, size_t sector_size, uint32_t magic,
    uint32_t current_inodes, uint32_t current_blocks,
    uint32_t current_data_lba, uint32_t v1_inodes) {
    if (!sector || sector_size < sizeof(struct minifs_superblock_prefix))
        return MINIFS_SUPERBLOCK_REJECT_CORRUPT;

    int blank = 1;
    for (size_t i = 0; i < sector_size; i++) {
        if (sector[i] != 0) {
            blank = 0;
            break;
        }
    }
    if (blank)
        return MINIFS_SUPERBLOCK_FORMAT_BLANK;

    const struct minifs_superblock_prefix *sb =
        (const struct minifs_superblock_prefix *)sector;
    for (size_t i = sizeof(*sb); i < sector_size; i++) {
        if (sector[i] != 0)
            return MINIFS_SUPERBLOCK_REJECT_CORRUPT;
    }
    if (sb->magic != magic)
        return MINIFS_SUPERBLOCK_REJECT_CORRUPT;
    if (sb->inode_count == current_inodes &&
        sb->block_count == current_blocks &&
        sb->data_lba == current_data_lba)
        return MINIFS_SUPERBLOCK_MOUNT_CURRENT;
    if (sb->inode_count == v1_inodes)
        return MINIFS_SUPERBLOCK_MIGRATE_V1;
    return MINIFS_SUPERBLOCK_REJECT_CORRUPT;
}
