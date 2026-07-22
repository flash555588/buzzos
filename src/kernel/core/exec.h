#ifndef BUZZOS_EXEC_H
#define BUZZOS_EXEC_H

#include <stddef.h>
#include <stdint.h>

/* Start an ELF as a separate user process. Returns task id or -1. */
int exec_start(const uint8_t *elf_data, size_t elf_size, const char *name, int console_silent);
int exec_start_args(const uint8_t *elf_data, size_t elf_size, const char *name,
                    int console_silent, int argc, const char *const argv[]);
int exec_start_args_with_fds(const uint8_t *elf_data, size_t elf_size, const char *name,
                             int console_silent, int argc, const char *const argv[],
                             int inherit_fd_owner, int inherit_stdio_only);
int exec_start_file_args_with_fds(int fd, size_t elf_size, const char *name,
                                  int console_silent, int argc,
                                  const char *const argv[], int inherit_fd_owner,
                                  int inherit_stdio_only);

/* Start an ELF process and wait for it to exit. Returns its exit code. */
int exec_elf(const uint8_t *elf_data, size_t elf_size);

#endif /* BUZZOS_EXEC_H */
