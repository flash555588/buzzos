#include <stdint.h>
#include "paging.h"
#include "pmm.h"
#include "syscall_internal.h"
#include "task.h"

#define SHM_MAX_OBJECTS USER_SHM_SLOTS
#define SHM_MAX_PAGES (USER_SHM_SLOT_SIZE / PAGE_SIZE)

struct shm_object {
    int used, ready;
    uint16_t generation, page_count;
    uint32_t size, owners;
    uintptr_t pages[SHM_MAX_PAGES];
};
static struct shm_object objects[SHM_MAX_OBJECTS];
static volatile int shm_locked;

static void shm_lock(void) {
    while (__sync_lock_test_and_set(&shm_locked, 1))
        task_yield();
}

static void shm_unlock(void) {
    __sync_lock_release(&shm_locked);
}

static int token_index(uint32_t token) {
    int i = (int)(token & 0xFFu) - 1;
    return i >= 0 && i < SHM_MAX_OBJECTS ? i : -1;
}
static uint32_t object_token(int i) {
    return ((uint32_t)objects[i].generation << 8) | (uint32_t)(i + 1);
}
static uint32_t object_va(int i) {
    return USER_SHM_START + (uint32_t)i * USER_SHM_SLOT_SIZE;
}
static struct shm_object *lookup(uint32_t token, int *index) {
    int i = token_index(token);
    if (i < 0 || !objects[i].used || !objects[i].ready ||
        object_token(i) != token) return 0;
    if (index) *index = i;
    return &objects[i];
}
static void free_object(struct shm_object *o) {
    for (uint32_t i = 0; i < o->page_count; i++)
        if (o->pages[i]) pmm_free_pages(o->pages[i], 1);
    o->used = 0; o->ready = 0; o->page_count = 0;
    o->size = 0; o->owners = 0;
}

int sys_shm_create(uint32_t size, uint32_t out_arg, uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    if (!size || size > USER_SHM_SLOT_SIZE ||
        !user_range_writable(out_arg, sizeof(struct syscall_shm_mapping))) return -1;
    int owner = task_get_pid();
    if (owner <= 0 || owner >= MAX_TASKS) return -1;
    shm_lock();
    int index = -1;
    for (int i = 0; i < SHM_MAX_OBJECTS; i++)
        if (!objects[i].used) { index = i; break; }
    if (index < 0) { shm_unlock(); return -1; }
    struct shm_object *o = &objects[index];
    if (++o->generation == 0) o->generation = 1;
    o->used = 1; o->ready = 0; o->size = size;
    o->page_count = (uint16_t)((size + PAGE_SIZE - 1u) / PAGE_SIZE);
    o->owners = 0;
    for (uint32_t i = 0; i < SHM_MAX_PAGES; i++)
        o->pages[i] = 0;
    shm_unlock();

    for (uint32_t i = 0; i < o->page_count; i++) {
        o->pages[i] = pmm_alloc_pages(1);
        if (!o->pages[i]) {
            shm_lock(); free_object(o); shm_unlock(); return -1;
        }
        uint8_t *p = (uint8_t *)(uintptr_t)o->pages[i];
        for (uint32_t j = 0; j < PAGE_SIZE; j++) p[j] = 0;
    }
    uint32_t va = object_va(index);
    if (paging_map_shared_pages(va, o->pages, o->page_count) < 0) {
        shm_lock(); free_object(o); shm_unlock(); return -1;
    }
    shm_lock();
    o->owners = 1u << (uint32_t)owner;
    o->ready = 1;
    struct syscall_shm_mapping *out = (void *)(uintptr_t)out_arg;
    out->token = object_token(index); out->address = va; out->size = size;
    shm_unlock();
    return 0;
}

int sys_shm_map(uint32_t token, uint32_t out_arg, uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    if (!user_range_writable(out_arg, sizeof(struct syscall_shm_mapping))) return -1;
    int owner = task_get_pid(), index;
    if (owner <= 0 || owner >= MAX_TASKS) return -1;
    shm_lock();
    struct shm_object *o = lookup(token, &index);
    uint32_t bit = 1u << (uint32_t)owner;
    if (!o || (o->owners & bit) ||
        paging_map_shared_pages(object_va(index), o->pages, o->page_count) < 0) {
        shm_unlock(); return -1;
    }
    o->owners |= bit;
    struct syscall_shm_mapping *out = (void *)(uintptr_t)out_arg;
    out->token = token; out->address = object_va(index); out->size = o->size;
    shm_unlock();
    return 0;
}

int sys_shm_unmap(uint32_t token, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    int owner = task_get_pid(), index;
    if (owner <= 0 || owner >= MAX_TASKS) return -1;
    shm_lock();
    struct shm_object *o = lookup(token, &index);
    uint32_t bit = 1u << (uint32_t)owner;
    if (!o || !(o->owners & bit) ||
        paging_unmap_shared_pages(paging_current_cr3(), object_va(index), o->page_count) < 0) {
        shm_unlock(); return -1;
    }
    o->owners &= ~bit;
    if (!o->owners) free_object(o);
    shm_unlock();
    return 0;
}

void shm_cleanup_owner(int owner) {
    if (owner <= 0 || owner >= MAX_TASKS) return;
    shm_lock();
    uint32_t bit = 1u << (uint32_t)owner;
    for (int i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (!objects[i].used || !(objects[i].owners & bit)) continue;
        objects[i].owners &= ~bit;
        if (!objects[i].owners) free_object(&objects[i]);
    }
    shm_unlock();
}
