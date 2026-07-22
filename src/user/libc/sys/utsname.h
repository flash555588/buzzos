#ifndef BUZZOS_SYS_UTSNAME_COMPAT_H
#define BUZZOS_SYS_UTSNAME_COMPAT_H

struct utsname {
    char sysname[32];
    char nodename[32];
    char release[32];
    char version[32];
    char machine[32];
};

int uname(struct utsname *name);

#endif
