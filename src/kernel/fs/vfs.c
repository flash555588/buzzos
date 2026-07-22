#include "vfs_internal.h"
#include "task.h"

struct vfs_mount {
    int used;
    const struct fs_ops *ops;
    char path[FS_NAME_LEN];
    int path_len;
};

struct open_file open_files[MAX_TASKS][MAX_FD];
int fd_open_file[MAX_TASKS][MAX_FD];
int fd_used[MAX_TASKS][MAX_FD];
static struct vfs_mount mounts[MAX_MOUNTS];
static volatile int vfs_locked;

void vfs_lock(void) {
    while (__sync_lock_test_and_set(&vfs_locked, 1))
        task_yield();
}

void vfs_unlock(void) {
    __sync_lock_release(&vfs_locked);
}

int nameeq(const char *name, const char *part, int len) {
    int i = 0;
    while (i < len && name[i] && name[i] == part[i]) i++;
    return i == len && name[i] == 0;
}

int vfs_normalize_path(const char *in, char *out, int out_sz) {
    char tmp[128];
    int n = 0;

    if (!in || !in[0])
        in = ".";
    if (in[0] == '/') {
        tmp[n++] = '/';
    } else {
        char cwd[128];
        if (task_get_cwd(cwd, sizeof(cwd)) < 0)
            return -1;
        for (int i = 0; cwd[i] && n < (int)sizeof(tmp) - 1; i++)
            tmp[n++] = cwd[i];
        if (n > 1 && tmp[n - 1] != '/' && n < (int)sizeof(tmp) - 1)
            tmp[n++] = '/';
    }
    for (int i = 0; in[i] && n < (int)sizeof(tmp) - 1; i++)
        tmp[n++] = in[i];
    tmp[n] = 0;

    int len = 1;
    out[0] = '/';
    out[1] = 0;

    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        int part_len = (int)(p - start);
        if (part_len == 1 && start[0] == '.')
            continue;
        if (part_len == 2 && start[0] == '.' && start[1] == '.') {
            if (len > 1) {
                if (out[len - 1] == '/') len--;
                while (len > 1 && out[len - 1] != '/') len--;
                out[len] = 0;
            }
            continue;
        }
        if (len > 1 && out[len - 1] != '/') {
            if (len >= out_sz - 1) return -1;
            out[len++] = '/';
        }
        for (int i = 0; i < part_len; i++) {
            if (len >= out_sz - 1) return -1;
            out[len++] = start[i];
        }
        out[len] = 0;
    }
    if (len > 1 && out[len - 1] == '/')
        out[len - 1] = 0;
    return 0;
}

void copy_name(char *dst, const char *src, int len) {
    int i = 0;
    while (i < len && i < FS_NAME_LEN - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int str_len(const char *s) {
    int n = 0;
    while (s && s[n])
        n++;
    return n;
}

static int path_mount_match(const char *path, const char *mount_path, int mount_len) {
    if (mount_len == 1 && mount_path[0] == '/')
        return path && path[0] == '/';
    if (!path || mount_len <= 0)
        return 0;
    for (int i = 0; i < mount_len; i++) {
        if (path[i] != mount_path[i])
            return 0;
    }
    return path[mount_len] == 0 || path[mount_len] == '/';
}

static struct vfs_mount *find_mount(const char *abs) {
    struct vfs_mount *best = 0;
    int best_len = -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].used)
            continue;
        if (path_mount_match(abs, mounts[i].path, mounts[i].path_len) &&
            mounts[i].path_len > best_len) {
            best = &mounts[i];
            best_len = mounts[i].path_len;
        }
    }
    return best;
}

static const char *mount_relpath(struct vfs_mount *mnt, const char *abs) {
    if (!mnt)
        return 0;
    if (mnt->path_len == 1 && mnt->path[0] == '/')
        return abs;
    return abs[mnt->path_len] ? abs + mnt->path_len : "/";
}

int vfs_mount(const char *path, const struct fs_ops *ops) {
    int len = str_len(path);
    if (len <= 0 || len >= FS_NAME_LEN || !ops)
        return -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].used && nameeq(mounts[i].path, path, len))
            return -1;
    }
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].used) {
            mounts[i].used = 1;
            mounts[i].ops = ops;
            mounts[i].path_len = len;
            copy_name(mounts[i].path, path, len);
            return 0;
        }
    }
    return -1;
}

