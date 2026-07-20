#include "minifs.h"
#include "block/ata.h"
#include "block/cache.h"
#include "irq.h"
#include "serial.h"

#define MINIFS_MAGIC       0x5346424Du /* MBFS */
#define MINIFS_BLOCK_SIZE  512
#define MINIFS_INODES      128
#define MINIFS_META_FREE   (MINIFS_SECTORS - 1 - MINIFS_INODES)
#define MINIFS_BITMAP_SECTORS ((MINIFS_META_FREE + MINIFS_BLOCK_SIZE) / (MINIFS_BLOCK_SIZE + 1))
#define MINIFS_BLOCKS      (MINIFS_META_FREE - MINIFS_BITMAP_SECTORS)
#define MINIFS_DIRECT      8
#define MINIFS_INDIRECT_ENTRIES (MINIFS_BLOCK_SIZE / (int)sizeof(uint16_t))
#define MINIFS_MAX_FILE_BLOCKS  (MINIFS_DIRECT + MINIFS_INDIRECT_ENTRIES)
#define MINIFS_MAX_FILE_SIZE    ((size_t)MINIFS_MAX_FILE_BLOCKS * MINIFS_BLOCK_SIZE)
#define MINIFS_ROOT_INO    1
#define MINIFS_NAME_LEN    24

enum {
    MINIFS_FREE = 0,
    MINIFS_DIR = 1,
    MINIFS_FILE = 2,
};

struct minifs_super {
    uint32_t magic;
    uint32_t inode_count;
    uint32_t block_count;
    uint32_t data_lba;
};

struct minifs_inode {
    uint8_t used;
    uint8_t type;
    uint16_t parent;
    uint32_t size;
    uint16_t block[MINIFS_DIRECT];
    uint16_t indirect;
};

_Static_assert(MINIFS_BITMAP_SECTORS > 0, "minifs needs bitmap sectors");
_Static_assert(MINIFS_BLOCKS <= MINIFS_BITMAP_SECTORS * MINIFS_BLOCK_SIZE,
               "minifs block bitmap must fit in reserved sectors");
_Static_assert(sizeof(struct minifs_inode) <= MINIFS_BLOCK_SIZE, "minifs inode must fit in one sector");

struct minifs_dirent_disk {
    uint16_t ino;
    uint8_t type;
    char name[MINIFS_NAME_LEN];
    uint8_t pad[5];
} __attribute__((packed));

static struct minifs_super sb;
static struct minifs_inode inodes[MINIFS_INODES];
static uint8_t block_used[MINIFS_BLOCKS];
static int mounted;
static volatile int minifs_locked;
static uint32_t minifs_irq_flags;

static void minifs_lock(void) {
    uint32_t flags = irq_save();
    while (__sync_lock_test_and_set(&minifs_locked, 1))
        __asm__ volatile("pause");
    minifs_irq_flags = flags;
}

static void minifs_unlock(void) {
    uint32_t flags = minifs_irq_flags;
    __sync_lock_release(&minifs_locked);
    irq_restore(flags);
}

static int nameeq(const char *name, const char *part, int len) {
    int i = 0;
    while (i < len && name[i] && name[i] == part[i])
        i++;
    return i == len && name[i] == 0;
}

static void zero(void *ptr, size_t len) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < len; i++)
        p[i] = 0;
}

