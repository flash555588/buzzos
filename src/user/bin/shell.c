#include "libc.h"

#define LINE_MAX 128
#define HISTORY_MAX 8
#define PIPELINE_MAX 6
#define PIPE_ARG_MAX 15
#define PIPE_TOKEN_MAX 24

#define CTRL_C 0x03

enum {
    KEY_UP = 256,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
};

static int futex_demo_word;
static int futex_block_word;
static int futex_block_ready;
static int futex_timeout_word;
static int pipe_block_fds[2];
static int pipe_block_n;
static int pipe_writer_rc;
static char pipe_block_buf[64];
static char history[HISTORY_MAX][LINE_MAX];
static int history_len;

struct pipeline_cmd {
    char *argv[PIPE_ARG_MAX + 1];
    int argc;
    char path[64];
    char *in_path;
    char *out_path;
    int append;
};

static int starts_with(const char *s, const char *p) {
    while (*p) {
        if (*s++ != *p++)
            return 0;
    }
    return 1;
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static int parse_u32(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static int parse_ipv4(const char *s, uint32_t *out) {
    uint32_t parts[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return -1;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255)
                return -1;
            s++;
        }
        parts[i] = v;
        if (i < 3) {
            if (*s != '.')
                return -1;
            s++;
        }
    }
    if (*s)
        return -1;
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 0;
}

static void print_ipv4(uint32_t ip) {
    printf("%u.%u.%u.%u",
           ip & 0xFFu, (ip >> 8) & 0xFFu, (ip >> 16) & 0xFFu, (ip >> 24) & 0xFFu);
}

static void trim_right(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && s[n - 1] == ' ')
        s[--n] = 0;
}

static int split_args(char *s, char *argv[], int max_args) {
    int argc = 0;
    while (*s && argc < max_args) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        argv[argc++] = s;
        while (*s && *s != ' ')
            s++;
        if (*s)
            *s++ = 0;
    }
    return argc;
}

static void copy_text(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0)
        return;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void append_text(char *dst, const char *src, int cap) {
    int n = (int)strlen(dst);
    int i = 0;
    while (src && src[i] && n < cap - 1)
        dst[n++] = src[i++];
    dst[n] = 0;
}

static void prompt(void) {
    char cwd[128];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "?");
    printf("\nbuzzos:%s> ", cwd);
}

static int read_raw_blocking(void) {
    unsigned char c;
    for (;;) {
        int n = read(0, &c, 1);
        if (n > 0)
            return c;
        yield();
    }
}

static int read_raw_poll(void) {
    unsigned char c;
    int n = read(0, &c, 1);
    if (n > 0)
        return c;
    return -1;
}

static int read_key(void) {
    int c = read_raw_blocking();
    if (c != 0x1B)
        return c;

    int c1 = -1;
    for (int i = 0; i < 20 && c1 < 0; i++) {
        c1 = read_raw_poll();
        if (c1 < 0)
            yield();
    }
    if (c1 != '[')
        return 0x1B;

    int c2 = -1;
    for (int i = 0; i < 20 && c2 < 0; i++) {
        c2 = read_raw_poll();
        if (c2 < 0)
            yield();
    }

    switch (c2) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '3': {
        int c3 = read_raw_poll();
        if (c3 == '~')
            return KEY_DELETE;
        return 0x1B;
    }
    default:
        return 0x1B;
    }
}

static void term_left(int count) {
    while (count-- > 0)
        write(1, "\x1B[D", 3);
}

static void term_right(int count) {
    while (count-- > 0)
        write(1, "\x1B[C", 3);
}

static int utf8_prev(const char *line, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)line[pos] & 0xC0u) == 0x80u) pos--;
    return pos;
}

static int utf8_next(const char *line, int len, int pos) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((unsigned char)line[pos] & 0xC0u) == 0x80u) pos++;
    return pos;
}

static void redraw_from(char *line, int len, int from, int target, int clear_extra) {
    for (int i = from; i < len; i++)
        putchar(line[i]);
    for (int i = 0; i < clear_extra; i++)
        putchar(' ');

    int printed = len - from + clear_extra;
    int keep = target - from;
    if (printed > keep)
        term_left(printed - keep);
}

static void replace_input_line(char *line, int *len, int *cursor, const char *src) {
    int old_len = *len;
    term_left(*cursor);

    int n = 0;
    while (src[n] && n < LINE_MAX - 1) {
        line[n] = src[n];
        n++;
    }
    line[n] = 0;
    *len = n;
    *cursor = n;

    for (int i = 0; i < n; i++)
        putchar(line[i]);
    for (int i = n; i < old_len; i++)
        putchar(' ');
    if (old_len > n)
        term_left(old_len - n);
}

static void history_add(const char *line) {
    if (!line[0])
        return;
    if (history_len > 0 && strcmp(history[history_len - 1], line) == 0)
        return;

    if (history_len == HISTORY_MAX) {
        for (int i = 1; i < HISTORY_MAX; i++)
            strcpy(history[i - 1], history[i]);
        history_len--;
    }
    strcpy(history[history_len++], line);
}

static int read_line(char *line, int size) {
    int len = 0;
    int cursor = 0;
    int hist_index = history_len;
    for (;;) {
        int c = read_key();
        if (c == '\r')
            c = '\n';
        if (c == CTRL_C) {
            term_right(len - cursor);
            puts("^C");
            line[0] = 0;
            return 0;
        }
        if (c == '\n') {
            term_right(len - cursor);
            putchar('\n');
            line[len] = 0;
            return len;
        }
        if (c == KEY_LEFT) {
            if (cursor > 0) {
                int previous = utf8_prev(line, cursor);
                term_left(cursor - previous);
                cursor = previous;
            }
            continue;
        }
        if (c == KEY_RIGHT) {
            if (cursor < len) {
                int next = utf8_next(line, len, cursor);
                term_right(next - cursor);
                cursor = next;
            }
            continue;
        }
        if (c == KEY_HOME) {
            term_left(cursor);
            cursor = 0;
            continue;
        }
        if (c == KEY_END) {
            term_right(len - cursor);
            cursor = len;
            continue;
        }
        if (c == KEY_UP) {
            if (history_len > 0 && hist_index > 0) {
                hist_index--;
                replace_input_line(line, &len, &cursor, history[hist_index]);
            }
            continue;
        }
        if (c == KEY_DOWN) {
            if (hist_index < history_len - 1) {
                hist_index++;
                replace_input_line(line, &len, &cursor, history[hist_index]);
            } else if (hist_index < history_len) {
                hist_index = history_len;
                replace_input_line(line, &len, &cursor, "");
            }
            continue;
        }
        if (c == '\b' || c == 0x7F) {
            if (cursor > 0) {
                int previous = utf8_prev(line, cursor);
                int removed = cursor - previous;
                for (int i = cursor; i < len; i++)
                    line[i - removed] = line[i];
                len -= removed;
                cursor = previous;
                term_left(removed);
                redraw_from(line, len, cursor, cursor, removed);
            }
            continue;
        }
        if (c == KEY_DELETE) {
            if (cursor < len) {
                int next = utf8_next(line, len, cursor);
                int removed = next - cursor;
                for (int i = next; i < len; i++)
                    line[i - removed] = line[i];
                len -= removed;
                redraw_from(line, len, cursor, cursor, removed);
            }
            continue;
        }
        if (c == 0x15) {
            term_right(len - cursor);
            term_left(len);
            for (int i = 0; i < len; i++) putchar(' ');
            term_left(len);
            len = cursor = 0;
            line[0] = 0;
            continue;
        }
        if (c >= 32 && c <= 255 && len < size - 1) {
            for (int i = len; i > cursor; i--)
                line[i] = line[i - 1];
            line[cursor] = (char)c;
            len++;
            redraw_from(line, len, cursor, cursor + 1, 0);
            cursor++;
        }
    }
}

