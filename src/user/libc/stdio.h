#ifndef BUZZOS_STDIO_COMPAT_H
#define BUZZOS_STDIO_COMPAT_H

#include "libc.h"

#define EOF (-1)

typedef struct buzz_file {
    int fd;
    int eof;
    int error;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fgetc(FILE *stream);
int fputc(int character, FILE *stream);
int fputs(const char *text, FILE *stream);
char *fgets(char *buffer, int size, FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
int fflush(FILE *stream);
void setbuf(FILE *stream, char *buffer);
int fprintf(FILE *stream, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, __builtin_va_list args);
int vsnprintf(char *buffer, size_t size, const char *fmt,
              __builtin_va_list args);
int sprintf(char *buffer, const char *fmt, ...);
int sscanf(const char *text, const char *fmt, ...);
int fscanf(FILE *stream, const char *fmt, ...);
int remove(const char *path);
void perror(const char *prefix);

#endif