static void copy_name(char *dst, const char *src, int len) {
    int i = 0;
    while (i < len && i < MINIFS_NAME_LEN - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    while (++i < MINIFS_NAME_LEN)
        dst[i] = 0;
}

static int valid_leaf(const char *name, int len) {
    (void)name;
    return len > 0 && len < MINIFS_NAME_LEN;
}

static uint32_t inode_lba(int idx) {
    return MINIFS_LBA_START + 1 + (uint32_t)idx;
}

static uint32_t bitmap_lba(void) {
    return MINIFS_LBA_START + 1 + MINIFS_INODES;
}

static uint32_t data_lba(int block) {
    return sb.data_lba + (uint32_t)block;
}

static int flush_super(void) {
    uint8_t sector[512];
    zero(sector, sizeof(sector));
    *(struct minifs_super *)sector = sb;
    return block_write_sector(MINIFS_LBA_START, sector);
}

static int flush_inode(int ino) {
    if (ino <= 0 || ino >= MINIFS_INODES)
        return -1;
    uint8_t sector[512];
    zero(sector, sizeof(sector));
    *(struct minifs_inode *)sector = inodes[ino];
    return block_write_sector(inode_lba(ino), sector);
}

static int flush_bitmap(void) {
    uint8_t sector[512];
    for (int s = 0; s < MINIFS_BITMAP_SECTORS; s++) {
        zero(sector, sizeof(sector));
        for (int i = 0; i < MINIFS_BLOCK_SIZE; i++) {
            int idx = s * MINIFS_BLOCK_SIZE + i;
            if (idx < MINIFS_BLOCKS)
                sector[i] = block_used[idx];
        }
        if (block_write_sector(bitmap_lba() + (uint32_t)s, sector) < 0)
            return -1;
    }
    return 0;
}

static int read_block(int block, void *buf) {
    if (block < 0 || block >= MINIFS_BLOCKS)
        return -1;
    return block_read_sector(data_lba(block), buf);
}

static int write_block(int block, const void *buf) {
    if (block < 0 || block >= MINIFS_BLOCKS)
        return -1;
    return block_write_sector(data_lba(block), buf);
}

static int alloc_inode(uint8_t type, uint16_t parent) {
    for (int i = 2; i < MINIFS_INODES; i++) {
        if (!inodes[i].used) {
            zero(&inodes[i], sizeof(inodes[i]));
            inodes[i].used = 1;
            inodes[i].type = type;
            inodes[i].parent = parent;
            flush_inode(i);
            return i;
        }
    }
    return -1;
}

static int alloc_block(void) {
    uint8_t zero_sector[512];
    zero(zero_sector, sizeof(zero_sector));
    for (int i = 0; i < MINIFS_BLOCKS; i++) {
        if (!block_used[i]) {
            block_used[i] = 1;
            flush_bitmap();
            write_block(i, zero_sector);
            return i;
        }
    }
    return -1;
}

static void free_inode_blocks(int ino) {
    for (int i = 0; i < MINIFS_DIRECT; i++) {
        uint16_t b = inodes[ino].block[i];
        if (b) {
            block_used[b - 1] = 0;
            inodes[ino].block[i] = 0;
        }
    }
    if (inodes[ino].indirect) {
        uint8_t sector[MINIFS_BLOCK_SIZE];
        int indirect_block = inodes[ino].indirect - 1;
        if (read_block(indirect_block, sector) == 0) {
            uint16_t *entries = (uint16_t *)sector;
            for (int i = 0; i < MINIFS_INDIRECT_ENTRIES; i++) {
                if (entries[i])
                    block_used[entries[i] - 1] = 0;
            }
        }
        block_used[indirect_block] = 0;
        inodes[ino].indirect = 0;
    }
    inodes[ino].size = 0;
    flush_bitmap();
}

static int block_for_logical(int ino, int logical) {
    if (logical < 0 || logical >= MINIFS_MAX_FILE_BLOCKS)
        return -1;
    if (logical < MINIFS_DIRECT) {
        if (!inodes[ino].block[logical])
            return -1;
        return inodes[ino].block[logical] - 1;
    }

    if (!inodes[ino].indirect)
        return -1;
    uint8_t sector[MINIFS_BLOCK_SIZE];
    if (read_block(inodes[ino].indirect - 1, sector) < 0)
        return -1;
    uint16_t *entries = (uint16_t *)sector;
    int index = logical - MINIFS_DIRECT;
    if (!entries[index])
        return -1;
    return entries[index] - 1;
}

static int ensure_block(int ino, int logical) {
    if (logical < 0 || logical >= MINIFS_MAX_FILE_BLOCKS)
        return -1;
    if (logical < MINIFS_DIRECT) {
        if (!inodes[ino].block[logical]) {
            int b = alloc_block();
            if (b < 0)
                return -1;
            inodes[ino].block[logical] = (uint16_t)(b + 1);
            flush_inode(ino);
        }
        return inodes[ino].block[logical] - 1;
    }

    if (!inodes[ino].indirect) {
        int table = alloc_block();
        if (table < 0)
            return -1;
        inodes[ino].indirect = (uint16_t)(table + 1);
        flush_inode(ino);
    }

    uint8_t sector[MINIFS_BLOCK_SIZE];
    if (read_block(inodes[ino].indirect - 1, sector) < 0)
        return -1;
    uint16_t *entries = (uint16_t *)sector;
    int index = logical - MINIFS_DIRECT;
    if (!entries[index]) {
        int b = alloc_block();
        if (b < 0)
            return -1;
        entries[index] = (uint16_t)(b + 1);
        if (write_block(inodes[ino].indirect - 1, sector) < 0)
            return -1;
    }
    return entries[index] - 1;
}

static int dir_entry_count(int dir_ino) {
    return (int)(inodes[dir_ino].size / sizeof(struct minifs_dirent_disk));
}

static int dir_read_entry(int dir_ino, int index, struct minifs_dirent_disk *de) {
    uint8_t sector[512];
    size_t off = (size_t)index * sizeof(*de);
    int logical = (int)(off / MINIFS_BLOCK_SIZE);
    int within = (int)(off % MINIFS_BLOCK_SIZE);
    int block = block_for_logical(dir_ino, logical);
    if (block < 0)
        return -1;
    if (read_block(block, sector) < 0)
        return -1;
    *de = *(struct minifs_dirent_disk *)(sector + within);
    return 0;
}

static int dir_write_entry(int dir_ino, int index, const struct minifs_dirent_disk *de) {
    uint8_t sector[512];
    size_t off = (size_t)index * sizeof(*de);
    int logical = (int)(off / MINIFS_BLOCK_SIZE);
    int within = (int)(off % MINIFS_BLOCK_SIZE);
    int block = ensure_block(dir_ino, logical);
    if (block < 0)
        return -1;
    if (read_block(block, sector) < 0)
        return -1;
    *(struct minifs_dirent_disk *)(sector + within) = *de;
    if (write_block(block, sector) < 0)
        return -1;
    if (off + sizeof(*de) > inodes[dir_ino].size) {
        inodes[dir_ino].size = (uint32_t)(off + sizeof(*de));
        flush_inode(dir_ino);
    }
    return 0;
}

static int dir_find(int dir_ino, const char *name, int len) {
    struct minifs_dirent_disk de;
    int count = dir_entry_count(dir_ino);
    for (int i = 0; i < count; i++) {
        if (dir_read_entry(dir_ino, i, &de) == 0 && de.ino && nameeq(de.name, name, len))
            return de.ino;
    }
    return -1;
}

static int dir_add(int dir_ino, const char *name, int len, int child_ino, uint8_t type) {
    if (dir_find(dir_ino, name, len) >= 0)
        return -1;
    struct minifs_dirent_disk de;
    zero(&de, sizeof(de));
    de.ino = (uint16_t)child_ino;
    de.type = type;
    copy_name(de.name, name, len);

    int count = dir_entry_count(dir_ino);
    struct minifs_dirent_disk old;
    for (int i = 0; i < count; i++) {
        if (dir_read_entry(dir_ino, i, &old) == 0 && old.ino == 0)
            return dir_write_entry(dir_ino, i, &de);
    }
    return dir_write_entry(dir_ino, count, &de);
}

static int dir_remove(int dir_ino, const char *name, int len) {
    struct minifs_dirent_disk de;
    int count = dir_entry_count(dir_ino);
    for (int i = 0; i < count; i++) {
        if (dir_read_entry(dir_ino, i, &de) == 0 && de.ino && nameeq(de.name, name, len)) {
            de.ino = 0;
            return dir_write_entry(dir_ino, i, &de);
        }
    }
    return -1;
}

static int dir_is_empty(int dir_ino) {
    struct minifs_dirent_disk de;
    int count = dir_entry_count(dir_ino);
    for (int i = 0; i < count; i++) {
        if (dir_read_entry(dir_ino, i, &de) == 0 && de.ino)
            return 0;
    }
    return 1;
}

static int path_equal(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i])
        i++;
    return a[i] == 0 && b[i] == 0;
}

