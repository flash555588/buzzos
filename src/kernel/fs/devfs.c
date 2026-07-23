#include "keyboard.h"
#include "fb.h"
#include "serial.h"
#include "vfs_internal.h"

struct dev_entry {
    int used;
    char name[FS_NAME_LEN];
    const struct vnode_ops *ops;
    void *data;
};

static struct dev_entry dev_entries[MAX_DEV];

static int dev_name_len(const char *name) {
    int len = 0;
    while (name && name[len])
        len++;
    return len;
}

static const char *dev_rel_name(const char *rel) {
    if (!rel)
        return 0;
    while (*rel == '/')
        rel++;
    return rel;
}

static void dev_copy_dirent_name(char *dst, const char *src) {
    int i = 0;
    while (i < FS_NAME_LEN - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    while (++i < FS_NAME_LEN)
        dst[i] = 0;
}

static struct dev_entry *dev_find(const char *name) {
    int len = dev_name_len(name);
    if (len <= 0)
        return 0;
    for (int i = 0; i < MAX_DEV; i++) {
        if (dev_entries[i].used && nameeq(dev_entries[i].name, name, len))
            return &dev_entries[i];
    }
    return 0;
}

static int serial_open(vnode_t *vn)  { (void)vn; return 0; }
static int serial_close(vnode_t *vn) { (void)vn; return 0; }

static int serial_read_dev(vnode_t *vn, void *buf, size_t count) {
    (void)vn; (void)buf; (void)count;
    return 0;
}

static int serial_write_dev(vnode_t *vn, const void *buf, size_t count) {
    (void)vn;
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < count; i++) serial_putc((char)p[i]);
    return (int)count;
}

static const struct vnode_ops serial_dev_ops = {
    .open  = serial_open,
    .read  = serial_read_dev,
    .write = serial_write_dev,
    .close = serial_close,
};

static int console_open(vnode_t *vn)  { (void)vn; return 0; }
static int console_close(vnode_t *vn) { (void)vn; return 0; }

static int console_read(vnode_t *vn, void *buf, size_t count) {
    (void)vn;
    uint8_t *out = (uint8_t *)buf;
    size_t n = 0;
    while (n < count) {
        int c = keyboard_getchar();
        if (c < 0)
            break;
        out[n++] = (uint8_t)c;
    }
    return (int)n;
}

static int console_write(vnode_t *vn, const void *buf, size_t count) {
    (void)vn;
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < count; i++)
        serial_putc((char)p[i]);
    fb_console_write((const char *)p, count);
    return (int)count;
}

static const struct vnode_ops console_dev_ops = {
    .open  = console_open,
    .read  = console_read,
    .write = console_write,
    .close = console_close,
};

static int keyevent_read(vnode_t *vn, void *buf, size_t count) {
    (void)vn;
    if (count < sizeof(uint16_t)) return 0;
    int event = keyboard_getevent();
    if (event < 0) return 0;
    *(uint16_t *)buf = (uint16_t)event;
    return (int)sizeof(uint16_t);
}

static int keyevent_write(vnode_t *vn, const void *buf, size_t count) {
    (void)vn; (void)buf; (void)count;
    return -1;
}

static const struct vnode_ops keyevent_dev_ops = {
    .open = console_open,
    .read = keyevent_read,
    .write = keyevent_write,
    .close = console_close,
};

static int null_open(vnode_t *vn)   { (void)vn; return 0; }
static int null_close(vnode_t *vn)  { (void)vn; return 0; }

static int null_read(vnode_t *vn, void *buf, size_t count) {
    (void)vn; (void)buf; (void)count;
    return 0;
}

static int null_write(vnode_t *vn, const void *buf, size_t count) {
    (void)vn; (void)buf;
    return (int)count;
}

static const struct vnode_ops null_dev_ops = {
    .open  = null_open,
    .read  = null_read,
    .write = null_write,
    .close = null_close,
};

static int dev_dir_open(vnode_t *vn) { (void)vn; return 0; }
static int dev_dir_close(vnode_t *vn) { (void)vn; return 0; }

static int dev_dir_read(vnode_t *vn, void *buf, size_t count) {
    (void)vn; (void)buf; (void)count;
    return -1;
}

static int dev_dir_write(vnode_t *vn, const void *buf, size_t count) {
    (void)vn; (void)buf; (void)count;
    return -1;
}

