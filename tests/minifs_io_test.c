#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define block_write_sector cached_write_sector
#include "../src/kernel/block/cache.c"
#undef block_write_sector

int block_write_sector(uint32_t lba, const void *buffer);
#include "../src/kernel/fs/minifs/minifs.c"

static uint8_t disk[MINIFS_SECTORS * MINIFS_BLOCK_SIZE];
static uint32_t fail_read_lba, fail_write_lba;
static int read_failures, write_failures, inode_write_attempts;
static uint32_t watched_inode_lba;
static int checks, failures;

static uint8_t *sector_data(uint32_t lba) {
    assert(lba >= MINIFS_LBA_START);
    assert(lba < MINIFS_LBA_START + MINIFS_SECTORS);
    return disk + (size_t)(lba - MINIFS_LBA_START) * MINIFS_BLOCK_SIZE;
}

void task_yield(void) {
    assert(!"Unexpected lock contention");
}

void serial_puts(const char *text) {
    (void)text;
}

int ata_init(void) {
    return 0;
}

int ata_read_sector(uint32_t lba, void *buffer) {
    if (read_failures && lba == fail_read_lba) {
        read_failures--;
        return -1;
    }
    memcpy(buffer, sector_data(lba), MINIFS_BLOCK_SIZE);
    return 0;
}

int ata_write_sector(uint32_t lba, const void *buffer) {
    memcpy(sector_data(lba), buffer, MINIFS_BLOCK_SIZE);
    return 0;
}

int ata_read_sectors(uint32_t lba, void *buffer, uint8_t count) {
    for (uint8_t index = 0; index < count; index++) {
        if (ata_read_sector(lba + index, (uint8_t *)buffer + index * MINIFS_BLOCK_SIZE) < 0)
            return -1;
    }
    return 0;
}

int ata_write_sectors(uint32_t lba, const void *buffer, uint8_t count) {
    for (uint8_t index = 0; index < count; index++)
        ata_write_sector(lba + index, (const uint8_t *)buffer + index * MINIFS_BLOCK_SIZE);
    return 0;
}

int ata_flush_cache(void) {
    return 0;
}

int block_write_sector(uint32_t lba, const void *buffer) {
    if (lba == watched_inode_lba)
        inode_write_attempts++;
    if (write_failures && lba == fail_write_lba) {
        write_failures--;
        return -1;
    }
    return cached_write_sector(lba, buffer);
}

static uint16_t prepare(int directory, size_t blocks) {
    read_failures = write_failures = inode_write_attempts = 0;
    fail_read_lba = fail_write_lba = watched_inode_lba = UINT32_MAX;
    memset(disk, 0, sizeof(disk));
    assert(minifs_mount() == 0);
    assert((directory ? minifs_mkdir("/probe") : minifs_create("/probe")) == 0);
    uint16_t inode;
    assert(minifs_open("/probe", &inode) == 0);
    if (blocks) {
        size_t length = blocks * MINIFS_BLOCK_SIZE, position = 0;
        uint8_t *data = calloc(1, length);
        assert(data != NULL);
        assert(minifs_write(inode, &position, data, length) == (int)length);
        free(data);
    }
    assert(minifs_sync() == 0);
    block_cache_init();
    return inode;
}

static int operate(int operation) {
    if (operation == 0)
        return minifs_unlink("/probe");
    if (operation == 1)
        return minifs_rmdir("/probe");
    return minifs_truncate("/probe");
}

static void check(int condition, const char *name, int operation) {
    checks++;
    if (!condition) {
        failures++;
        printf("FAIL %s operation=%d\n", name, operation);
    }
}

static void normal_operations(void) {
    for (int operation = 0; operation < 3; operation++) {
        uint16_t inode = prepare(operation == 1, operation == 1 ? 0 : 3);
        check(operate(operation) == 0, "normal result", operation);
        assert(minifs_sync() == 0);
        struct minifs_inode persisted;
        memcpy(&persisted, sector_data(inode_lba(inode)), sizeof(persisted));
        check(persisted.size == 0 && (operation == 2 ? persisted.used : !persisted.used),
              "normal persisted inode", operation);
    }
}

static void write_errors(void) {
    for (int operation = 0; operation < 3; operation++) {
        uint16_t inode = prepare(operation == 1, operation == 1 ? 0 : 3);
        watched_inode_lba = inode_lba(inode);
        fail_write_lba = bitmap_lba() + 3;
        write_failures = 1;
        check(operate(operation) == -1 && write_failures == 0,
              "bitmap write error is returned", operation);
        check(inode_write_attempts == 1,
              "inode write still attempted after bitmap error", operation);
        assert(minifs_locked == 0 && cache_locked == 0);
        assert(minifs_sync() == 0);

        inode = prepare(operation == 1, operation == 1 ? 0 : 3);
        watched_inode_lba = fail_write_lba = inode_lba(inode);
        write_failures = 1;
        check(operate(operation) == -1 && write_failures == 0,
              "inode write error is returned", operation);
        assert(minifs_locked == 0 && cache_locked == 0);
        assert(minifs_sync() == 0);
    }
}

static void read_errors(void) {
    for (int operation = 0; operation < 3; operation += 2) {
        uint16_t inode = prepare(0, 9);
        watched_inode_lba = inode_lba(inode);
        fail_read_lba = data_lba(inodes[inode].indirect - 1);
        read_failures = 1;
        check(operate(operation) == -1 && read_failures == 0,
              "single indirect read error is returned", operation);
        check(inode_write_attempts == 1,
              "inode write still attempted after read error", operation);
        assert(minifs_locked == 0 && cache_locked == 0);
    }
    for (int inner = 0; inner < 2; inner++) {
        uint16_t inode = prepare(0, MINIFS_SINGLE_BLOCKS + 1);
        uint16_t first_inner;
        uint32_t outer_lba = data_lba(inodes[inode].double_indirect - 1);
        memcpy(&first_inner, sector_data(outer_lba), sizeof(first_inner));
        fail_read_lba = inner ? data_lba(first_inner - 1) : outer_lba;
        read_failures = 1;
        check(minifs_truncate("/probe") == -1 && read_failures == 0,
              inner ? "double indirect inner read error" : "double indirect outer read error", 2);
        assert(minifs_locked == 0 && cache_locked == 0);
    }
}

static void unsynced_snapshot(void) {
    uint16_t inode = prepare(0, 3);
    struct minifs_inode before = inodes[inode], persisted;
    assert(minifs_unlink("/probe") == 0);
    memcpy(&persisted, sector_data(inode_lba(inode)), sizeof(persisted));
    int dangling = 0;
    for (int index = 0; index < 3; index++) {
        int block = before.block[index] - 1;
        uint8_t marked = sector_data(bitmap_lba() + block / MINIFS_BLOCK_SIZE)[block % MINIFS_BLOCK_SIZE];
        if (persisted.used && persisted.block[index] == before.block[index] && !marked)
            dangling++;
    }
    printf("DIAGNOSTIC unsynced deletion snapshot: %d old inode references to cleared bitmap blocks\n", dangling);
    assert(minifs_sync() == 0);
    memcpy(&persisted, sector_data(inode_lba(inode)), sizeof(persisted));
    check(!persisted.used, "sync persists deletion", 0);
}

int main(void) {
    normal_operations();
    write_errors();
    read_errors();
    unsynced_snapshot();
    printf("%d host production MiniFS/cache checks, %d failures; mock ATA, no guest execution\n", checks, failures);
    return failures ? 1 : 0;
}
