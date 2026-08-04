#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mount_policy.h"

enum {
    TEST_MAGIC = 0x5346424Du,
    TEST_INODES = 2048,
    TEST_BLOCKS = 63363,
    TEST_DATA_LBA = 69657,
    TEST_V1_INODES = 128,
};

static enum minifs_superblock_action classify(const uint8_t *sector) {
    return minifs_classify_superblock(sector, 512, TEST_MAGIC,
                                      TEST_INODES, TEST_BLOCKS, TEST_DATA_LBA,
                                      TEST_V1_INODES);
}

int main(void) {
    uint8_t sector[512] = {0};
    if (classify(sector) != MINIFS_SUPERBLOCK_FORMAT_BLANK) {
        fputs("blank superblock was not classified for formatting\n", stderr);
        return 1;
    }

    struct minifs_superblock_prefix *sb =
        (struct minifs_superblock_prefix *)sector;
    sb->magic = TEST_MAGIC;
    sb->inode_count = TEST_INODES;
    sb->block_count = TEST_BLOCKS;
    sb->data_lba = TEST_DATA_LBA;
    if (classify(sector) != MINIFS_SUPERBLOCK_MOUNT_CURRENT) {
        fputs("valid current superblock was rejected\n", stderr);
        return 1;
    }

    sb->inode_count = TEST_V1_INODES;
    if (classify(sector) != MINIFS_SUPERBLOCK_MIGRATE_V1) {
        fputs("valid v1 superblock was not offered migration\n", stderr);
        return 1;
    }

    sb->inode_count = TEST_INODES;
    sb->data_lba = TEST_DATA_LBA + 1;
    if (classify(sector) != MINIFS_SUPERBLOCK_REJECT_CORRUPT) {
        fputs("out-of-layout data LBA was accepted\n", stderr);
        return 1;
    }

    sb->data_lba = TEST_DATA_LBA;
    sector[511] = 1;
    if (classify(sector) != MINIFS_SUPERBLOCK_REJECT_CORRUPT) {
        fputs("unknown reserved superblock metadata was accepted\n", stderr);
        return 1;
    }

    memset(sector, 0, sizeof(sector));
    sector[37] = 0xA5;
    uint8_t before[sizeof(sector)];
    memcpy(before, sector, sizeof(before));
    if (classify(sector) != MINIFS_SUPERBLOCK_REJECT_CORRUPT) {
        fputs("nonblank corrupt superblock was not rejected\n", stderr);
        return 1;
    }
    if (memcmp(before, sector, sizeof(sector)) != 0) {
        fputs("classification modified corrupt media\n", stderr);
        return 1;
    }

    puts("minifs mount policy: corrupt media remains byte-for-byte unchanged");
    return 0;
}