static int resolve(const char *path) {
    int cur = MINIFS_ROOT_INO;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);
        if (len > 0) {
            if (!inodes[cur].used || inodes[cur].type != MINIFS_DIR)
                return -1;
            cur = dir_find(cur, start, len);
            if (cur < 0)
                return -1;
        }
        while (*p == '/') p++;
    }
    return cur;
}

static int resolve_parent(const char *path, int *parent_out, const char **leaf, int *leaf_len) {
    const char *last = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        last = p;
        while (*p && *p != '/') p++;
    }
    if (!last)
        return -1;

    *leaf = last;
    p = last;
    while (*p && *p != '/') p++;
    *leaf_len = (int)(p - last);

    int cur = MINIFS_ROOT_INO;
    p = path;
    while (*p == '/') p++;
    while (p < last) {
        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);
        while (*p == '/') p++;
        if (p > last) break;
        if (len > 0) {
            cur = dir_find(cur, start, len);
            if (cur < 0 || inodes[cur].type != MINIFS_DIR)
                return -1;
        }
    }
    *parent_out = cur;
    return 0;
}

static void format_fs(void) {
    zero(&sb, sizeof(sb));
    zero(inodes, sizeof(inodes));
    zero(block_used, sizeof(block_used));
    sb.magic = MINIFS_MAGIC;
    sb.inode_count = MINIFS_INODES;
    sb.block_count = MINIFS_BLOCKS;
    sb.data_lba = MINIFS_LBA_START + 1 + MINIFS_INODES + MINIFS_BITMAP_SECTORS;
    inodes[MINIFS_ROOT_INO].used = 1;
    inodes[MINIFS_ROOT_INO].type = MINIFS_DIR;
    inodes[MINIFS_ROOT_INO].parent = MINIFS_ROOT_INO;
    flush_super();
    for (int i = 0; i < MINIFS_INODES; i++)
        flush_inode(i);
    flush_bitmap();
}

