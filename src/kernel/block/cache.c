#include "block/cache.h"
#include "block/ata.h"
#include "task.h"

#define CACHE_BLOCKS 64
#define CACHE_FLUSH_BATCH 8
#define SECTOR_SIZE 512

struct cache_entry {
    int valid;
    int dirty;
    uint32_t lba;
    uint32_t age;
    uint8_t data[SECTOR_SIZE];
};

static struct cache_entry cache[CACHE_BLOCKS];
static uint32_t cache_clock;
static volatile int cache_locked;
static int cache_device_dirty;
static uint8_t flush_batch[CACHE_FLUSH_BATCH * SECTOR_SIZE];

static void cache_lock(void) {
    while (__sync_lock_test_and_set(&cache_locked, 1))
        task_yield();
}

static void cache_unlock(void) {
    __sync_lock_release(&cache_locked);
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

static int flush_entry(int idx) {
    if (!cache[idx].valid || !cache[idx].dirty)
        return 0;
    if (ata_write_sector(cache[idx].lba, cache[idx].data) < 0)
        return -1;
    cache[idx].dirty = 0;
    cache_device_dirty = 1;
    return 0;
}

void block_cache_init(void) {
    cache_locked = 0;
    cache_lock();
    for (int i = 0; i < CACHE_BLOCKS; i++) {
        cache[i].valid = 0;
        cache[i].dirty = 0;
    }
    cache_clock = 1;
    cache_device_dirty = 0;
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
    if (flush_entry(idx) < 0) {
        cache_unlock();
        return -1;
    }
    uint8_t sector[SECTOR_SIZE];
    if (ata_read_sector(lba, sector) < 0) {
        cache_unlock();
        return -1;
    }
    cache[idx].valid = 1;
    cache[idx].dirty = 0;
    cache[idx].lba = lba;
    cache[idx].age = ++cache_clock;
    copy_bytes(cache[idx].data, sector, SECTOR_SIZE);
    copy_bytes(buf, cache[idx].data, SECTOR_SIZE);
    cache_unlock();
    return 0;
}

int block_write_sector(uint32_t lba, const void *buf) {
    cache_lock();
    int idx = find_entry(lba);
    if (idx < 0) {
        idx = pick_entry();
        if (flush_entry(idx) < 0) {
            cache_unlock();
            return -1;
        }
    }
    cache[idx].valid = 1;
    cache[idx].dirty = 1;
    cache[idx].lba = lba;
    cache[idx].age = ++cache_clock;
    copy_bytes(cache[idx].data, buf, SECTOR_SIZE);
    cache_unlock();
    return 0;
}

int block_cache_flush(void) {
    cache_lock();
    for (;;) {
        int first = -1;
        for (int i = 0; i < CACHE_BLOCKS; i++) {
            if (!cache[i].valid || !cache[i].dirty)
                continue;
            if (first < 0 || cache[i].lba < cache[first].lba)
                first = i;
        }
        if (first < 0)
            break;

        uint32_t start_lba = cache[first].lba;
        int entries[CACHE_FLUSH_BATCH];
        int count = 0;
        while (count < CACHE_FLUSH_BATCH) {
            int idx = find_entry(start_lba + (uint32_t)count);
            if (idx < 0 || !cache[idx].dirty)
                break;
            entries[count] = idx;
            copy_bytes(flush_batch + count * SECTOR_SIZE,
                       cache[idx].data, SECTOR_SIZE);
            count++;
        }
        if (ata_write_sectors(start_lba, flush_batch, (uint8_t)count) < 0) {
            cache_unlock();
            return -1;
        }
        cache_device_dirty = 1;
        for (int i = 0; i < count; i++)
            cache[entries[i]].dirty = 0;
    }
    int ret = 0;
    if (cache_device_dirty) {
        ret = ata_flush_cache();
        if (ret == 0)
            cache_device_dirty = 0;
    }
    cache_unlock();
    return ret;
}
