/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Career import: moves the career that the online game keeps on the console
 * (a user's sce_sdmemory save) into the Offline Mode save, with a backup of
 * the Offline Mode save first. Nothing runs until the page asks for it.
 *
 * Each call returns an HTTP status and sets *out to a malloc'd reply: JSON
 * for 200, otherwise a message for the page. The caller frees it.
 */
#pragma once

#include <stddef.h>

void career_init(int (*game_running)(void));
int career_state(int rescan, char **out);
int career_move(const char *body, char **out);
int career_restore(const char *body, char **out);
int career_delete(const char *body, char **out);
int career_remove_added(const char *body, char **out);

/* For streaming: the file a download names, and where an upload goes (write
 * it to part, then rename part to path and call career_upload_done). */
int career_download_path(const char *query, char *path, size_t len, char *name, size_t namelen);
int career_upload_path(const char *query, char *path, size_t len, char *part, size_t partlen);
void career_upload_done(const char *path, size_t size);
