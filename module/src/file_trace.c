/*
 * file_trace.c -- Wolf4SDL's files, without a file system. Every fopen/fclose/stat/unlink the game makes comes here
 * (include/wolftrace_compat.h). Folders in a path are ignored and names are matched in lower case.
 *
 * There are two kinds of file:
 *   - The game data (vswap.wl1, gamemaps.wl1, ...), read-only, from the pack the door sent as an asset (wolftrace.c).
 *   - The player's own files: config.wl1 (settings, key bindings, high scores) and savegam0-9.wl1. These live on the
 *     BBS, per player. The door sends them at the start; whenever the game writes one, the new contents go back up a
 *     piece at a time (wolftrace_user_files_pump), so they're safe even if the call drops.
 *
 * Reading is fmemopen over the bytes, writing is open_memstream: real stdio streams, so the game's own fread/fwrite/
 * fseek calls work on them unchanged.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trace_api.h"
#include "wolftrace.h"

#undef fopen
#undef fclose
#undef stat
#undef unlink

/* ---- the player's files ---- */

#define CHUNK 3000   /* bytes per message to the door, header and all well under TRACE_SEND_MAX; see door/files.c */

typedef struct
{
    const char *name;
    uint8_t    *data;       /* what the game sees */
    size_t      size;
    int         present;    /* the player has this file (the door sent it, or the game wrote it) */

    /* receiving from the door */
    uint8_t    *incoming;
    size_t      incoming_total, incoming_have;

    /* sending to the door: a snapshot, so the game can write again while an older copy is going up */
    uint8_t    *outgoing;
    size_t      outgoing_size, outgoing_sent;
    int         outgoing_busy;
} user_file_t;

/* Only these are kept, and only these may go to the BBS: the door accepts nothing else either (door/files.c) */
static user_file_t g_user_files[] =
{
    { "config.wl1" },
    { "savegam0.wl1" }, { "savegam1.wl1" }, { "savegam2.wl1" }, { "savegam3.wl1" }, { "savegam4.wl1" },
    { "savegam5.wl1" }, { "savegam6.wl1" }, { "savegam7.wl1" }, { "savegam8.wl1" }, { "savegam9.wl1" },
};
#define USER_FILE_COUNT (sizeof(g_user_files) / sizeof(g_user_files[0]))

static user_file_t *find_user_file(const char *name)
{
    for (size_t i = 0; i < USER_FILE_COUNT; i++)
        if (strcmp(g_user_files[i].name, name) == 0)
            return &g_user_files[i];
    return NULL;
}

/* A piece of one of the player's files from the door. Pieces come in order; the file is only used once it's whole. */
void wolftrace_user_file_received(const char *name, size_t off, size_t total, const uint8_t *data, size_t len)
{
    user_file_t *uf = find_user_file(name);

    if (uf == NULL || total > 1024 * 1024)
        return;
    if (off == 0)
    {
        free(uf->incoming);
        uf->incoming = (uint8_t *)malloc(total > 0 ? total : 1);
        uf->incoming_total = total;
        uf->incoming_have = 0;
    }
    if (uf->incoming == NULL || off != uf->incoming_have || off + len > uf->incoming_total)
    {
        wolftrace_log("wolf3d: a piece of %s arrived out of order; ignoring it", name);
        free(uf->incoming);
        uf->incoming = NULL;
        return;
    }
    memcpy(uf->incoming + off, data, len);
    uf->incoming_have += len;
    if (uf->incoming_have == uf->incoming_total)
    {
        free(uf->data);
        uf->data = uf->incoming;
        uf->size = uf->incoming_total;
        uf->present = 1;
        uf->incoming = NULL;
        wolftrace_log("wolf3d: %s from the BBS, %zu bytes", name, uf->size);
    }
}

const uint8_t *wolftrace_user_file(const char *name, size_t *size)
{
    user_file_t *uf = find_user_file(name);
    if (uf == NULL || !uf->present)
        return NULL;
    *size = uf->size;
    return uf->data;
}

/* The game wrote a file: keep it, and send it to the BBS unless it's what the BBS already has */
void wolftrace_user_file_written(const char *name, const uint8_t *data, size_t size)
{
    user_file_t *uf = find_user_file(name);
    uint8_t *copy;

    if (uf == NULL)
        return;
    if (uf->present && uf->size == size && (size == 0 || memcmp(uf->data, data, size) == 0))
        return;

    copy = (uint8_t *)malloc(size > 0 ? size : 1);
    if (copy == NULL)
        return;
    memcpy(copy, data, size);
    free(uf->data);
    uf->data = copy;
    uf->size = size;
    uf->present = 1;

    /* The newest copy replaces one still on its way: the door starts again when a piece at offset 0 arrives */
    free(uf->outgoing);
    uf->outgoing = (uint8_t *)malloc(size > 0 ? size : 1);
    if (uf->outgoing == NULL)
    {
        uf->outgoing_busy = 0;
        return;
    }
    memcpy(uf->outgoing, data, size);
    uf->outgoing_size = size;
    uf->outgoing_sent = 0;
    uf->outgoing_busy = 1;
}