static void cmd_help(const char *topic) {
    topic = skip_spaces(topic);
    if (!topic[0]) {
        puts("commands: ls cd pwd stat about health interfaces limits fsinfo fsstat fdstat cat mkdir rmdir touch write rm mv nano basm lua gui apps ping wget tcptwotest dhcp netstat syncstat badptrtest elfbadtest pipetest pipeedgetest pipeblocktest futextest futextimeouttest futexcanceltest futexblocktest threadreusetest threads exec wait kill ps echo sleep reboot help");
        puts("external: /bin/echo /bin/cat /bin/lua; pipeline: echo hello | cat");
        puts("topics: help apps | help gui | help files | help proc | help edit | help net | help pipes | help lua");
        puts("quick start: about; health; fsinfo; gui");
        return;
    }
    if (strcmp(topic, "apps") == 0) {
        puts("apps [list|info <name>]");
        puts("GUI apps are launched through the desktop.");
        return;
    }
    if (strcmp(topic, "gui") == 0) {
        puts("gui opens the desktop.");
        puts("Desktop manages GUI windows and app launching.");
        return;
    }
    if (strcmp(topic, "files") == 0) {
        puts("/fs is writable minifs: ls cat stat fsinfo fsstat touch write rm mv mkdir rmdir");
        puts("examples: write /fs/hello text; cat /fs/hello; rm /fs/hello");
        return;
    }
    if (strcmp(topic, "proc") == 0) {
        puts("/proc is read-only runtime state");
        puts("try: about; health; limits; interfaces; fsinfo; ls /proc");
        puts("read: cat /proc/about; cat /proc/health; cat /proc/limits; cat /proc/fs");
        puts("more: cat /proc/tasks; cat /proc/threads; cat /proc/fds; cat /proc/net; cat /proc/sync");
        return;
    }
    if (strcmp(topic, "edit") == 0) {
        puts("shell edit: arrows, Home, End, Delete, Backspace, history Up/Down, Ctrl-C");
        puts("gui textboxes: click or Tab focus, type, Backspace/Delete, Home/End, Enter");
        return;
    }
    if (strcmp(topic, "net") == 0) {
        puts("network: dhcp renews IP, netstat shows /proc/net");
        puts("try: ping <ip>, wget <host> [port], tcptwotest <host> <p1> <p2>");
        puts("QEMU user net is enabled by the provided run and smoke scripts");
        return;
    }
    if (strcmp(topic, "pipes") == 0) {
        puts("shell pipelines connect external programs with |");
        puts("redirection: < input, > output, >> append");
        puts("try: echo ok | cat | cat; echo saved > /fs/out; cat < /fs/out");
        return;
    }
    if (strcmp(topic, "lua") == 0) {
        puts("lua [script] [args...]  -- Lua 5.4 interpreter (/bin/lua)");
        puts("try: lua; lua /fs/hello.lua; lua -e \"print(1+1)\"");
        puts("package.path searches ./ /fs /fs/lua /share/lua");
        return;
    }
    puts("help: unknown topic");
}

static void cmd_ls(const char *path) {
    int fd = open(path[0] ? path : ".", O_RDONLY);
    if (fd < 0) {
        puts("ls: not found");
        return;
    }

    struct dirent ents[8];
    int n;
    while ((n = getdents(fd, ents, sizeof(ents))) > 0) {
        int count = n / (int)sizeof(struct dirent);
        for (int i = 0; i < count; i++) {
            printf("%s", ents[i].d_name);
            if (ents[i].d_type == DT_DIR)
                putchar('/');
            putchar('\n');
        }
    }
    close(fd);
}

static void cmd_pwd(void) {
    char cwd[128];
    if (getcwd(cwd, sizeof(cwd)))
        puts(cwd);
    else
        puts("pwd: failed");
}

static void cmd_stat(const char *path) {
    struct stat st;
    if (!path[0] || stat(path, &st) < 0) {
        puts("stat: not found");
        return;
    }
    if ((st.st_mode & S_IFMT) == S_IFDIR)
        printf("dir size %u\n", st.st_size);
    else if ((st.st_mode & S_IFMT) == S_IFREG)
        printf("file size %u\n", st.st_size);
    else if ((st.st_mode & S_IFMT) == S_IFCHR)
        printf("char size %u\n", st.st_size);
    else
        printf("unknown size %u\n", st.st_size);
}

static void cmd_fsstat(void) {
    struct fs_info info;
    if (fsstat(&info) < 0) {
        puts("fsstat: failed");
        return;
    }
    puts("/fs minifs");
    printf("inodes %u/%u dirs %u files %u\n",
           info.used_inodes, info.inode_count,
           info.dir_count, info.file_count);
    printf("blocks %u/%u free %u\n",
           info.used_blocks, info.block_count, info.free_blocks);
    printf("data_lba %u max_file %u\n",
           info.data_lba, info.max_file_size);
}

static void cmd_cat(const char *path) {
    char buf[128];
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        puts("cat: not found");
        return;
    }
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)n);
    close(fd);
    putchar('\n');
}

static void cmd_write_file(const char *args) {
    char path[64];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < (int)sizeof(path) - 1) {
        path[i] = args[i];
        i++;
    }
    path[i] = 0;
    while (args[i] == ' ')
        i++;
    if (!path[0] || !args[i]) {
        puts("write: usage: write <file> <text>");
        return;
    }
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY);
    if (fd < 0) {
        puts("write: open failed");
        return;
    }
    if (write(fd, args + i, strlen(args + i)) < 0)
        puts("write: failed");
    close(fd);
}

