#include "minifs.h"
#include "vfs_internal.h"

static int minifs_vn_open(vnode_t *vn) { (void)vn; return 0; }
static int minifs_vn_close(vnode_t *vn) { (void)vn; return minifs_sync(); }

static int minifs_vn_read(vnode_t *vn, void *buf, size_t count) {
    struct file_stream *s = (struct file_stream *)vn->data;
    return minifs_read(s->minifs_ino, &s->pos, buf, count);
}

static int minifs_vn_write(vnode_t *vn, const void *buf, size_t count) {
    struct file_stream *s = (struct file_stream *)vn->data;
    return minifs_write(s->minifs_ino, &s->pos, buf, count);
}

static int minifs_vn_size(vnode_t *vn, size_t *size_out) {
    struct file_stream *s = (struct file_stream *)vn->data;
    if (!s || !size_out)
        return -1;
    return minifs_size_ino(s->minifs_ino, size_out);
}

static int minifs_dir_read(vnode_t *vn, void *buf, size_t count) {
    (void)vn; (void)buf; (void)count;
    return -1;
}

static int minifs_dir_write(vnode_t *vn, const void *buf, size_t count) {
    (void)vn; (void)buf; (void)count;
    return -1;
}

static int minifs_dir_getdents(vnode_t *vn, struct dirent *ents, size_t count) {
    struct file_stream *s = (struct file_stream *)vn->data;
    return minifs_getdents(s->minifs_ino, &s->pos, ents, count);
}

static const struct vnode_ops minifs_file_ops = {
    .open = minifs_vn_open,
    .read = minifs_vn_read,
    .write = minifs_vn_write,
    .size = minifs_vn_size,
    .close = minifs_vn_close,
};

static const struct vnode_ops minifs_dir_ops = {
    .open = minifs_vn_open,
    .read = minifs_dir_read,
    .write = minifs_dir_write,
    .getdents = minifs_dir_getdents,
    .size = minifs_vn_size,
    .close = minifs_vn_close,
};

static int minifs_fs_open(const char *abs, const char *rel, int flags, struct open_file *of) {
    (void)abs;
    if ((flags & O_CREAT) && minifs_create(rel) < 0)
        return -1;
    uint16_t ino;
    if (minifs_open(rel, &ino) < 0)
        return -1;
    int is_dir = minifs_is_dir_ino(ino);
    if (is_dir && open_can_write(flags))
        return -1;
    if ((flags & O_TRUNC) && (!open_can_write(flags) || minifs_truncate(rel) < 0))
        return -1;

    init_open_file(of, flags);
    of->vnode.name = "minifs";
    of->vnode.ops = is_dir ? &minifs_dir_ops : &minifs_file_ops;
    of->stream.minifs_ino = ino;
    if ((flags & O_APPEND) && !is_dir)
        minifs_size_ino(ino, &of->stream.pos);
    of->vnode.data = &of->stream;
    return 0;
}

static int minifs_create_path(const char *abs, const char *rel) {
    (void)abs;
    return minifs_create(rel);
}

static int minifs_mkdir_path(const char *abs, const char *rel) {
    (void)abs;
    return minifs_mkdir(rel);
}

static int minifs_unlink_path(const char *abs, const char *rel) {
    (void)abs;
    return minifs_unlink(rel);
}

static int minifs_rmdir_path(const char *abs, const char *rel) {
    (void)abs;
    return minifs_rmdir(rel);
}

static int minifs_rename_path(const char *old_abs, const char *old_rel,
                              const char *new_abs, const char *new_rel) {
    (void)old_abs; (void)new_abs;
    return minifs_rename(old_rel, new_rel);
}

static int minifs_stat_path(const char *abs, const char *rel, struct stat *st) {
    (void)abs;
    return minifs_stat(rel, st);
}

static int minifs_is_dir_path_op(const char *abs, const char *rel) {
    (void)abs;
    return minifs_is_dir_path(rel);
}

static int minifs_ls_path_op(const char *abs, const char *rel, void (*putc)(char)) {
    uint16_t ino;
    struct dirent ents[8];
    size_t off = 0;
    if (minifs_open(rel, &ino) < 0)
        return -1;
    if (minifs_is_dir_ino(ino)) {
        int n;
        while ((n = minifs_getdents(ino, &off, ents, sizeof(ents))) > 0) {
            int count = n / (int)sizeof(struct dirent);
            for (int i = 0; i < count; i++) {
                const char *s = ents[i].d_name;
                while (*s) putc(*s++);
                if (ents[i].d_type == DT_DIR) putc('/');
                putc('\n');
            }
        }
    } else {
        const char *s = abs;
        const char *name = s;
        while (*s) {
            if (*s == '/') name = s + 1;
            s++;
        }
        while (*name) putc(*name++);
        putc('\n');
    }
    return 0;
}

static const struct fs_ops minifs_ops = {
    .open = minifs_fs_open,
    .create = minifs_create_path,
    .mkdir = minifs_mkdir_path,
    .unlink = minifs_unlink_path,
    .rmdir = minifs_rmdir_path,
    .rename = minifs_rename_path,
    .stat = minifs_stat_path,
    .is_dir = minifs_is_dir_path_op,
    .ls = minifs_ls_path_op,
};

void minifs_vfs_init(void) {
    minifs_mount();
    vfs_mount("/fs", &minifs_ops);
}