int current_fd_owner(void) {
    if (!current_task) return 0;
    if (current_task->id < 0 || current_task->id >= MAX_TASKS) return 0;
    if (current_task->fd_owner < 0 || current_task->fd_owner >= MAX_TASKS)
        return current_task->id;
    return current_task->fd_owner;
}

int valid_fd_owner(int owner) {
    return owner >= 0 && owner < MAX_TASKS;
}

int alloc_open_file(int owner);

int alloc_fd_slot(int owner) {
    for (int i = 0; i < MAX_FD; i++) {
        if (!fd_used[owner][i])
            return i;
    }
    return -1;
}

static struct vfs_mount *mount_for_path(const char *abs, const char **rel_out) {
    struct vfs_mount *mnt = find_mount(abs);
    if (!mnt || !mnt->ops)
        return 0;
    if (rel_out)
        *rel_out = mount_relpath(mnt, abs);
    return mnt;
}

int open_can_read(int flags) {
    return (flags & 3) != O_WRONLY;
}

int open_can_write(int flags) {
    int mode = flags & 3;
    return mode == O_WRONLY || mode == O_RDWR;
}

void init_open_file(struct open_file *of, int flags) {
    of->used = 1;
    of->refs = 1;
    of->flags = flags;
    of->vnode.name = 0;
    of->vnode.ops = 0;
    of->vnode.data = 0;
    of->stream.node = 0;
    of->stream.pos = 0;
    of->stream.minifs_ino = 0;
    of->stream.pipe_idx = -1;
    of->stream.pipe_end = 0;
}

static int open_abs_for_owner(int owner, const char *abs, int flags) {
    if (!valid_fd_owner(owner))
        return -1;

    const char *rel;
    struct vfs_mount *mnt = mount_for_path(abs, &rel);
    if (!mnt || !mnt->ops->open)
        return -1;

    int fd = alloc_fd_slot(owner);
    int of_idx = alloc_open_file(owner);
    if (fd < 0 || of_idx < 0)
        return -1;

    struct open_file *of = &open_files[owner][of_idx];
    if (mnt->ops->open(abs, rel, flags, of) < 0)
        return -1;

    if (of->vnode.ops && of->vnode.ops->open)
        of->vnode.ops->open(&of->vnode);
    fd_used[owner][fd] = 1;
    fd_open_file[owner][fd] = of_idx;
    return fd;
}

int alloc_open_file(int owner) {
    for (int i = 0; i < MAX_FD; i++) {
        if (!open_files[owner][i].used)
            return i;
    }
    return -1;
}

struct open_file *fd_to_open_file(int owner, int fd) {
    if (fd < 0 || fd >= MAX_FD || !fd_used[owner][fd])
        return 0;
    int of = fd_open_file[owner][fd];
    if (of < 0 || of >= MAX_FD || !open_files[owner][of].used)
        return 0;
    return &open_files[owner][of];
}

static int close_fd_locked(int owner, int fd) {
    struct open_file *of = fd_to_open_file(owner, fd);
    if (!of)
        return -1;

    fd_used[owner][fd] = 0;
    fd_open_file[owner][fd] = -1;

    if (--of->refs > 0)
        return 0;

    int ret = 0;
    if (of->vnode.ops && of->vnode.ops->close)
        ret = of->vnode.ops->close(&of->vnode);
    of->used = 0;
    of->refs = 0;
    of->flags = 0;
    of->vnode.name = 0;
    of->vnode.ops = 0;
    of->vnode.data = 0;
    of->stream.node = 0;
    of->stream.pos = 0;
    of->stream.minifs_ino = 0;
    of->stream.pipe_idx = -1;
    of->stream.pipe_end = 0;
    return ret;
}

int vfs_open_flags(const char *path, int flags) {
    char abs[128];
    if (vfs_normalize_path(path, abs, sizeof(abs)) < 0)
        return -1;

    vfs_lock();
    int owner = current_fd_owner();
    int fd = open_abs_for_owner(owner, abs, flags);
    vfs_unlock();
    return fd;
}

int vfs_open(const char *path) {
    return vfs_open_flags(path, O_RDONLY);
}

int vfs_read(int fd, void *buf, size_t count) {
    vfs_lock();
    int owner = current_fd_owner();
    struct open_file *of = fd_to_open_file(owner, fd);
    if (!of || !open_can_read(of->flags) || !of->vnode.ops || !of->vnode.ops->read) {
        vfs_unlock();
        return -1;
    }
    int ret = of->vnode.ops->read(&of->vnode, buf, count);
    vfs_unlock();
    return ret;
}

