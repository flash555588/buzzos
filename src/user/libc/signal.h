#ifndef BUZZOS_SIGNAL_COMPAT_H
#define BUZZOS_SIGNAL_COMPAT_H

typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);

#define SIGINT 2
#define SIGILL 4
#define SIGFPE 8
#define SIGBUS 10
#define SIGSEGV 11
#define SIGPIPE 13
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

sighandler_t signal(int number, sighandler_t handler);

#endif