int minifs_mount(void) {
    uint8_t sector[512];
    minifs_locked = 0;
    minifs_irq_flags = 0;
    mounted = 0;
    minifs_lock();
    if (ata_init() < 0)
    {
        minifs_unlock();
        return -1;
    }
    block_cache_init();
    if (block_read_sector(MINIFS_LBA_START, sector) < 0)
    {
        minifs_unlock();
        return -1;
    }
    sb = *(struct minifs_super *)sector;
    if (sb.magic != MINIFS_MAGIC || sb.inode_count != MINIFS_INODES ||
        sb.block_count != MINIFS_BLOCKS) {
        serial_puts("[minifs] formatting disk area\n");
        format_fs();
    } else {
        for (int i = 0; i < MINIFS_INODES; i++) {
            if (block_read_sector(inode_lba(i), sector) < 0) {
                minifs_unlock();
                return -1;
            }
            inodes[i] = *(struct minifs_inode *)sector;
        }
        for (int s = 0; s < MINIFS_BITMAP_SECTORS; s++) {
            if (block_read_sector(bitmap_lba() + (uint32_t)s, sector) < 0) {
                minifs_unlock();
                return -1;
            }
            for (int i = 0; i < MINIFS_BLOCK_SIZE; i++) {
                int idx = s * MINIFS_BLOCK_SIZE + i;
                if (idx < MINIFS_BLOCKS)
                    block_used[idx] = sector[i];
            }
        }
    }
    mounted = 1;
    serial_puts("[minifs] mounted /fs\n");
    minifs_unlock();
    return 0;
}

