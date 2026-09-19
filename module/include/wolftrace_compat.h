/*
 * wolftrace_compat.h -- forced into every file with -include, so Wolf4SDL's sources build unchanged.
 *
 *   exit()                    Wolf4SDL leaves with exit() (Quit, fatal errors). In the sandbox that has to close
 *                             TERMinator's picture and tell the door, so it goes to wolftrace_exit (src/wolftrace.c).
 *   fopen/fclose/stat/unlink  There is no file system. The game's data comes from the pack the door sent and the
 *                             player's config and saves live on the BBS, so every file goes through src/file_trace.c.
 *   mkdir                     The config folder only has to seem to exist.
 */

#ifndef WOLFTRACE_COMPAT_H
#define WOLFTRACE_COMPAT_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

void  wolftrace_exit(int code) __attribute__((noreturn));
FILE *wolftrace_fopen(const char *path, const char *mode);
int   wolftrace_fclose(FILE *f);
int   wolftrace_stat(const char *path, struct stat *buf);
int   wolftrace_unlink(const char *path);

#ifdef __cplusplus
}
#endif

#define exit(code)     wolftrace_exit(code)
#define fopen          wolftrace_fopen
#define fclose         wolftrace_fclose
#define stat(p, b)     wolftrace_stat(p, b)
#define unlink         wolftrace_unlink
#define fs_unlink      wolftrace_unlink
#define mkdir(p, m)    0

#endif
