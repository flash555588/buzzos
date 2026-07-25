#ifndef BUZZOS_STDIO_COMPAT_H
#define BUZZOS_STDIO_COMPAT_H

#include "libc.h"

#define EOF (-1)
#define BUFSIZ 1024
#define getc(stream) fgetc(stream)
#define putc(character, stream) fputc((character), (stream))
#define getchar() fgetc(stdin)
#define putchar_stdio(character) fputc((character), stdout)

typedef struct buzz_file {
    int fd;
    int eof;
    int error;
    int have_unget;
    int unget_char;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define L_tmpnam 32
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fgetc(FILE *stream);
int fputc(int character, FILE *stream);
int fputs(const char *text, FILE *stream);
char *fgets(char *buffer, int size, FILE *stream);
int ungetc(int character, FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fflush(FILE *stream);
void setbuf(FILE *stream, char *buffer);
int setvbuf(FILE *stream, char *buffer, int mode, size_t size);
int fprintf(FILE *stream, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, __builtin_va_list args);
int vsnprintf(char *buffer, size_t size, const char *fmt,
              __builtin_va_list args);
int sprintf(char *buffer, const char *fmt, ...);
int sscanf(const char *text, const char *fmt, ...);
int fscanf(FILE *stream, const char *fmt, ...);
int remove(const char *path);
char *tmpnam(char *buffer);
FILE *tmpfile(void);
void perror(const char *prefix);

#endif