int minifs_info(struct fs_info *out) {
    if (!out)
        return -1;
    minifs_lock();
    if (!mounted) {
        minifs_unlock();
        return -1;
    }

    zero(out, sizeof(*out));
    out->magic = sb.magic;
    out->inode_count = sb.inode_count;
    out->block_count = sb.block_count;
    out->data_lba = sb.data_lba;
    out->max_file_size = (uint32_t)MINIFS_MAX_FILE_SIZE;

    for (int i = 0; i < MINIFS_INODES; i++) {
        if (!inodes[i].used)
            continue;
        out->used_inodes++;
        if (inodes[i].type == MINIFS_DIR)
            out->dir_count++;
        else if (inodes[i].type == MINIFS_FILE)
            out->file_count++;
    }
    for (int i = 0; i < MINIFS_BLOCKS; i++) {
        if (block_used[i])
            out->used_blocks++;
    }
    out->free_blocks = out->block_count - out->used_blocks;
    minifs_unlock();
    return 0;
}

int minifs_open(const char *path, uint16_t *ino_out) {
    minifs_lock();
    if (!mounted || !ino_out) {
        minifs_unlock();
        return -1;
    }
    int ino = resolve(path);
    if (ino < 0) {
        minifs_unlock();
        return -1;
    }
    *ino_out = (uint16_t)ino;
    minifs_unlock();
    return 0;
}

int minifs_create(const char *path) {
    int parent;
    const char *leaf;
    int leaf_len;
    minifs_lock();
    if (resolve_parent(path, &parent, &leaf, &leaf_len) < 0 || !valid_leaf(leaf, leaf_len)) {
        minifs_unlock();
        return -1;
    }
    int old = dir_find(parent, leaf, leaf_len);
    if (old >= 0) {
        int ret = inodes[old].type == MINIFS_FILE ? 0 : -1;
        minifs_unlock();
        return ret;
    }
    int ino = alloc_inode(MINIFS_FILE, (uint16_t)parent);
    if (ino < 0) {
        minifs_unlock();
        return -1;
    }
    if (dir_add(parent, leaf, leaf_len, ino, MINIFS_FILE) < 0) {
        inodes[ino].used = 0;
        flush_inode(ino);
        minifs_unlock();
        return -1;
    }
    minifs_unlock();
    return 0;
}

int minifs_mkdir(const char *path) {
    int parent;
    const char *leaf;
    int leaf_len;
    minifs_lock();
    if (resolve_parent(path, &parent, &leaf, &leaf_len) < 0 || !valid_leaf(leaf, leaf_len)) {
        minifs_unlock();
        return -1;
    }
    if (dir_find(parent, leaf, leaf_len) >= 0) {
        minifs_unlock();
        return -1;
    }
    int ino = alloc_inode(MINIFS_DIR, (uint16_t)parent);
    if (ino < 0) {
        minifs_unlock();
        return -1;
    }
    if (dir_add(parent, leaf, leaf_len, ino, MINIFS_DIR) < 0) {
        inodes[ino].used = 0;
        flush_inode(ino);
        minifs_unlock();
        return -1;
    }
    minifs_unlock();
    return 0;
}

int minifs_unlink(const char *path) {
    int parent;
    const char *leaf;
    int leaf_len;
    minifs_lock();
    if (resolve_parent(path, &parent, &leaf, &leaf_len) < 0 || !valid_leaf(leaf, leaf_len)) {
        minifs_unlock();
        return -1;
    }
    int ino = dir_find(parent, leaf, leaf_len);
    if (ino <= MINIFS_ROOT_INO) {
        minifs_unlock();
        return -1;
    }
    if (inodes[ino].type != MINIFS_FILE) {
        minifs_unlock();
        return -1;
    }
    if (dir_remove(parent, leaf, leaf_len) < 0) {
        minifs_unlock();
        return -1;
    }
    free_inode_blocks(ino);
    zero(&inodes[ino], sizeof(inodes[ino]));
    flush_inode(ino);
    minifs_unlock();
    return 0;
}