int vfs_write(int fd, const void *buf, size_t count) {
    vfs_lock();
    int owner = current_fd_owner();
    struct open_file *of = fd_to_open_file(owner, fd);
    if (!of || !open_can_write(of->flags) || !of->vnode.ops || !of->vnode.ops->write) {
        vfs_unlock();
        return -1;
    }
    if (of->flags & O_APPEND) {
        size_t size = 0;
        if (!of->vnode.ops->size || of->vnode.ops->size(&of->vnode, &size) < 0) {
            vfs_unlock();
            return -1;
        }
        of->stream.pos = size;
    }
    int ret = of->vnode.ops->write(&of->vnode, buf, count);
    vfs_unlock();
    return ret;
}

int vfs_close(int fd) {
    vfs_lock();
    int owner = current_fd_owner();
    int ret = close_fd_locked(owner, fd);
    vfs_unlock();
    return ret;
}

int vfs_dup(int fd) {
    vfs_lock();
    int owner = current_fd_owner();
    struct open_file *of = fd_to_open_file(owner, fd);
    int newfd = alloc_fd_slot(owner);
    if (!of || newfd < 0) {
        vfs_unlock();
        return -1;
    }
    int of_idx = fd_open_file[owner][fd];
    of->refs++;
    fd_used[owner][newfd] = 1;
    fd_open_file[owner][newfd] = of_idx;
    vfs_unlock();
    return newfd;
}

int vfs_dup2(int oldfd, int newfd) {
    vfs_lock();
    int owner = current_fd_owner();
    struct open_file *of = fd_to_open_file(owner, oldfd);
    if (!of || newfd < 0 || newfd >= MAX_FD) {
        vfs_unlock();
        return -1;
    }
    if (oldfd == newfd) {
        vfs_unlock();
        return newfd;
    }
    if (fd_used[owner][newfd])
        close_fd_locked(owner, newfd);
    int of_idx = fd_open_file[owner][oldfd];
    of->refs++;
    fd_used[owner][newfd] = 1;
    fd_open_file[owner][newfd] = of_idx;
    vfs_unlock();
    return newfd;
}

int vfs_lseek(int fd, int offset, int whence) {
    vfs_lock();
    int owner = current_fd_owner();
    struct open_file *of = fd_to_open_file(owner, fd);
    if (!of) {
        vfs_unlock();
        return -1;
    }

    size_t size = 0;
    if (!of->vnode.ops || !of->vnode.ops->size || of->vnode.ops->size(&of->vnode, &size) < 0) {
        vfs_unlock();
        return -1;
    }

    int base;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = (int)of->stream.pos;
    else if (whence == SEEK_END)
        base = (int)size;
    else {
        vfs_unlock();
        return -1;
    }

    int next = base + offset;
    if (next < 0) {
        vfs_unlock();
        return -1;
    }
    of->stream.pos = (size_t)next;
    vfs_unlock();
    return next;
}

int vfs_stat(const char *path, struct stat *st) {
    if (!st)
        return -1;
    char abs[128];
    if (vfs_normalize_path(path, abs, sizeof(abs)) < 0)
        return -1;
    const char *rel;
    struct vfs_mount *mnt = mount_for_path(abs, &rel);
    if (!mnt || !mnt->ops->stat)
        return -1;
    return mnt->ops->stat(abs, rel, st);
}

int vfs_getdents(int fd, struct dirent *ents, size_t count) {
    if (!ents)
        return -1;
    size_t max_entries = count / sizeof(struct dirent);
    if (max_entries == 0)
        return -1;

    vfs_lock();
    int owner = current_fd_owner();
    struct open_file *of = fd_to_open_file(owner, fd);
    if (!of || !of->vnode.ops || !of->vnode.ops->getdents) {
        vfs_unlock();
        return -1;
    }
    int ret = of->vnode.ops->getdents(&of->vnode, ents, count);
    vfs_unlock();
    return ret;
}

int vfs_mkdir(const char *path) {
    char abs[128];
    if (vfs_normalize_path(path, abs, sizeof(abs)) < 0)
        return -1;
    const char *rel;
    struct vfs_mount *mnt = mount_for_path(abs, &rel);
    if (!mnt || !mnt->ops->mkdir)
        return -1;
    return mnt->ops->mkdir(abs, rel);
}

int vfs_create(const char *path) {
    char abs[128];
    if (vfs_normalize_path(path, abs, sizeof(abs)) < 0)
        return -1;
    const char *rel;
    struct vfs_mount *mnt = mount_for_path(abs, &rel);
    if (!mnt || !mnt->ops->create)
        return -1;
    return mnt->ops->create(abs, rel);
}

