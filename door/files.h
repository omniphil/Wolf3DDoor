/* files.h -- the player's own Wolfenstein 3D files, kept on the BBS, one set per player; see files.c. */

#ifndef FILES_H
#define FILES_H

#include <stdbool.h>
#include <stddef.h>

/* Bytes of file per message. Base64 plus the TRACE wrapper has to stay under the terminal's 8 KB command limit.
 * The module sends pieces of the same size (module/src/file_trace.c CHUNK): the two must agree. */
#define FILES_CHUNK 3000

/* Picks the folder for this player (their handle and BBS user number, from the drop file) and makes sure it exists. */
void files_init(const char *player, int user_number);

/* ANSI mode keeps a config of its own (see files.c). Call before files_send_all. */
void files_use_ansi_config(bool on);

/* The folder where this player's files are kept (the door keeps its display choice there too) */
const char *files_folder(void);

/* The folder name this player's files are kept under, for the door to show them. */
const char *files_player(void);

/* Sends the game every file this player has, in pieces. */
void files_send_all(void (*send)(const char *head, const void *payload, size_t len));

/* Takes a piece of a file from the game. Returns 1 when the last piece completes the file and it has been kept. */
int files_receive_chunk(const char *name, size_t offset, size_t total, const unsigned char *data, size_t size);

#endif
