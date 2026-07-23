#ifndef BUZZOS_VFS_H
#define BUZZOS_VFS_H

#include <stddef.h>
#include <stdint.h>

/* Forward decl */
struct vnode;
struct dirent;

/* vnode operations — filesystem implementations fill these in */
struct vnode_ops {
    int  (*open)(struct vnode *vn);
    int  (*read)(struct vnode *vn, void *buf, size_t count);
    int  (*write)(struct vnode *vn, const void *buf, size_t count);
    int  (*getdents)(struct vnode *vn, struct dirent *ents, size_t count);
    int  (*size)(struct vnode *vn, size_t *size_out);
    int  (*close)(struct vnode *vn);
};

/* A vnode — one per open file. name is for devfs lookup. */
struct vnode {
    const char        *name;
    const struct vnode_ops *ops;
    void              *data;    /* fs-private */
};

typedef struct vnode vnode_t;

#define S_IFMT  0170000u
#define S_IFCHR 0020000u
#define S_IFDIR 0040000u
#define S_IFREG 0100000u

#define DT_UNKNOWN 0u
#define DT_CHR     2u
#define DT_DIR     4u
#define DT_REG     8u

#define O_RDONLY 0x0000u
#define O_WRONLY 0x0001u
#define O_RDWR   0x0002u
#define O_CREAT  0x0100u
#define O_TRUNC  0x0200u
#define O_APPEND 0x0400u

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct stat {
    uint32_t st_mode;
    uint32_t st_size;
    uint32_t st_type;
};

struct dirent {
    uint32_t d_type;
    uint32_t d_size;
    char d_name[24];
};

/* Register a device node (devfs). After registration it can be opened
 * by name via vfs_open("/dev/..."). */
void devfs_register(const char *name, const struct vnode_ops *ops,
                    void *data);

/* Open a file by path. Returns fd >= 0, or -1 on error. */
int  vfs_open(const char *path);
int  vfs_open_flags(const char *path, int flags);

/* Read from fd, returns bytes read or -1 on error. */
int  vfs_read(int fd, void *buf, size_t count);

/* Write to fd, returns bytes written or -1 on error. */
int  vfs_write(int fd, const void *buf, size_t count);

/* Close fd. Returns 0 or -1. */
int  vfs_close(int fd);
int  vfs_dup(int fd);
int  vfs_dup2(int oldfd, int newfd);
int  vfs_pipe(int fds[2]);
int  vfs_lseek(int fd, int offset, int whence);
int  vfs_stat(const char *path, struct stat *st);
int  vfs_getdents(int fd, struct dirent *ents, size_t count);

/* Register a ramfs file. Data is referenced (not copied). */
void ramfs_register(const char *name, const uint8_t *data, size_t size);

/* Writable ramfs operations */
int  vfs_create(const char *path);                        /* touch */
int  vfs_write_file(const char *path, const void *data, size_t len); /* append */
int  vfs_remove(const char *path);                        /* rm */
int  vfs_rmdir(const char *path);
int  vfs_rename(const char *old_path, const char *new_path);
int  vfs_mkdir(const char *path);
int  vfs_is_dir(const char *path);
int  vfs_chdir(const char *path);
int  vfs_getcwd(char *buf, size_t size);

/* Initialise VFS and built-in filesystems. */
void vfs_init(void);
void vfs_task_reset(int task_id);
int  vfs_setup_stdio(int task_id, int console_silent);
int  vfs_setup_serial_stdio(int task_id);
int  current_fd_owner(void);
int  vfs_clone_fd_table(int dst_task_id, int src_task_id);
int  vfs_clone_stdio(int dst_task_id, int src_task_id);

void vfs_ls(void (*putc)(char));
void vfs_ls_path(const char *path, void (*putc)(char));
int  vfs_cat(const char *path, void (*putc)(char));

#endif /* BUZZOS_VFS_H */