static int dev_dir_getdents(vnode_t *vn, struct dirent *ents, size_t count) {
    struct file_stream *s = (struct file_stream *)vn->data;
    size_t max_entries = count / sizeof(struct dirent);
    if (!s || max_entries == 0)
        return -1;

    size_t copied = 0;
    size_t seen = 0;
    for (int i = 0; i < MAX_DEV && copied < max_entries; i++) {
        if (!dev_entries[i].used)
            continue;
        if (seen++ < s->pos)
            continue;
        ents[copied].d_type = DT_CHR;
        ents[copied].d_size = 0;
        dev_copy_dirent_name(ents[copied].d_name, dev_entries[i].name);
        copied++;
        s->pos++;
    }
    return (int)(copied * sizeof(struct dirent));
}

static const struct vnode_ops dev_dir_ops = {
    .open = dev_dir_open,
    .read = dev_dir_read,
    .write = dev_dir_write,
    .getdents = dev_dir_getdents,
    .close = dev_dir_close,
};

static int devfs_open_path(const char *abs, const char *rel, int flags, struct open_file *of) {
    (void)abs;
    const char *name = dev_rel_name(rel);
    if (!name)
        return -1;

    if (!name[0]) {
        if (open_can_write(flags))
            return -1;
        init_open_file(of, flags);
        of->vnode.name = "dev";
        of->vnode.ops = &dev_dir_ops;
        of->vnode.data = &of->stream;
        return 0;
    }

    struct dev_entry *dev = dev_find(name);
    if (!dev || !dev->ops)
        return -1;
    init_open_file(of, flags);
    of->vnode.name = dev->name;
    of->vnode.ops = dev->ops;
    of->vnode.data = dev->data;
    return 0;
}

static int devfs_stat_path(const char *abs, const char *rel, struct stat *st) {
    (void)abs;
    const char *name = dev_rel_name(rel);
    if (!name || !st)
        return -1;
    if (!name[0]) {
        st->st_type = DT_DIR;
        st->st_mode = S_IFDIR | 0755u;
        st->st_size = 0;
        return 0;
    }
    if (!dev_find(name))
        return -1;
    st->st_type = DT_CHR;
    st->st_mode = S_IFCHR | 0666u;
    st->st_size = 0;
    return 0;
}

static int devfs_is_dir_path(const char *abs, const char *rel) {
    (void)abs;
    const char *name = dev_rel_name(rel);
    return (name && !name[0]) ? 1 : 0;
}

static int devfs_ls_path(const char *abs, const char *rel, void (*putc)(char)) {
    (void)abs;
    const char *name = dev_rel_name(rel);
    if (!name)
        return -1;
    if (name[0]) {
        struct dev_entry *dev = dev_find(name);
        if (!dev)
            return -1;
        const char *s = dev->name;
        while (*s) putc(*s++);
        putc('\n');
        return 0;
    }

    char out[512];
    int pos = 0;
    vfs_lock();
    for (int i = 0; i < MAX_DEV; i++) {
        if (!dev_entries[i].used)
            continue;
        const char *s = dev_entries[i].name;
        while (*s && pos < (int)sizeof(out) - 1)
            out[pos++] = *s++;
        if (pos < (int)sizeof(out) - 1)
            out[pos++] = '\n';
    }
    vfs_unlock();

    for (int i = 0; i < pos; i++)
        putc(out[i]);
    return 0;
}

static const struct fs_ops devfs_ops = {
    .open = devfs_open_path,
    .stat = devfs_stat_path,
    .is_dir = devfs_is_dir_path,
    .ls = devfs_ls_path,
};

void devfs_register(const char *name, const struct vnode_ops *ops, void *data) {
    int len = dev_name_len(name);
    if (len <= 0 || len >= FS_NAME_LEN || !ops)
        return;

    vfs_lock();
    if (dev_find(name)) {
        vfs_unlock();
        return;
    }
    for (int i = 0; i < MAX_DEV; i++) {
        if (!dev_entries[i].used) {
            dev_entries[i].used = 1;
            copy_name(dev_entries[i].name, name, len);
            dev_entries[i].ops = ops;
            dev_entries[i].data = data;
            break;
        }
    }
    vfs_unlock();
}

void devfs_init(void) {
    for (int i = 0; i < MAX_DEV; i++)
        dev_entries[i].used = 0;

    devfs_register("serial",  &serial_dev_ops,  0);
    devfs_register("console", &console_dev_ops, 0);
    devfs_register("keyevent", &keyevent_dev_ops, 0);
    devfs_register("null",    &null_dev_ops,    0);
    vfs_mount("/dev", &devfs_ops);
}