int minifs_rmdir(const char *path) {
    int parent;
    const char *leaf;
    int leaf_len;
    minifs_lock();
    if (resolve_parent(path, &parent, &leaf, &leaf_len) < 0 || !valid_leaf(leaf, leaf_len)) {
        minifs_unlock();
        return -1;
    }
    int ino = dir_find(parent, leaf, leaf_len);
    if (ino <= MINIFS_ROOT_INO || inodes[ino].type != MINIFS_DIR || !dir_is_empty(ino)) {
        minifs_unlock();
        return -1;
    }
    if (dir_remove(parent, leaf, leaf_len) < 0) {
        minifs_unlock();
        return -1;
    }
    free_inode_blocks(ino);
    zero(&inodes[ino], sizeof(inodes[ino]));
    flush_inode(ino);
    minifs_unlock();
    return 0;
}

int minifs_rename(const char *old_path, const char *new_path) {
    int old_parent;
    int new_parent;
    const char *old_leaf;
    const char *new_leaf;
    int old_len;
    int new_len;
    minifs_lock();
    if (path_equal(old_path, new_path)) {
        minifs_unlock();
        return 0;
    }
    if (resolve_parent(old_path, &old_parent, &old_leaf, &old_len) < 0 ||
        resolve_parent(new_path, &new_parent, &new_leaf, &new_len) < 0 ||
        !valid_leaf(old_leaf, old_len) || !valid_leaf(new_leaf, new_len)) {
        minifs_unlock();
        return -1;
    }
    int ino = dir_find(old_parent, old_leaf, old_len);
    if (ino <= MINIFS_ROOT_INO || dir_find(new_parent, new_leaf, new_len) >= 0) {
        minifs_unlock();
        return -1;
    }
    if (dir_add(new_parent, new_leaf, new_len, ino, inodes[ino].type) < 0) {
        minifs_unlock();
        return -1;
    }
    if (dir_remove(old_parent, old_leaf, old_len) < 0) {
        minifs_unlock();
        return -1;
    }
    inodes[ino].parent = (uint16_t)new_parent;
    flush_inode(ino);
    minifs_unlock();
    return 0;
}

int minifs_truncate(const char *path) {
    minifs_lock();
    int ino = resolve(path);
    if (ino <= MINIFS_ROOT_INO || inodes[ino].type != MINIFS_FILE) {
        minifs_unlock();
        return -1;
    }
    free_inode_blocks(ino);
    flush_inode(ino);
    minifs_unlock();
    return 0;
}

int minifs_size_ino(uint16_t ino, size_t *size_out) {
    minifs_lock();
    if (!size_out || ino >= MINIFS_INODES || !inodes[ino].used) {
        minifs_unlock();
        return -1;
    }
    *size_out = inodes[ino].size;
    minifs_unlock();
    return 0;
}

int minifs_stat(const char *path, struct stat *st) {
    minifs_lock();
    int ino = resolve(path);
    if (ino < 0 || !st) {
        minifs_unlock();
        return -1;
    }
    st->st_size = inodes[ino].size;
    st->st_type = inodes[ino].type == MINIFS_DIR ? DT_DIR : DT_REG;
    st->st_mode = inodes[ino].type == MINIFS_DIR ? (S_IFDIR | 0755u) : (S_IFREG | 0644u);
    minifs_unlock();
    return 0;
}