/*
 * Sends what the link will take right now:
 *   put name=<n> off=<o> total=<t>\n<bytes>
 * Returns 1 while anything is still waiting to go.
 */
int wolftrace_user_files_pump(void)
{
    static uint8_t message[CHUNK + 128];
    int waiting = 0;

    for (size_t i = 0; i < USER_FILE_COUNT; i++)
    {
        user_file_t *uf = &g_user_files[i];

        while (uf->outgoing_busy)
        {
            size_t len = uf->outgoing_size - uf->outgoing_sent;
            int head;

            if (len > CHUNK)
                len = CHUNK;
            head = snprintf((char *)message, 128, "put name=%s off=%zu total=%zu\n",
                            uf->name, uf->outgoing_sent, uf->outgoing_size);
            memcpy(message + head, uf->outgoing + uf->outgoing_sent, len);
            if (trace_send_room() < head + (int)len || trace_send(message, head + (int32_t)len) <= 0)
                break;      /* the link is busy: the rest goes on a later call */

            uf->outgoing_sent += len;
            if (uf->outgoing_sent >= uf->outgoing_size)
            {
                wolftrace_log("wolf3d: %s saved to the BBS, %zu bytes", uf->name, uf->outgoing_size);
                free(uf->outgoing);
                uf->outgoing = NULL;
                uf->outgoing_busy = 0;
            }
        }
        waiting |= uf->outgoing_busy;
    }
    return waiting;
}

/* ---- the calls the game makes ---- */

/* The file's own name, lower case: "user/SAVEGAM0.WL1" -> "savegam0.wl1" */
static void leaf(const char *path, char *out, size_t size)
{
    const char *slash = strrchr(path, '/');
    size_t i = 0;

    if (slash != NULL)
        path = slash + 1;
    for (; path[i] && i + 1 < size; i++)
        out[i] = (char)tolower((unsigned char)path[i]);
    out[i] = 0;
}

/* Files being written: open_memstream keeps the bytes, and they become the player's file when it's closed */
typedef struct
{
    FILE       *f;
    char       *buffer;
    size_t      size;
    const char *user_name;
} writer_t;

static writer_t g_writers[4];

FILE *wolftrace_fopen(const char *path, const char *mode)
{
    char name[64];
    const uint8_t *data;
    size_t size = 0;

    leaf(path, name, sizeof(name));
    if (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL)
    {
        user_file_t *uf = find_user_file(name);
        writer_t *w = NULL;

        if (uf == NULL)
        {
            errno = EACCES;     /* a screenshot or a demo: there's nowhere to keep it */
            return NULL;
        }
        for (size_t i = 0; i < sizeof(g_writers) / sizeof(g_writers[0]); i++)
            if (g_writers[i].f == NULL)
            {
                w = &g_writers[i];
                break;
            }
        if (w == NULL)
        {
            errno = EMFILE;
            return NULL;
        }
        w->buffer = NULL;
        w->size = 0;
        w->f = open_memstream(&w->buffer, &w->size);
        w->user_name = uf->name;
        return w->f;
    }

    data = wolftrace_data_file(name, &size);
    if (data == NULL)
        data = wolftrace_user_file(name, &size);
    if (data == NULL || size == 0)      /* fmemopen won't take an empty buffer, and an empty file is no use */
    {
        errno = ENOENT;
        return NULL;
    }
    return fmemopen((void *)data, size, "rb");
}

/* Closing a file the game wrote is what sends it to the BBS: only whole, successfully written files go */
int wolftrace_fclose(FILE *f)
{
    writer_t *w = NULL;
    int result;

    if (f == NULL)
        return EOF;
    for (size_t i = 0; i < sizeof(g_writers) / sizeof(g_writers[0]); i++)
        if (g_writers[i].f == f)
            w = &g_writers[i];
    result = fclose(f);
    if (w != NULL)
    {
        if (result == 0)
            wolftrace_user_file_written(w->user_name, (const uint8_t *)w->buffer, w->size);
        free(w->buffer);
        w->buffer = NULL;
        w->f = NULL;
    }
    return result;
}

/* The game checks the data files are there (which episode is this?) and that its config folder exists */
int wolftrace_stat(const char *path, struct stat *buf)
{
    char name[64];
    size_t size = 0;

    leaf(path, name, sizeof(name));
    memset(buf, 0, sizeof(*buf));
    if (!strcmp(name, "user") || (path[0] && path[strlen(path) - 1] == '/'))
    {
        buf->st_mode = S_IFDIR | 0755;      /* the config folder (--configdir user) */
        return 0;
    }
    if (wolftrace_data_file(name, &size) != NULL || wolftrace_user_file(name, &size) != NULL)
    {
        buf->st_mode = S_IFREG | 0644;
        buf->st_size = (off_t)size;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

/* A save being overwritten is deleted first; the new one follows at once, so only this copy forgets it */
int wolftrace_unlink(const char *path)
{
    char name[64];
    user_file_t *uf;

    leaf(path, name, sizeof(name));
    uf = find_user_file(name);
    if (uf == NULL || !uf->present)
    {
        errno = ENOENT;
        return -1;
    }
    uf->present = 0;
    return 0;
}