static void cmd_exec(const char *args) {
    char local[128];
    int n = 0;
    while (args[n] && n < (int)sizeof(local) - 1) {
        local[n] = args[n];
        n++;
    }
    local[n] = 0;
    trim_right(local);

    char *argv[16];
    int argc = split_args(local, argv, 16);

    if (argc == 0) {
        puts("exec: missing path");
        return;
    }

    int bg = 0;
    if (argc > 1 && strcmp(argv[argc - 1], "&") == 0) {
        bg = 1;
        argc--;
    } else if (argc > 1 && strcmp(argv[argc - 1], "bg") == 0) {
        bg = 1;
        argc--;
    }

    int flags = SPAWN_FLAG_INHERIT_STDIO | (bg ? SPAWN_FLAG_SILENT : 0);
    int pid = spawn_process_args(argv[0], argv, argc, flags);
    if (pid < 0) {
        puts("exec: failed");
        return;
    }
    if (bg) {
        printf("[exec] started task %d\n", pid);
        return;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    printf("[exec] exited %d\n", status);
}

static void cmd_nano(const char *path) {
    while (*path == ' ')
        path++;
    if (!path[0]) {
        puts("nano: usage: nano <file>");
        return;
    }

    char *argv[2];
    argv[0] = "/bin/nano";
    argv[1] = (char *)path;
    int pid = spawn_process_args("/bin/nano", argv, 2, SPAWN_FLAG_INHERIT_STDIO);
    if (pid < 0) {
        puts("nano: failed");
        return;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    printf("[nano] exited %d\n", status);
}

static void cmd_basm(const char *args) {
    char local[128];
    int n = 0;
    while (args[n] && n < (int)sizeof(local) - 1) {
        local[n] = args[n];
        n++;
    }
    local[n] = 0;
    trim_right(local);

    char *argsv[2];
    int argn = split_args(local, argsv, 2);
    if (argn < 1) {
        puts("basm: usage: basm <input.asm> [output]");
        return;
    }

    char *argv[3];
    argv[0] = "/bin/basm";
    argv[1] = argsv[0];
    if (argn >= 2)
        argv[2] = argsv[1];

    int pid = spawn_process_args("/bin/basm", argv, argn + 1, SPAWN_FLAG_INHERIT_STDIO);
    if (pid < 0) {
        puts("basm: failed");
        return;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    printf("[basm] exited %d\n", status);
}

static void run_program_sync(const char *label, const char *path,
                             char **argv, int argc) {
    int pid = spawn_process_args(path, argv, argc, SPAWN_FLAG_INHERIT_STDIO);
    if (pid < 0) {
        printf("%s: failed\n", label);
        return;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    printf("[%s] exited %d\n", label, status);
}

static void cmd_gui(const char *args) {
    (void)args;
    char *argv[1];
    argv[0] = "/bin/gui";
    run_program_sync("gui", "/bin/gui", argv, 1);
}

struct app_meta {
    char file[24];
    char path[64];
    char name[24];
    char kind[18];
    char version[12];
    char summary[48];
    char state[64];
    char source[64];
    char readme[64];
    uint32_t size;
};

static int is_elf_file(const char *path) {
    uint8_t magic[4];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    int n = read(fd, magic, sizeof(magic));
    close(fd);
    return n == 4 && magic[0] == 0x7F && magic[1] == 'E' &&
        magic[2] == 'L' && magic[3] == 'F';
}

static int manifest_key_len(const char *line, int line_len, const char *key) {
    int i = 0;
    while (key[i]) {
        if (i >= line_len || line[i] != key[i])
            return 0;
        i++;
    }
    return i < line_len && line[i] == '=' ? i : 0;
}

static int copy_manifest_value(char *dst, int cap, const char *line,
                               int line_len, const char *key) {
    int key_len = manifest_key_len(line, line_len, key);
    if (!key_len)
        return 0;
    int src = key_len + 1;
    int out = 0;
    while (src < line_len && out < cap - 1)
        dst[out++] = line[src++];
    dst[out] = 0;
    return 1;
}

static void parse_manifest_line(struct app_meta *meta,
                                const char *line,
                                int line_len) {
    if (copy_manifest_value(meta->name, sizeof(meta->name), line, line_len, "name"))
        return;
    if (copy_manifest_value(meta->kind, sizeof(meta->kind), line, line_len, "kind"))
        return;
    if (copy_manifest_value(meta->version, sizeof(meta->version), line, line_len, "version"))
        return;
    if (copy_manifest_value(meta->summary, sizeof(meta->summary), line, line_len, "summary"))
        return;
    if (copy_manifest_value(meta->state, sizeof(meta->state), line, line_len, "state"))
        return;
    if (copy_manifest_value(meta->source, sizeof(meta->source), line, line_len, "source"))
        return;
    (void)copy_manifest_value(meta->readme, sizeof(meta->readme), line, line_len, "readme");
}

static void load_app_manifest(struct app_meta *meta) {
    char manifest_path[80];
    char buf[384];

    copy_text(meta->name, meta->file, sizeof(meta->name));
    copy_text(meta->kind, "user-elf", sizeof(meta->kind));
    copy_text(meta->version, "-", sizeof(meta->version));
    copy_text(meta->summary, "RUNS OUTSIDE BUILTINS", sizeof(meta->summary));
    copy_text(meta->state, "-", sizeof(meta->state));
    copy_text(meta->source, "-", sizeof(meta->source));
    copy_text(meta->readme, "-", sizeof(meta->readme));

    manifest_path[0] = 0;
    append_text(manifest_path, meta->path, sizeof(manifest_path));
    append_text(manifest_path, ".app", sizeof(manifest_path));

    int fd = open(manifest_path, O_RDONLY);
    if (fd < 0)
        return;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;

    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (buf[i] == '\n' || buf[i] == 0) {
            int len = i - start;
            if (len > 0 && buf[start + len - 1] == '\r')
                len--;
            if (len > 0)
                parse_manifest_line(meta, buf + start, len);
            start = i + 1;
        }
    }
}

static int load_app_by_file(const char *file, struct app_meta *meta) {
    meta->file[0] = 0;
    meta->path[0] = 0;
    copy_text(meta->file, file, sizeof(meta->file));
    append_text(meta->path, "/fs/apps/", sizeof(meta->path));
    append_text(meta->path, meta->file, sizeof(meta->path));
    if (!is_elf_file(meta->path))
        return -1;
    struct stat st;
    meta->size = 0;
    if (stat(meta->path, &st) == 0)
        meta->size = st.st_size;
    load_app_manifest(meta);
    return 0;
}

static int find_app(const char *name, struct app_meta *out) {
    int fd = open("/fs/apps", O_RDONLY);
    if (fd < 0)
        return -1;

    struct dirent ents[8];
    int n;
    while ((n = getdents(fd, ents, sizeof(ents))) > 0) {
        int count = n / (int)sizeof(struct dirent);
        for (int i = 0; i < count; i++) {
            if (ents[i].d_type != DT_REG)
                continue;
            struct app_meta meta;
            if (load_app_by_file(ents[i].d_name, &meta) < 0)
                continue;
            if (strcmp(name, meta.file) == 0 || strcmp(name, meta.name) == 0) {
                *out = meta;
                close(fd);
                return 0;
            }
        }
    }
    close(fd);
    return -1;
}

static void print_app_info(const struct app_meta *meta) {
    printf("name    %s\n", meta->name);
    printf("file    %s\n", meta->file);
    printf("kind    %s\n", meta->kind);
    printf("version %s\n", meta->version);
    printf("summary %s\n", meta->summary);
    printf("exec    %s\n", meta->path);
    printf("state   %s\n", meta->state);
    printf("source  %s\n", meta->source);
    printf("readme  %s\n", meta->readme);
    printf("size    %u\n", meta->size);
}

static void cmd_apps(const char *args) {
    while (*args == ' ')
        args++;

    if (starts_with(args, "info ")) {
        struct app_meta meta;
        if (find_app(args + 5, &meta) < 0) {
            puts("apps: not found");
            return;
        }
        print_app_info(&meta);
        return;
    }

    if (args[0] && !starts_with(args, "list")) {
        puts("apps: usage: apps [list|info <name>]");
        return;
    }

    int fd = open("/fs/apps", O_RDONLY);
    if (fd < 0) {
        puts("apps: /fs/apps missing");
        return;
    }
    puts("APP      VERSION  SUMMARY");
    struct dirent ents[8];
    int n;
    int found = 0;
    while ((n = getdents(fd, ents, sizeof(ents))) > 0) {
        int count = n / (int)sizeof(struct dirent);
        for (int i = 0; i < count; i++) {
            if (ents[i].d_type != DT_REG)
                continue;
            struct app_meta meta;
            if (load_app_by_file(ents[i].d_name, &meta) < 0)
                continue;
            printf("%s %s %s\n", meta.name, meta.version, meta.summary);
            found = 1;
        }
    }
    close(fd);
    if (!found)
        puts("(none)");
}

static void cmd_wait(const char *arg) {
    int pid = arg[0] ? parse_u32(arg) : -1;
    int status = 0;
    int got = waitpid(pid == 0 ? -1 : pid, &status, 0);
    if (got < 0) {
        puts("wait: no child");
        return;
    }
    printf("wait: pid %d exited %d\n", got, status);
}

static void cmd_ps(const char *arg) {
    char buf[1024];
    int show_dead = starts_with(arg, "-a");
    if (ps(buf, sizeof(buf), show_dead) < 0) {
        puts("ps: failed");
        return;
    }
    printf("%s", buf);
}

static void cmd_threads(void) {
    cmd_cat("/proc/threads");
}

static volatile int thread_reuse_runs;

static void thread_reuse_worker(void) {
    thread_reuse_runs++;
}

static void cmd_threadreusetest(void) {
    thread_reuse_runs = 0;
    for (int i = 0; i < 40; i++) {
        int tid = spawn(thread_reuse_worker);
        if (tid < 0 || join(tid) < 0) {
            printf("threadreuse: failed at %d\n", i);
            return;
        }
    }
    printf("threadreuse: joined %d\n", thread_reuse_runs);
}

static void cmd_health(void) {
    cmd_cat("/proc/health");
}

static void cmd_about(void) {
    cmd_cat("/proc/about");
}

static void cmd_interfaces(void) {
    cmd_cat("/proc/interfaces");
}

static void cmd_limits(void) {
    cmd_cat("/proc/limits");
}

static void cmd_fsinfo(void) {
    cmd_cat("/proc/fs");
}

static void cmd_netstat(void) {
    cmd_cat("/proc/net");
}

static void cmd_syncstat(void) {
    cmd_cat("/proc/sync");
}

static void cmd_fdstat(void) {
    cmd_cat("/proc/fds");
}

static void append_port_text(char *req, int *pos, int cap, int port) {
    char rev[8];
    int pn = 0;
    int v = port;
    do {
        rev[pn++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && pn < (int)sizeof(rev));
    for (int i = pn - 1; i >= 0 && *pos < cap; i--)
        req[(*pos)++] = rev[i];
}

static int tcp_send_get(int sd, const char *host, int port, const char *path) {
    char req[256];
    int pos = 0;
    const char *a = "GET ";
    while (*a && pos < (int)sizeof(req)) req[pos++] = *a++;
    const char *p = path && path[0] ? path : "/";
    while (*p && pos < (int)sizeof(req)) req[pos++] = *p++;
    const char *b = " HTTP/1.0\r\nHost: ";
    while (*b && pos < (int)sizeof(req)) req[pos++] = *b++;
    const char *h = host;
    while (*h && pos < (int)sizeof(req)) req[pos++] = *h++;
    if (port != 80 && pos < (int)sizeof(req))
        req[pos++] = ':';
    if (port != 80)
        append_port_text(req, &pos, (int)sizeof(req), port);
    const char *tail = "\r\nUser-Agent: BuzzOS-socket/1.0\r\nConnection: close\r\n\r\n";
    while (*tail && pos < (int)sizeof(req)) req[pos++] = *tail++;
    return send(sd, req, (size_t)pos, 0);
}

static int tcp_drain_to_console(int sd) {
    char buf[512];
    for (;;) {
        int n = recv(sd, buf, sizeof(buf), 0);
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
        write(1, buf, (size_t)n);
    }
}

static void cmd_sockget(const char *arg) {
    arg = skip_spaces(arg);
    if (!arg[0]) {
        puts("sockget: usage: wget <host> [port]");
        return;
    }

    char host[96];
    int hlen = 0;
    while (arg[hlen] && arg[hlen] != ' ' && arg[hlen] != '\t' && arg[hlen] != ':' &&
           hlen < (int)sizeof(host) - 1) {
        host[hlen] = arg[hlen];
        hlen++;
    }
    host[hlen] = 0;
    const char *rest = skip_spaces(arg + hlen);
    int port = 80;
    if (*rest == ':')
        rest = skip_spaces(rest + 1);
    if (*rest)
        port = parse_u32(rest);
    if (!host[0] || port <= 0 || port > 65535) {
        puts("sockget: usage: wget <host> [port]");
        return;
    }

    uint32_t ip;
    if (parse_ipv4(host, &ip) < 0 && dns_resolve(host, &ip) < 0) {
        puts("sockget: dns failed");
        return;
    }

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) {
        puts("sockget: socket failed");
        return;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr = ip;
    if (connect(sd, &addr, sizeof(addr)) < 0) {
        puts("sockget: connect failed");
        closesocket(sd);
        return;
    }

    if (tcp_send_get(sd, host, port, "/") < 0) {
        puts("sockget: send failed");
        closesocket(sd);
        return;
    }

    if (tcp_drain_to_console(sd) < 0)
        puts("sockget: recv failed");
    closesocket(sd);
}

static void cmd_tcptwotest(const char *arg) {
    char local[128];
    copy_text(local, arg, sizeof(local));
    char *argv[3];
    if (split_args(local, argv, 3) != 3) {
        puts("tcptwotest: usage: tcptwotest <host> <port-a> <port-b>");
        return;
    }

    int port_a = parse_u32(argv[1]);
    int port_b = parse_u32(argv[2]);
    if (port_a <= 0 || port_a > 65535 || port_b <= 0 || port_b > 65535) {
        puts("tcptwotest: bad port");
        return;
    }

    uint32_t ip;
    if (parse_ipv4(argv[0], &ip) < 0 && dns_resolve(argv[0], &ip) < 0) {
        puts("tcptwotest: resolve failed");
        return;
    }

    int a = socket(AF_INET, SOCK_STREAM, 0);
    int b = socket(AF_INET, SOCK_STREAM, 0);
    if (a < 0 || b < 0) {
        puts("tcptwotest: socket failed");
        if (a >= 0) closesocket(a);
        if (b >= 0) closesocket(b);
        return;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr = ip;
    addr.sin_port = htons((uint16_t)port_a);
    if (connect(a, &addr, sizeof(addr)) < 0) {
        puts("tcptwotest: connect a failed");
        closesocket(a);
        closesocket(b);
        return;
    }
    addr.sin_port = htons((uint16_t)port_b);
    if (connect(b, &addr, sizeof(addr)) < 0) {
        puts("tcptwotest: connect b failed");
        closesocket(a);
        closesocket(b);
        return;
    }

    if (tcp_send_get(a, argv[0], port_a, "/a") < 0 ||
        tcp_send_get(b, argv[0], port_b, "/b") < 0) {
        puts("tcptwotest: send failed");
        closesocket(a);
        closesocket(b);
        return;
    }

    puts("tcptwotest: recv b");
    if (tcp_drain_to_console(b) < 0)
        puts("tcptwotest: recv b failed");
    puts("tcptwotest: recv a");
    if (tcp_drain_to_console(a) < 0)
        puts("tcptwotest: recv a failed");

    closesocket(b);
    closesocket(a);
    puts("tcptwotest: done");
}

static void cmd_ping(const char *target) {
    uint32_t ip;
    if (parse_ipv4(target, &ip) < 0) {
        if (dns_resolve(target, &ip) < 0) {
            puts("ping: resolve failed");
            return;
        }
    }

    int sd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sd < 0) {
        puts("ping: socket failed");
        return;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr = ip;
    if (connect(sd, &addr, sizeof(addr)) < 0) {
        puts("ping: connect failed");
        closesocket(sd);
        return;
    }
    const char payload[] = "buzzos";
    if (send(sd, payload, sizeof(payload), 0) < 0) {
        puts("ping: send failed");
        closesocket(sd);
        return;
    }
    char buf[64];
    int n = recv(sd, buf, sizeof(buf), 0);
    if (n < 0) {
        puts("ping: timeout");
    } else {
        printf("reply from ");
        print_ipv4(ip);
        printf(": %d bytes\n", n);
    }
    closesocket(sd);
}

struct dhcp_msg {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t magic[4];
    uint8_t opts[64];
} __attribute__((packed));

static int dhcp_add_opt(uint8_t *opts, int pos, uint8_t code, const void *data, int len) {
    opts[pos++] = code;
    opts[pos++] = (uint8_t)len;
    const uint8_t *p = (const uint8_t *)data;
    for (int i = 0; i < len; i++)
        opts[pos++] = p[i];
    return pos;
}

static void cmd_dhcp(void) {
    int sd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sd < 0) {
        puts("dhcp: socket failed");
        return;
    }

    struct sockaddr_in local;
    local.sin_family = AF_INET;
    local.sin_port = htons(68);
    local.sin_addr = 0;
    if (bind(sd, &local, sizeof(local)) < 0) {
        puts("dhcp: bind failed");
        closesocket(sd);
        return;
    }

    struct dhcp_msg msg;
    memset(&msg, 0, sizeof(msg));
    uint8_t mac[6];
    uint32_t cur_ip;
    if (net_info(mac, &cur_ip) == 0) {
        for (int i = 0; i < 6; i++)
            msg.chaddr[i] = mac[i];
    }
    msg.op = 1;
    msg.htype = 1;
    msg.hlen = 6;
    msg.xid = 0x78563412u;
    msg.flags = htons(0x8000);
    msg.magic[0] = 99; msg.magic[1] = 130; msg.magic[2] = 83; msg.magic[3] = 99;
    int pos = 0;
    uint8_t type = 1;
    pos = dhcp_add_opt(msg.opts, pos, 53, &type, 1);
    uint8_t params[] = { 1, 3, 6 };
    pos = dhcp_add_opt(msg.opts, pos, 55, params, sizeof(params));
    msg.opts[pos++] = 255;

    struct sockaddr_in dst;
    dst.sin_family = AF_INET;
    dst.sin_port = htons(67);
    dst.sin_addr = INADDR_BROADCAST;
    if (sendto(sd, &msg, sizeof(msg), 0, &dst, sizeof(dst)) < 0) {
        puts("dhcp: discover send failed");
        closesocket(sd);
        return;
    }

    struct sockaddr_in from;
    int n = recvfrom(sd, &msg, sizeof(msg), 0, &from, sizeof(from));
    if (n < 0) {
        puts("dhcp: no offer");
        closesocket(sd);
        return;
    }
    printf("dhcp offer ");
    print_ipv4(msg.yiaddr);
    putchar('\n');
    closesocket(sd);
}

static void cmd_pipetest(void) {
    int fds[2];
    if (pipe(fds) < 0) {
        puts("pipe: failed");
        return;
    }
    const char *msg = "hello through pipe";
    if (write(fds[1], msg, strlen(msg)) < 0) {
        puts("pipe: write failed");
        close(fds[0]);
        close(fds[1]);
        return;
    }
    char buf[64];
    int n = read(fds[0], buf, sizeof(buf) - 1);
    if (n < 0) {
        puts("pipe: read failed");
    } else {
        buf[n] = 0;
        printf("pipe: %s\n", buf);
    }
    close(fds[0]);
    close(fds[1]);
}

static void cmd_pipeedgetest(void) {
    int fds[2];
    char ch = 0;
    if (pipe(fds) < 0) {
        puts("pipeedge: pipe failed");
        return;
    }

    close(fds[1]);
    int eof = read(fds[0], &ch, 1);
    close(fds[0]);
    printf("pipeedge: eof %d\n", eof);

    if (pipe(fds) < 0) {
        puts("pipeedge: pipe failed");
        return;
    }
    close(fds[0]);
    int no_reader = write(fds[1], "x", 1);
    close(fds[1]);
    printf("pipeedge: no-reader %d\n", no_reader);
}

static void pipe_block_reader_thread(void) {
    pipe_block_n = read(pipe_block_fds[0], pipe_block_buf, sizeof(pipe_block_buf) - 1);
    if (pipe_block_n >= 0 && pipe_block_n < (int)sizeof(pipe_block_buf))
        pipe_block_buf[pipe_block_n] = 0;
    else
        pipe_block_buf[0] = 0;
}

static void pipe_block_writer_thread(void) {
    char big[600];
    for (int i = 0; i < (int)sizeof(big); i++)
        big[i] = (char)('a' + (i % 26));
    pipe_writer_rc = write(pipe_block_fds[1], big, sizeof(big));
}

static void cmd_pipeblocktest(void) {
    int fds[2];
    if (pipe(fds) < 0) {
        puts("pipeblock: pipe failed");
        return;
    }
    pipe_block_fds[0] = fds[0];
    pipe_block_fds[1] = fds[1];
    pipe_block_n = -9;
    pipe_block_buf[0] = 0;
    int tid = spawn(pipe_block_reader_thread);
    if (tid < 0) {
        puts("pipeblock: reader spawn failed");
        close(fds[0]);
        close(fds[1]);
        return;
    }
    sleep_ms(100);
    int wrote = write(fds[1], "wake", 4);
    join(tid);
    close(fds[0]);
    close(fds[1]);
    printf("pipeblock: reader %d write %d %s\n", pipe_block_n, wrote, pipe_block_buf);

    if (pipe(fds) < 0) {
        puts("pipeblock: pipe failed");
        return;
    }
    pipe_block_fds[0] = fds[0];
    pipe_block_fds[1] = fds[1];
    pipe_writer_rc = -9;
    tid = spawn(pipe_block_writer_thread);
    if (tid < 0) {
        puts("pipeblock: writer spawn failed");
        close(fds[0]);
        close(fds[1]);
        return;
    }
    sleep_ms(100);
    char drain[512];
    int first = read(fds[0], drain, sizeof(drain));
    join(tid);
    int second = read(fds[0], drain, 128);
    close(fds[0]);
    close(fds[1]);
    printf("pipeblock: writer %d drain %d+%d\n", pipe_writer_rc, first, second);
}

static void put16le(uint8_t *buf, int off, uint16_t value) {
    buf[off] = (uint8_t)(value & 0xFFu);
    buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void put32le(uint8_t *buf, int off, uint32_t value) {
    buf[off] = (uint8_t)(value & 0xFFu);
    buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
    buf[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
    buf[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

static int make_bad_elf(uint8_t *buf, int cap, int kind) {
    if (!buf || cap < 85)
        return -1;
    for (int i = 0; i < cap; i++)
        buf[i] = 0;

    buf[0] = 0x7F;
    buf[1] = 'E';
    buf[2] = 'L';
    buf[3] = 'F';
    buf[4] = 1;  /* ELFCLASS32 */
    buf[5] = 1;  /* little-endian */
    buf[6] = 1;  /* EV_CURRENT */
    put16le(buf, 16, 2);              /* ET_EXEC */
    put16le(buf, 18, 3);              /* EM_386 */
    put32le(buf, 20, 1);              /* e_version */
    put32le(buf, 24, 0x001C0000u);    /* e_entry */
    put32le(buf, 28, 52);             /* e_phoff */
    put16le(buf, 40, 52);             /* e_ehsize */
    put16le(buf, 42, 32);             /* e_phentsize */
    put16le(buf, 44, 1);              /* e_phnum */

    put32le(buf, 52, 1);              /* PT_LOAD */
    put32le(buf, 56, 84);             /* p_offset */
    put32le(buf, 60, 0x001C0000u);    /* p_vaddr */
    put32le(buf, 68, 1);              /* p_filesz */
    put32le(buf, 72, 1);              /* p_memsz */
    put32le(buf, 76, 5);              /* PF_R | PF_X */
    put32le(buf, 80, 1);              /* p_align */
    buf[84] = 0xC3;                   /* ret, never reached by these tests */

    if (kind == 0) {
        put32le(buf, 24, 0x00001000u);
        put32le(buf, 60, 0x00001000u);
    } else if (kind == 1) {
        put32le(buf, 68, 64);
        return 85;
    } else if (kind == 2) {
        put32le(buf, 68, 2);
        put32le(buf, 72, 1);
    } else if (kind == 3) {
        put32le(buf, 24, 0x001D0000u);
    }
    return 85;
}

static int run_bad_elf_case(const char *label, int kind) {
    const char *path = "/fs/badelf.bin";
    uint8_t image[128];
    int size = make_bad_elf(image, sizeof(image), kind);
    if (size < 0) {
        printf("elfbad: %s build failed\n", label);
        return -1;
    }

    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY);
    if (fd < 0) {
        printf("elfbad: %s open failed\n", label);
        return -1;
    }
    int wrote = write(fd, image, (size_t)size);
    close(fd);
    if (wrote != size) {
        printf("elfbad: %s write failed\n", label);
        unlink(path);
        return -1;
    }

    char *argv[1];
    argv[0] = (char *)path;
    int pid = spawn_process_args(path, argv, 1, SPAWN_FLAG_SILENT);
    unlink(path);
    printf("elfbad: %s %d\n", label, pid);
    return pid < 0 ? 0 : -1;
}

static void cmd_elfbadtest(void) {
    int ok = 1;
    if (run_bad_elf_case("vaddr", 0) < 0) ok = 0;
    if (run_bad_elf_case("filesz", 1) < 0) ok = 0;
    if (run_bad_elf_case("memsz", 2) < 0) ok = 0;
    if (run_bad_elf_case("entry", 3) < 0) ok = 0;
    printf("elfbad: %s\n", ok ? "ok" : "failed");
}

static void cmd_badptrtest(void) {
    const char *unmapped = (const char *)0x02700000u;
    int rc = write(1, unmapped, 16);
    printf("badptr: rejected %d\n", rc);
}

static void resolve_external_path(const char *cmd, char *out, int cap) {
    if (!cmd || !cmd[0] || cap <= 0)
        return;
    if (cmd[0] == '/' || cmd[0] == '.') {
        copy_text(out, cmd, cap);
        return;
    }
    copy_text(out, "/bin/", cap);
    append_text(out, cmd, cap);
}

static void run_external_command(const char *line) {
    char local[LINE_MAX];
    char path[128];
    char *argv[16];
    copy_text(local, line, sizeof(local));
    int argc = split_args(local, argv, 16);
    if (argc <= 0)
        return;
    resolve_external_path(argv[0], path, sizeof(path));
    argv[0] = path;
    run_program_sync(local, path, argv, argc);
}

static int has_exec_operator(const char *line, int *has_pipe) {
    int found = 0;
    if (has_pipe)
        *has_pipe = 0;
    for (int i = 0; line[i]; i++) {
        if (line[i] == '|') {
            found = 1;
            if (has_pipe)
                *has_pipe = 1;
        } else if (line[i] == '<' || line[i] == '>') {
            found = 1;
        }
    }
    return found;
}

static int parse_pipeline_cmd(char *stage, struct pipeline_cmd *cmd) {
    char *tokens[PIPE_TOKEN_MAX];
    cmd->argc = 0;
    cmd->path[0] = 0;
    cmd->in_path = 0;
    cmd->out_path = 0;
    cmd->append = 0;
    for (int i = 0; i <= PIPE_ARG_MAX; i++)
        cmd->argv[i] = 0;

    int ntok = split_args(stage, tokens, PIPE_TOKEN_MAX);
    for (int i = 0; i < ntok; i++) {
        char *tok = tokens[i];
        if (strcmp(tok, "<") == 0) {
            if (i + 1 >= ntok)
                return -1;
            cmd->in_path = tokens[++i];
            continue;
        }
        if (tok[0] == '<' && tok[1]) {
            cmd->in_path = tok + 1;
            continue;
        }
        if (strcmp(tok, ">") == 0) {
            if (i + 1 >= ntok)
                return -1;
            cmd->out_path = tokens[++i];
            cmd->append = 0;
            continue;
        }
        if (strcmp(tok, ">>") == 0) {
            if (i + 1 >= ntok)
                return -1;
            cmd->out_path = tokens[++i];
            cmd->append = 1;
            continue;
        }
        if (tok[0] == '>' && tok[1] == '>') {
            if (tok[2]) {
                cmd->out_path = tok + 2;
            } else {
                if (i + 1 >= ntok)
                    return -1;
                cmd->out_path = tokens[++i];
            }
            cmd->append = 1;
            continue;
        }
        if (tok[0] == '>' && tok[1]) {
            cmd->out_path = tok + 1;
            cmd->append = 0;
            continue;
        }
        if (cmd->argc >= PIPE_ARG_MAX)
            return -1;
        cmd->argv[cmd->argc++] = tok;
    }

    if (cmd->argc <= 0)
        return -1;
    resolve_external_path(cmd->argv[0], cmd->path, sizeof(cmd->path));
    cmd->argv[0] = cmd->path;
    cmd->argv[cmd->argc] = 0;
    return 0;
}

static void restore_stdio(int saved_in, int saved_out) {
    if (saved_in >= 0)
        dup2(saved_in, 0);
    if (saved_out >= 0)
        dup2(saved_out, 1);
}

static void stop_spawned(int pids[], int count) {
    for (int i = 0; i < count; i++)
        kill(pids[i]);
    for (int i = 0; i < count; i++) {
        int status = 0;
        waitpid(pids[i], &status, 0);
    }
}

static void print_pipeline_status(int statuses[], int count) {
    if (count <= 0)
        return;
    printf("[pipe] exited %d", statuses[0]);
    for (int i = 1; i < count; i++)
        printf(" | %d", statuses[i]);
    putchar('\n');
}

static int execute_pipeline(char *line) {
    int has_pipe = 0;
    if (!has_exec_operator(line, &has_pipe))
        return 0;

    char *stages[PIPELINE_MAX];
    int stage_count = 0;
    stages[stage_count++] = line;
    for (int i = 0; line[i]; i++) {
        if (line[i] == '|') {
            if (stage_count >= PIPELINE_MAX) {
                puts("pipe: too many stages");
                return 1;
            }
            line[i] = 0;
            stages[stage_count++] = line + i + 1;
        }
    }

    struct pipeline_cmd cmds[PIPELINE_MAX];
    for (int i = 0; i < stage_count; i++) {
        char *stage = (char *)skip_spaces(stages[i]);
        trim_right(stage);
        if (!stage[0] || parse_pipeline_cmd(stage, &cmds[i]) < 0) {
            puts("pipe: usage: program [args] [< in] [| program ...] [> out]");
            return 1;
        }
        if (i > 0 && cmds[i].in_path) {
            puts("pipe: input redirection is only supported on the first stage");
            return 1;
        }
        if (i < stage_count - 1 && cmds[i].out_path) {
            puts("pipe: output redirection is only supported on the last stage");
            return 1;
        }
    }

    int saved_in = dup(0);
    int saved_out = dup(1);
    if (saved_in < 0 || saved_out < 0) {
        puts("pipe: dup failed");
        if (saved_in >= 0) close(saved_in);
        if (saved_out >= 0) close(saved_out);
        return 1;
    }

    int pids[PIPELINE_MAX];
    int pid_count = 0;
    int prev_read = -1;

    for (int i = 0; i < stage_count; i++) {
        int next_read = -1;
        int next_write = -1;
        int opened_in = -1;
        int opened_out = -1;
        int failed = 0;
        const char *error = 0;

        if (i < stage_count - 1) {
            int fds[2];
            if (pipe(fds) < 0) {
                failed = 1;
                error = "pipe: failed";
            } else {
                next_read = fds[0];
                next_write = fds[1];
            }
        }

        if (!failed) {
            if (cmds[i].in_path) {
                opened_in = open(cmds[i].in_path, O_RDONLY);
                if (opened_in < 0 || dup2(opened_in, 0) < 0) {
                    failed = 1;
                    error = "pipe: input redirect failed";
                }
            } else if (prev_read >= 0) {
                if (dup2(prev_read, 0) < 0) {
                    failed = 1;
                    error = "pipe: input redirect failed";
                }
            } else if (dup2(saved_in, 0) < 0) {
                failed = 1;
                error = "pipe: input restore failed";
            }
        }

        if (!failed) {
            if (cmds[i].out_path) {
                int flags = O_CREAT | O_WRONLY | (cmds[i].append ? O_APPEND : O_TRUNC);
                opened_out = open(cmds[i].out_path, flags);
                if (opened_out < 0 || dup2(opened_out, 1) < 0) {
                    failed = 1;
                    error = "pipe: output redirect failed";
                }
            } else if (next_write >= 0) {
                if (dup2(next_write, 1) < 0) {
                    failed = 1;
                    error = "pipe: output redirect failed";
                }
            } else if (dup2(saved_out, 1) < 0) {
                failed = 1;
                error = "pipe: output restore failed";
            }
        }

        int pid = -1;
        if (!failed) {
            pid = spawn_process_args(cmds[i].path, cmds[i].argv, cmds[i].argc,
                                     SPAWN_FLAG_INHERIT_STDIO);
            if (pid < 0) {
                failed = 1;
                error = "pipe: spawn failed";
            }
        }

        restore_stdio(saved_in, saved_out);
        if (opened_in >= 0) close(opened_in);
        if (opened_out >= 0) close(opened_out);
        if (prev_read >= 0) close(prev_read);
        if (next_write >= 0) close(next_write);

        if (failed) {
            if (next_read >= 0) close(next_read);
            close(saved_in);
            close(saved_out);
            if (error)
                puts(error);
            stop_spawned(pids, pid_count);
            return 1;
        }

        pids[pid_count++] = pid;
        prev_read = next_read;
    }

    close(saved_in);
    close(saved_out);

    int statuses[PIPELINE_MAX];
    for (int i = 0; i < pid_count; i++) {
        statuses[i] = -1;
        waitpid(pids[i], &statuses[i], 0);
    }
    if (has_pipe)
        print_pipeline_status(statuses, pid_count);
    return 1;
}

static void futex_demo_thread(void) {
    sleep_ms(200);
    futex_demo_word = 1;
    futex_wake(&futex_demo_word, 1);
}

static void cmd_futextest(void) {
    futex_demo_word = 0;
    int tid = spawn(futex_demo_thread);
    if (tid < 0) {
        puts("futex: spawn failed");
        return;
    }
    while (futex_demo_word == 0)
        futex_wait(&futex_demo_word, 0);
    join(tid);
    puts("futex: woke");
}

static void futex_block_thread(void) {
    futex_block_ready = 1;
    futex_wait(&futex_block_word, 0);
}

static void cmd_futexblocktest(void) {
    futex_block_word = 0;
    futex_block_ready = 0;
    int tid = spawn(futex_block_thread);
    if (tid < 0) {
        puts("futexblock: spawn failed");
        return;
    }
    for (int i = 0; i < 20 && !futex_block_ready; i++)
        sleep_ms(10);
    sleep_ms(50);
    puts("futexblock: waiting threads");
    cmd_threads();
    cmd_syncstat();
    futex_block_word = 1;
    int woke = futex_wake(&futex_block_word, 1);
    join(tid);
    printf("futexblock: woke %d\n", woke);
}

static void futex_timeout_waker(void) {
    sleep_ms(100);
    futex_timeout_word = 1;
    futex_wake(&futex_timeout_word, 1);
}

static void cmd_futextimeouttest(void) {
    futex_timeout_word = 0;
    int rc = futex_wait_timeout(&futex_timeout_word, 0, 50);
    printf("futextimeout: timeout %d\n", rc);

    futex_timeout_word = 0;
    int tid = spawn(futex_timeout_waker);
    if (tid < 0) {
        puts("futextimeout: spawn failed");
        return;
    }
    rc = futex_wait_timeout(&futex_timeout_word, 0, 1000);
    join(tid);
    printf("futextimeout: wake %d word %d\n", rc, futex_timeout_word);
}

static void cmd_futexcanceltest(void) {
    char *argv[1] = { "/bin/futexhold" };
    int killed = 0;
    for (int i = 0; i < 34; i++) {
        int pid = spawn_process_args("/bin/futexhold", argv, 1, 1);
        if (pid < 0) {
            printf("futexcancel: spawn failed at %d\n", i);
            return;
        }
        sleep_ms(20);
        if (kill(pid) < 0) {
            printf("futexcancel: kill failed pid %d\n", pid);
            return;
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            printf("futexcancel: wait failed pid %d\n", pid);
            return;
        }
        killed++;
    }

    futex_timeout_word = 0;
    int rc = futex_wait_timeout(&futex_timeout_word, 0, 10);
    printf("futexcancel: killed %d wait %d\n", killed, rc);
}

static void execute(char *line) {
    trim_right(line);
    if (!line[0])
        return;
    if (execute_pipeline(line))
        return;
    if (strcmp(line, "help") == 0 || starts_with(line, "help ")) cmd_help(line + 4);
    else if (starts_with(line, "ls ")) cmd_ls(line + 3);
    else if (starts_with(line, "ls")) cmd_ls("");
    else if (starts_with(line, "cd ")) {
        if (chdir(line + 3) < 0) puts("cd: not a directory");
    } else if (starts_with(line, "cd")) {
        if (chdir("/") < 0) puts("cd: failed");
    } else if (starts_with(line, "pwd")) cmd_pwd();
    else if (starts_with(line, "about")) cmd_about();
    else if (starts_with(line, "health")) cmd_health();
    else if (starts_with(line, "interfaces")) cmd_interfaces();
    else if (starts_with(line, "limits")) cmd_limits();
    else if (starts_with(line, "fsinfo")) cmd_fsinfo();
    else if (starts_with(line, "fsstat")) cmd_fsstat();
    else if (starts_with(line, "fdstat")) cmd_fdstat();
    else if (starts_with(line, "stat ")) cmd_stat(line + 5);
    else if (starts_with(line, "cat ")) cmd_cat(line + 4);
    else if (starts_with(line, "ping ")) cmd_ping(line + 5);
    else if (starts_with(line, "wget ")) cmd_sockget(line + 5);
    else if (starts_with(line, "tcptwotest ")) cmd_tcptwotest(line + 11);
    else if (starts_with(line, "tcptwotest")) cmd_tcptwotest("");
    else if (starts_with(line, "dhcp")) cmd_dhcp();
    else if (starts_with(line, "netstat")) cmd_netstat();
    else if (starts_with(line, "syncstat")) cmd_syncstat();
    else if (starts_with(line, "badptrtest")) cmd_badptrtest();
    else if (starts_with(line, "elfbadtest")) cmd_elfbadtest();
    else if (starts_with(line, "pipetest")) cmd_pipetest();
    else if (starts_with(line, "pipeedgetest")) cmd_pipeedgetest();
    else if (starts_with(line, "pipeblocktest")) cmd_pipeblocktest();
    else if (starts_with(line, "futextest")) cmd_futextest();
    else if (starts_with(line, "futexblocktest")) cmd_futexblocktest();
    else if (starts_with(line, "futextimeouttest")) cmd_futextimeouttest();
    else if (starts_with(line, "futexcanceltest")) cmd_futexcanceltest();
    else if (starts_with(line, "threadreusetest")) cmd_threadreusetest();
    else if (starts_with(line, "threads")) cmd_threads();
    else if (starts_with(line, "mkdir ")) {
        if (mkdir(line + 6) < 0) puts("mkdir: failed");
    } else if (starts_with(line, "rmdir ")) {
        if (rmdir(line + 6) < 0) puts("rmdir: failed");
    } else if (starts_with(line, "touch ")) {
        if (create(line + 6) < 0) puts("touch: failed");
    } else if (starts_with(line, "write ")) cmd_write_file(line + 6);
    else if (starts_with(line, "rm ")) {
        if (unlink(line + 3) < 0) puts("rm: failed");
    } else if (starts_with(line, "mv ")) {
        char local[128];
        int n = 0;
        while (line[3 + n] && n < (int)sizeof(local) - 1) {
            local[n] = line[3 + n];
            n++;
        }
        local[n] = 0;
        char *argv[2];
        if (split_args(local, argv, 2) != 2 || rename(argv[0], argv[1]) < 0)
            puts("mv: failed");
    }
    else if (starts_with(line, "nano ")) cmd_nano(line + 5);
    else if (starts_with(line, "nano")) cmd_nano("");
    else if (starts_with(line, "basm ")) cmd_basm(line + 5);
    else if (starts_with(line, "basm")) cmd_basm("");
    else if (starts_with(line, "apps ")) cmd_apps(line + 5);
    else if (starts_with(line, "apps")) cmd_apps("");
    else if (starts_with(line, "gui ")) cmd_gui(line + 3);
    else if (starts_with(line, "gui")) cmd_gui("");
    else if (starts_with(line, "exec ")) cmd_exec(line + 5);
    else if (starts_with(line, "wait ")) cmd_wait(line + 5);
    else if (starts_with(line, "wait")) cmd_wait("");
    else if (starts_with(line, "kill ")) {
        if (kill(parse_u32(line + 5)) < 0) puts("kill: failed");
    } else if (starts_with(line, "ps ")) cmd_ps(line + 3);
    else if (starts_with(line, "ps")) cmd_ps("");
    else if (starts_with(line, "echo ")) puts(line + 5);
    else if (starts_with(line, "sleep ")) sleep_ms((unsigned int)parse_u32(line + 6) * 1000u);
    else if (starts_with(line, "reboot")) reboot();
    else run_external_command(line);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    puts("=== BuzzOS User Shell ===");
    char line[LINE_MAX];
    for (;;) {
        prompt();
        read_line(line, sizeof(line));
        trim_right(line);
        history_add(line);
        execute(line);
    }
}