int vfs_write_file(const char *path, const void *data, size_t len) {
    int fd = vfs_open_flags(path, O_CREAT | O_TRUNC | O_WRONLY);
    if (fd < 0)
        return -1;
    int ret = vfs_write(fd, data, len);
    vfs_close(fd);
    return ret;
}

int vfs_remove(const char *path) {
    char abs[128];
    if (vfs_normalize_path(path, abs, sizeof(abs)) < 0)
        return -1;
    const char *rel;
    struct vfs_mount *mnt = mount_for_path(abs, &rel);
    if (!mnt || !mnt->ops->unlink)
        return -1;
    return mnt->ops->unlink(abs, rel);
}

int vfs_rmdir(const char *path) {
    char abs[128];
    if (vfs_normalize_path(path, abs, sizeof(abs)) < 0)
        return -1;
    const char *rel;
    struct vfs_mount *mnt = mount_for_path(abs, &rel);
    if (!mnt || !mnt->ops->rmdir)
        return -1;
    return mnt->ops->rmdir(abs, rel);
}

int vfs_rename(const char *old_path, const char *new_path) {
    char old_abs[128];
    char new_abs[128];
    if (vfs_normalize_path(old_path, old_abs, sizeof(old_abs)) < 0 ||
        vfs_normalize_path(new_path, new_abs, sizeof(new_abs)) < 0)
        return -1;
    const char *old_rel;
    const char *new_rel;
    struct vfs_mount *old_mnt = mount_for_path(old_abs, &old_rel);
    struct vfs_mount *new_mnt = mount_for_path(new_abs, &new_rel);
    if (!old_mnt || old_mnt != new_mnt || !old_mnt->ops->rename)
        return -1;
    return old_mnt->ops->rename(old_abs, old_rel, new_abs, new_rel);
}

int vfs_is_dir(const char *path) {
    char abs[128];
    if (vfs_normalize_path(path, abs, sizeof(abs)) < 0)
        return 0;
    const char *rel;
    struct vfs_mount *mnt = mount_for_path(abs, &rel);
    if (!mnt || !mnt->ops->is_dir)
        return 0;
    return mnt->ops->is_dir(abs, rel);
}

int vfs_chdir(const char *path) {
    char abs[128];
    if (vfs_normalize_path(path, abs, sizeof(abs)) < 0)
        return -1;
    if (!vfs_is_dir(abs))
        return -1;
    return task_set_cwd(abs);
}

int vfs_getcwd(char *buf, size_t size) {
    return task_get_cwd(buf, (int)size);
}

void vfs_init(void) {
    vfs_lock();
    for (int i = 0; i < MAX_MOUNTS; i++) mounts[i].used = 0;
    for (int t = 0; t < MAX_TASKS; t++) {
        for (int i = 0; i < MAX_FD; i++) {
            fd_used[t][i] = 0;
            fd_open_file[t][i] = -1;
            open_files[t][i].used = 0;
            open_files[t][i].refs = 0;
            open_files[t][i].flags = 0;
            open_files[t][i].stream.pipe_idx = -1;
            open_files[t][i].stream.pipe_end = 0;
        }
    }
    vfs_unlock();

    ramfs_init();
    pipefs_init();
    devfs_init();
    procfs_init();
    minifs_vfs_init();
}

void vfs_task_reset(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return;
    vfs_lock();
    for (int i = 0; i < MAX_FD; i++)
        if (fd_used[task_id][i])
            close_fd_locked(task_id, i);
    for (int i = 0; i < MAX_FD; i++) {
        fd_used[task_id][i] = 0;
        fd_open_file[task_id][i] = -1;
        open_files[task_id][i].used = 0;
        open_files[task_id][i].refs = 0;
        open_files[task_id][i].flags = 0;
        open_files[task_id][i].stream.pipe_idx = -1;
        open_files[task_id][i].stream.pipe_end = 0;
    }
    vfs_unlock();
}

