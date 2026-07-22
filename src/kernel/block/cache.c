#include "block/cache.h"
#include "block/ata.h"
#include "task.h"

#define CACHE_BLOCKS 16
#define SECTOR_SIZE 512

struct cache_entry {
    int valid;
    uint32_t lba;
    uint32_t age;
    uint8_t data[SECTOR_SIZE];
};

static struct cache_entry cache[CACHE_BLOCKS];
static uint32_t cache_clock;
static volatile int cache_locked;

static void cache_lock(void) {
    task_preempt_disable();
    while (__sync_lock_test_and_set(&cache_locked, 1))
        __asm__ volatile("pause");
}

static void cache_unlock(void) {
    __sync_lock_release(&cache_locked);
    task_preempt_enable();
}

static void copy_bytes(void *dst, const void *src, int len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < len; i++)
        d[i] = s[i];
}

static int find_entry(uint32_t lba) {
    for (int i = 0; i < CACHE_BLOCKS; i++) {
        if (cache[i].valid && cache[i].lba == lba)
            return i;
    }
    return -1;
}

static int pick_entry(void) {
    int best = 0;
    for (int i = 0; i < CACHE_BLOCKS; i++) {
        if (!cache[i].valid)
            return i;
        if (cache[i].age < cache[best].age)
            best = i;
    }
    return best;
}

void block_cache_init(void) {
    cache_locked = 0;
    cache_lock();
    for (int i = 0; i < CACHE_BLOCKS; i++)
        cache[i].valid = 0;
    cache_clock = 1;
    cache_unlock();
}

int block_read_sector(uint32_t lba, void *buf) {
    cache_lock();
    int idx = find_entry(lba);
    if (idx >= 0) {
        cache[idx].age = ++cache_clock;
        copy_bytes(buf, cache[idx].data, SECTOR_SIZE);
        cache_unlock();
        return 0;
    }

    idx = pick_entry();
    if (ata_read_sector(lba, cache[idx].data) < 0) {
        cache_unlock();
        return -1;
    }
    cache[idx].valid = 1;
    cache[idx].lba = lba;
    cache[idx].age = ++cache_clock;
    copy_bytes(buf, cache[idx].data, SECTOR_SIZE);
    cache_unlock();
    return 0;
}

int block_write_sector(uint32_t lba, const void *buf) {
    cache_lock();
    int idx = find_entry(lba);
    if (idx < 0)
        idx = pick_entry();
    cache[idx].valid = 1;
    cache[idx].lba = lba;
    cache[idx].age = ++cache_clock;
    copy_bytes(cache[idx].data, buf, SECTOR_SIZE);
    int ret = ata_write_sector(lba, cache[idx].data);
    cache_unlock();
    return ret;
}

int block_cache_flush(void) {
    return 0;
}
