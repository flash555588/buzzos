#include "vfs_internal.h"

struct pipe_obj pipes[MAX_PIPES];

static void pipe_copy(uint8_t *dst, const uint8_t *src, size_t count) {
    for (size_t i = 0; i < count; i++)
        dst[i] = src[i];
}

static int pipe_open(vnode_t *vn) { (void)vn; return 0; }

static uint32_t current_task_bit(void) {
    if (!current_task || current_task->id < 0 || current_task->id >= MAX_TASKS)
        return 0;
    return 1u << (uint32_t)current_task->id;
}

static void pipe_wake_mask(uint32_t *mask) {
    uint32_t waiters = *mask;
    *mask = 0;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (waiters & (1u << (uint32_t)i))
            task_wake(i);
    }
}

static int pipe_wait(struct pipe_obj *p, int end) {
    uint32_t bit = current_task_bit();
    if (!p || !bit)
        return -1;
    if (end == PIPE_READ_END)
        p->read_waiters |= bit;
    else
        p->write_waiters |= bit;

    task_prepare_block_current(0);
    vfs_unlock();
    task_yield();
    vfs_lock();

    if (end == PIPE_READ_END)
        p->read_waiters &= ~bit;
    else
        p->write_waiters &= ~bit;
    return 0;
}

static int pipe_close(vnode_t *vn) {
    struct file_stream *s = (struct file_stream *)vn->data;
    if (!s || s->pipe_idx < 0 || s->pipe_idx >= MAX_PIPES)
        return -1;
    struct pipe_obj *p = &pipes[s->pipe_idx];
    if (!p->used)
        return -1;
    if (s->pipe_end == PIPE_READ_END && p->readers > 0)
        p->readers--;
    if (s->pipe_end == PIPE_WRITE_END && p->writers > 0)
        p->writers--;
    if (p->readers == 0)
        pipe_wake_mask(&p->write_waiters);
    if (p->writers == 0)
        pipe_wake_mask(&p->read_waiters);
    if (p->readers == 0 && p->writers == 0) {
        p->read_waiters = 0;
        p->write_waiters = 0;
        p->used = 0;
    }
    return 0;
}

static int pipe_read(vnode_t *vn, void *buf, size_t count) {
    struct file_stream *s = (struct file_stream *)vn->data;
    if (!s || s->pipe_end != PIPE_READ_END || s->pipe_idx < 0 || s->pipe_idx >= MAX_PIPES)
        return -1;
    struct pipe_obj *p = &pipes[s->pipe_idx];
    if (!p->used)
        return -1;
    if (!buf && count > 0)
        return -1;
    if (count == 0)
        return 0;
    while (p->count == 0 && p->writers > 0) {
        if (pipe_wait(p, PIPE_READ_END) < 0)
            return -1;
        if (!p->used)
            return 0;
    }
    if (p->count == 0)
        return 0;
    uint8_t *out = (uint8_t *)buf;
    size_t done = count < p->count ? count : p->count;
    size_t first = done;
    size_t contiguous = PIPE_BUFSZ - p->tail;
    if (first > contiguous)
        first = contiguous;
    pipe_copy(out, p->data + p->tail, first);
    if (done > first)
        pipe_copy(out + first, p->data, done - first);
    p->tail = (p->tail + done) % PIPE_BUFSZ;
    p->count -= done;
    pipe_wake_mask(&p->write_waiters);
    return (int)done;
}

static int pipe_write(vnode_t *vn, const void *buf, size_t count) {
    struct file_stream *s = (struct file_stream *)vn->data;
    if (!s || s->pipe_end != PIPE_WRITE_END || s->pipe_idx < 0 || s->pipe_idx >= MAX_PIPES)
        return -1;
    struct pipe_obj *p = &pipes[s->pipe_idx];
    if (!p->used || p->readers == 0)
        return -1;
    if (!buf && count > 0)
        return -1;
    if (count == 0)
        return 0;
    const uint8_t *in = (const uint8_t *)buf;
    size_t done = 0;
    while (done < count) {
        while (p->count == PIPE_BUFSZ && p->readers > 0) {
            if (pipe_wait(p, PIPE_WRITE_END) < 0)
                return done ? (int)done : -1;
            if (!p->used)
                return done ? (int)done : -1;
        }
        if (p->readers == 0)
            return done ? (int)done : -1;
        size_t available = PIPE_BUFSZ - p->count;
        size_t chunk = count - done;
        if (chunk > available)
            chunk = available;
        size_t first = chunk;
        size_t contiguous = PIPE_BUFSZ - p->head;
        if (first > contiguous)
            first = contiguous;
        pipe_copy(p->data + p->head, in + done, first);
        if (chunk > first)
            pipe_copy(p->data, in + done + first, chunk - first);
        p->head = (p->head + chunk) % PIPE_BUFSZ;
        p->count += chunk;
        done += chunk;
        pipe_wake_mask(&p->read_waiters);
    }
    return (int)done;
}

static const struct vnode_ops pipe_ops = {
    .open = pipe_open,
    .read = pipe_read,
    .write = pipe_write,
    .close = pipe_close,
};

int vfs_pipe(int fds[2]) {
    if (!fds)
        return -1;
    vfs_lock();
    int owner = current_fd_owner();
    if (!valid_fd_owner(owner)) {
        vfs_unlock();
        return -1;
    }

    int pipe_idx = -1;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) {
            pipe_idx = i;
            break;
        }
    }
    int rfd = alloc_fd_slot(owner);
    if (rfd < 0 || pipe_idx < 0) {
        vfs_unlock();
        return -1;
    }
    fd_used[owner][rfd] = 1;
    int wfd = alloc_fd_slot(owner);
    fd_used[owner][rfd] = 0;
    if (wfd < 0) {
        vfs_unlock();
        return -1;
    }
    int rof = alloc_open_file(owner);
    if (rof < 0) {
        vfs_unlock();
        return -1;
    }
    open_files[owner][rof].used = 1;
    int wof = alloc_open_file(owner);
    open_files[owner][rof].used = 0;
    if (wof < 0) {
        vfs_unlock();
        return -1;
    }

    struct pipe_obj *p = &pipes[pipe_idx];
    p->used = 1;
    p->readers = 1;
    p->writers = 1;
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->read_waiters = 0;
    p->write_waiters = 0;

    struct open_file *r = &open_files[owner][rof];
    init_open_file(r, O_RDONLY);
    r->vnode.name = "pipe";
    r->vnode.ops = &pipe_ops;
    r->stream.pipe_idx = pipe_idx;
    r->stream.pipe_end = PIPE_READ_END;
    r->vnode.data = &r->stream;

    struct open_file *w = &open_files[owner][wof];
    init_open_file(w, O_WRONLY);
    w->vnode.name = "pipe";
    w->vnode.ops = &pipe_ops;
    w->stream.pipe_idx = pipe_idx;
    w->stream.pipe_end = PIPE_WRITE_END;
    w->vnode.data = &w->stream;

    fd_used[owner][rfd] = 1;
    fd_used[owner][wfd] = 1;
    fd_open_file[owner][rfd] = rof;
    fd_open_file[owner][wfd] = wof;
    fds[0] = rfd;
    fds[1] = wfd;
    vfs_unlock();
    return 0;
}

void pipefs_init(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        pipes[i].used = 0;
        pipes[i].read_waiters = 0;
        pipes[i].write_waiters = 0;
    }
}