static int clone_fd_range(int dst_task_id, int src_task_id, int fd_limit) {
    if (!valid_fd_owner(dst_task_id) || !valid_fd_owner(src_task_id))
        return -1;
    if (dst_task_id == src_task_id)
        return 0;
    if (fd_limit < 0)
        return -1;
    if (fd_limit > MAX_FD)
        fd_limit = MAX_FD;

    int refs[MAX_FD];
    for (int i = 0; i < MAX_FD; i++)
        refs[i] = 0;

    vfs_lock();
    for (int i = 0; i < MAX_FD; i++)
        if (fd_used[dst_task_id][i])
            close_fd_locked(dst_task_id, i);
    for (int i = 0; i < MAX_FD; i++) {
        fd_used[dst_task_id][i] = 0;
        fd_open_file[dst_task_id][i] = -1;
        open_files[dst_task_id][i].used = 0;
        open_files[dst_task_id][i].refs = 0;
        open_files[dst_task_id][i].flags = 0;
        open_files[dst_task_id][i].vnode.name = 0;
        open_files[dst_task_id][i].vnode.ops = 0;
        open_files[dst_task_id][i].vnode.data = 0;
        open_files[dst_task_id][i].stream.node = 0;
        open_files[dst_task_id][i].stream.pos = 0;
        open_files[dst_task_id][i].stream.minifs_ino = 0;
        open_files[dst_task_id][i].stream.pipe_idx = -1;
        open_files[dst_task_id][i].stream.pipe_end = 0;
    }

    for (int fd = 0; fd < fd_limit; fd++) {
        if (!fd_used[src_task_id][fd])
            continue;
        int of_idx = fd_open_file[src_task_id][fd];
        if (of_idx < 0 || of_idx >= MAX_FD || !open_files[src_task_id][of_idx].used)
            continue;
        refs[of_idx]++;
    }

    for (int i = 0; i < MAX_FD; i++) {
        if (!refs[i])
            continue;
        struct open_file *src = &open_files[src_task_id][i];
        struct open_file *dst = &open_files[dst_task_id][i];
        *dst = *src;
        dst->refs = refs[i];
        if (src->vnode.data == &src->stream)
            dst->vnode.data = &dst->stream;
        if (dst->stream.pipe_idx >= 0 && dst->stream.pipe_idx < MAX_PIPES) {
            struct pipe_obj *p = &pipes[dst->stream.pipe_idx];
            if (p->used) {
                if (dst->stream.pipe_end == PIPE_READ_END)
                    p->readers++;
                else if (dst->stream.pipe_end == PIPE_WRITE_END)
                    p->writers++;
            }
        }
    }

    for (int fd = 0; fd < fd_limit; fd++) {
        if (!fd_used[src_task_id][fd])
            continue;
        int of_idx = fd_open_file[src_task_id][fd];
        if (of_idx < 0 || of_idx >= MAX_FD || !open_files[dst_task_id][of_idx].used)
            continue;
        fd_used[dst_task_id][fd] = 1;
        fd_open_file[dst_task_id][fd] = of_idx;
    }
    vfs_unlock();
    return 0;
}

int vfs_clone_fd_table(int dst_task_id, int src_task_id) {
    return clone_fd_range(dst_task_id, src_task_id, MAX_FD);
}

int vfs_clone_stdio(int dst_task_id, int src_task_id) {
    return clone_fd_range(dst_task_id, src_task_id, 3);
}

int vfs_setup_stdio(int task_id, int console_silent) {
    if (!valid_fd_owner(task_id))
        return -1;
    const char *path = console_silent ? "/dev/null" : "/dev/console";

    vfs_task_reset(task_id);
    vfs_lock();
    int in_fd = open_abs_for_owner(task_id, path, O_RDWR);
    int out_fd = open_abs_for_owner(task_id, path, O_RDWR);
    int err_fd = open_abs_for_owner(task_id, path, O_RDWR);
    vfs_unlock();

    return (in_fd == 0 && out_fd == 1 && err_fd == 2) ? 0 : -1;
}

void vfs_ls_path(const char *path, void (*putc)(char)) {
    char abs[128];

    if (vfs_normalize_path((path && path[0]) ? path : ".", abs, sizeof(abs)) < 0)
        return;
    const char *rel;
    struct vfs_mount *mnt = mount_for_path(abs, &rel);
    if (!mnt || !mnt->ops->ls)
        return;
    mnt->ops->ls(abs, rel, putc);
}

void vfs_ls(void (*putc)(char)) {
    vfs_ls_path("/", putc);
}

int vfs_cat(const char *path, void (*putc)(char)) {
    int fd = vfs_open(path);
    if (fd < 0) return -1;
    char buf[128];
    int n;
    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0)
        for (int i = 0; i < n; i++) putc(buf[i]);
    vfs_close(fd);
    return n < 0 ? -1 : 0;
}