int minifs_read(uint16_t ino, size_t *pos, void *buf, size_t count) {
    minifs_lock();
    if (!pos || !buf || ino >= MINIFS_INODES || !inodes[ino].used ||
        inodes[ino].type != MINIFS_FILE) {
        minifs_unlock();
        return -1;
    }
    if (*pos >= inodes[ino].size) {
        minifs_unlock();
        return 0;
    }
    if (count > inodes[ino].size - *pos)
        count = inodes[ino].size - *pos;

    uint8_t sector[512];
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    while (done < count) {
        size_t off = *pos + done;
        int logical = (int)(off / MINIFS_BLOCK_SIZE);
        int within = (int)(off % MINIFS_BLOCK_SIZE);
        int block = block_for_logical(ino, logical);
        if (block < 0)
            break;
        if (read_block(block, sector) < 0) {
            minifs_unlock();
            return -1;
        }
        size_t n = MINIFS_BLOCK_SIZE - (size_t)within;
        if (n > count - done)
            n = count - done;
        for (size_t i = 0; i < n; i++)
            out[done + i] = sector[within + i];
        done += n;
    }
    *pos += done;
    minifs_unlock();
    return (int)done;
}

int minifs_write(uint16_t ino, size_t *pos, const void *buf, size_t count) {
    minifs_lock();
    if (!pos || !buf || ino >= MINIFS_INODES || !inodes[ino].used ||
        inodes[ino].type != MINIFS_FILE) {
        minifs_unlock();
        return -1;
    }
    if (*pos >= MINIFS_MAX_FILE_SIZE) {
        minifs_unlock();
        return -1;
    }
    if (count > MINIFS_MAX_FILE_SIZE - *pos)
        count = MINIFS_MAX_FILE_SIZE - *pos;

    uint8_t sector[512];
    const uint8_t *in = (const uint8_t *)buf;
    size_t done = 0;
    while (done < count) {
        size_t off = *pos + done;
        int logical = (int)(off / MINIFS_BLOCK_SIZE);
        int within = (int)(off % MINIFS_BLOCK_SIZE);
        int block = ensure_block(ino, logical);
        if (block < 0) {
            int ret = done ? (int)done : -1;
            minifs_unlock();
            return ret;
        }
        if (read_block(block, sector) < 0) {
            minifs_unlock();
            return -1;
        }
        size_t n = MINIFS_BLOCK_SIZE - (size_t)within;
        if (n > count - done)
            n = count - done;
        for (size_t i = 0; i < n; i++)
            sector[within + i] = in[done + i];
        if (write_block(block, sector) < 0) {
            minifs_unlock();
            return -1;
        }
        done += n;
    }
    *pos += done;
    if (*pos > inodes[ino].size) {
        inodes[ino].size = (uint32_t)*pos;
        flush_inode(ino);
    }
    minifs_unlock();
    return (int)done;
}

int minifs_getdents(uint16_t ino, size_t *pos, struct dirent *ents, size_t count) {
    minifs_lock();
    if (!pos || !ents || ino >= MINIFS_INODES || !inodes[ino].used ||
        inodes[ino].type != MINIFS_DIR) {
        minifs_unlock();
        return -1;
    }
    size_t max_entries = count / sizeof(struct dirent);
    if (max_entries == 0) {
        minifs_unlock();
        return -1;
    }

    size_t copied = 0;
    int total = dir_entry_count(ino);
    struct minifs_dirent_disk de;
    while (*pos < (size_t)total && copied < max_entries) {
        if (dir_read_entry(ino, (int)*pos, &de) < 0) {
            minifs_unlock();
            return -1;
        }
        (*pos)++;
        if (!de.ino)
            continue;
        ents[copied].d_type = de.type == MINIFS_DIR ? DT_DIR : DT_REG;
        ents[copied].d_size = inodes[de.ino].size;
        copy_name(ents[copied].d_name, de.name, MINIFS_NAME_LEN);
        copied++;
    }
    minifs_unlock();
    return (int)(copied * sizeof(struct dirent));
}

int minifs_is_dir_path(const char *path) {
    minifs_lock();
    int ino = resolve(path);
    int ret = ino >= 0 && inodes[ino].type == MINIFS_DIR;
    minifs_unlock();
    return ret;
}

int minifs_is_dir_ino(uint16_t ino) {
    minifs_lock();
    int ret = ino < MINIFS_INODES && inodes[ino].used && inodes[ino].type == MINIFS_DIR;
    minifs_unlock();
    return ret;
}
